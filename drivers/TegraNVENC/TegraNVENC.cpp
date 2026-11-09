// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 NVENC (video encoder 5.0, a falcon behind host1x): bring-up, the job engine and sleep. tegra_nvenc_probe()
// runs the whole bring-up by default on its own thread:
//   E0  host1x (drivers/TegraHost1x, shared with NVDEC)
//   E1  NVENC's SMMU address space (TegraNVENCMemory.cpp), power partition, clock, reset, falcon firmware loaded and
//       started
//   E2  proof of life through host1x channel 0 in NVENC's class: an IMMEDIATE syncpoint increment (the engine's host
//       interface answers) then an OP_DONE one (the falcon firmware answers), each completed by interrupt
//   E3  the TegraNVENC service and its user client published (TegraNVENCUserClient.cpp)
// One line per stage ("TegraNVENC: E1 ok ..."), or the failing stage's FAIL line and nothing after it. Until E3
// publishes, every failure is that one line: the engine is put back in reset with its clock off, its address space is
// given back and nothing is published, so VideoToolbox has no hardware encoder. From E3 on a job past its bound panics with
// one line, and a lost syncpoint interrupt panics in TegraHost1x. switch-no-nvdec turns it off.
//
// Input pictures are 16x16 tiles with chroma at the surface config's offset (TegraNVENCInfo): the layout and chroma
// reading the unit encoded IPCM and IDR pictures from exactly on every boot of 11-06 (r30 archives, "E3 IPCM ok input
// layout tiled 16x16, chroma offset").
//
// Jobs (tegra_nvenc_run): host1x channel 0 in NVENC's class (0x21), each method written through the engine's THI
// METHOD0/METHOD1 pair as NVDEC's, the picture setup from a kernel-only slot, then an OP_DONE increment of NVENC's
// syncpoint: the job's fence. Every address method the firmware knows is written on every job (the submit's own, 0, or
// the scratch buffer), so no job inherits an address from another client's. At most 4 jobs are queued, which bounds
// the push buffer NVENC shares with NVDEC (8 x 197 + 4 x 190 words < 4096) and the 8 setup slots. Every job runs with
// the GP timer on (the checker requires it, 1 << 27 cycles at most, 329 ms at 408 MHz); waits are bounded by
// kJobBoundUs per queued job whatever a client asks for, and the largest picture the checker admits (36864 macroblocks)
// encodes well inside it. The GP timer does not end a stalled job: its interrupt only records an error for the H.264
// app, whose end-of-picture spin waits for macroblocks the engine never reports (appDump), so the kernel's bound is
// what ends it.
//
// Firmware: linux-firmware's nvidia/tegra210/nvenc.bin, byte-identical to L4T's nvhost_nvenc050.fw (firmware/SOURCE),
// booted non-securely as L4T's nvhost and TegraNVDEC boot theirs.
//
// Sources (Linux's drivers are GPL-2.0-only, so this follows them without copying):
//   tegra210.dtsi nvenc@544c0000 (0x40000; CAR clock and reset 219; power domain pd_nvenc = PMC partition "mpe", 6),
//   clk-tegra-periph.c (CLK_SOURCE_NVENC 0x6a0, the NVDEC/NVJPG mux: PLLP is code 4), clk-tegra210.c
//   tegra210_pg_mbist_war[TEGRA_POWERGATE_MPE] (LVL2_CLK_GATE_OVRE bit 29 pulsed, no slcg clock),
//   memory/tegra/tegra210.c (MC client hot reset NVENC: control 0x200 bit 11; SMMU ASID NVENC 0x264),
//   include/linux/host1x.h (HOST1X_CLASS_NVENC 0x21), soc/tegra/pmc.c tegra_powergate_power_up, drm/tegra/falcon.c
//   falcon_boot (shared with TegraNVDEC.cpp), L4T nvhost t210.c (NVENC 5.0, nvhost_nvenc050.fw).
//   Clock: PLLP undivided, 408 MHz, the rate NVDEC runs at (the T210B01 core DVFS table gives NVENC and NVDEC the same
//   points).
// Waits on hardware sequencing with no interrupt of its own (partition status, IMEM/DMEM scrub, the falcon's DMA
// queue) are bounded reads, as in TegraNVDEC; every completion of work is a host1x syncpoint interrupt.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../kits/SwitchBootArgs/SwitchBootArgs.h"
#include "TegraNVENC.h"
#include "TegraNVENCCheck.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
extern const uint8_t tegra_nvenc_fw_start[] __asm__("_tegra_nvenc_fw_start");
extern const uint8_t tegra_nvenc_fw_end[] __asm__("_tegra_nvenc_fw_end");
}

namespace {
constexpr uint64_t kNvencPA = 0x544c0000, kNvencSize = 0x40000;
constexpr uint32_t kClass = TEGRA_HOST1X_CLASS_NVENC;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data)
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kPartition = 6;  // TEGRA_POWERGATE_MPE
constexpr unsigned kClock = 219;    // TEGRA210_CLK_NVENC: CAR bank Y bit 27, also its reset

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_Y = 0x298,
    RST_DEVICES_Y = 0x2a4,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_NVENC = 0x6a0,
    OVRE_MPE_MBIST = 1u << 29,
    SOURCE_PLLP = 4u << 29,
    NVENC_DIV_1 = 0,  // 7.1 divider: 408 MHz * 2 / (0 + 2) = 408 MHz
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    PWRGATE_STATUS = 0x38,
};

enum : uint32_t {
    MC_HOTRESET_CTRL = 0x200,
    MC_HOTRESET_NVENC = 1u << 11,
};

enum : uint32_t {
    ICD_RREG = 0x8,  // icd_cmd opc rreg; register index << 8: SP 20, PC 21 (nvgpu falcon.h FALCON_REG_*)
    THI_INT_STATUS = 0x78,
    FALCON_IO = 0x1000,           // the falcon's I/O address 0 (I[0x8] is IRQSTAT at 0x1008)
    ENCODER_INT_STATUS = 0x1870,  // I/O; bit 0 error, bit 1 macroblocks done, write 1 to clear (the H.264 app's ISR)
};

// The H.264 app's (app 1) DMEM words for the end of a picture, read with tools/falcon: the error its ISR (0xf1b)
// records, the macroblocks it counts (+= I[0x1848] per encoder interrupt) and the count fn 0x48b9 spins at 0x4916 until
// (the slice's num_mb, at most the picture's). H.265's app keeps its count and target at 0x360 and 0x38c.
enum : uint32_t {
    APP_ERROR = 0x2c8,
    APP_MBS_DONE = 0x2e4,
    APP_MBS_TARGET = 0x354,
};

volatile uint8_t *gNvenc, *gMc;
switch_dma_buffer gFirmware;  // stays: the falcon OS loads its apps from it through DMATRFBASE
switch_falcon_image gImage;
char gLine[256];

// Address methods of features no submit turns on, written 0 on every job: the value an instance starts with and the
// one NVIDIA's own T210 driver leaves them at (its captured streams set none of them). The firmware takes a non-zero
// address as the feature's data being there; 0 also means no job keeps another client's address in them.
constexpr uint16_t kUnusedMethods[] = {
    NVC5B7_SET_IN_RCDATA, NVC5B7_SET_IN_CEAHINTS_DATA, NVC5B7_SET_IN_COLOC_DATA,
    NVC5B7_SET_IN_QP_MAP, NVC5B7_SET_IN_PARTITION_BUF,
};

// The job engine. kJobBoundUs: the GP timer's 329 ms plus margin; a wait covers at most kInFlight queued jobs.
constexpr uint32_t kInFlight = 4, kSetupSlots = 8, kSetupSlotBytes = TEGRA_NVENC_MAX_SETUP, kSinkBytes = 256 * 1024;
constexpr uint32_t kJobBoundUs = 500000, kWaitBoundUs = kInFlight * kJobBoundUs;
IOLock *gEngineLock;
uint32_t gSyncpt, gBase, gIssued;  // job n's fence is gBase + n
bool gUp, gWedged, gSuspended;
switch_dma_buffer gSetup, gSink;  // setup slots and the scratch buffer, never mapped into a client

const char *falconState()
{
    static char text[160];
    return switch_falcon_state(gNvenc, text, sizeof(text));
}

const char *falconRunState(uint32_t cpuctl)
{
    if (cpuctl & 0x10)
        return "halted";
    return (cpuctl & 0x20) ? "stopped" : "running";
}

// A falcon that did not finish, for its one line (nvgpu gk20a_falcon_dump_stats): CPUCTL (bit 4 halted, bit 5
// stopped, i.e. waiting for work), IDLESTATE (bit 0 the falcon, bits 1.. engine units busy), interrupts, mailboxes,
// the exception register, the falcon's DMA queue and its external-error capture, the OS's version word, the THI
// interrupt status, the last branch targets in the trace buffer (newest first), and PC and SP read three times through
// the in-circuit debugger (the same PC three times: a tight wait loop or a stall; different: still running code). The
// debugger is only used on a falcon whose fault is about to panic the system.
const char *falconDump()
{
    static char text[512];
    const uint32_t cpuctl = switch_mmio_read32(gNvenc, SWITCH_FALCON_CPUCTL);
    int n = snprintf(
        text, sizeof(text),
        "falcon CPUCTL %#x (%s) IDLESTATE %#x IRQSTAT %#x MAILBOX %#x %#x EXCI %#x OS %#x DEBUGINFO %#x "
        "DMATRFCMD %#x EXTERR %#x@%#x THI_INT %#x",
        cpuctl, falconRunState(cpuctl), switch_mmio_read32(gNvenc, SWITCH_FALCON_IDLESTATE),
        switch_mmio_read32(gNvenc, SWITCH_FALCON_IRQSTAT), switch_mmio_read32(gNvenc, SWITCH_FALCON_MAILBOX0),
        switch_mmio_read32(gNvenc, SWITCH_FALCON_MAILBOX1), switch_mmio_read32(gNvenc, SWITCH_FALCON_EXCI),
        switch_mmio_read32(gNvenc, SWITCH_FALCON_OS), switch_mmio_read32(gNvenc, SWITCH_FALCON_DEBUGINFO),
        switch_mmio_read32(gNvenc, SWITCH_FALCON_DMATRFCMD), switch_mmio_read32(gNvenc, SWITCH_FALCON_EXTERRSTAT),
        switch_mmio_read32(gNvenc, SWITCH_FALCON_EXTERRADDR), switch_mmio_read32(gNvenc, THI_INT_STATUS));
    const uint32_t traced = (switch_mmio_read32(gNvenc, SWITCH_FALCON_TRACEIDX) >> 16) & 0xff;
    n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " trace(%u)", traced);
    for (uint32_t i = 0; i < traced && i < 6; i++) {
        switch_mmio_write32(gNvenc, SWITCH_FALCON_TRACEIDX, i);
        n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " %#x",
                      switch_mmio_read32(gNvenc, SWITCH_FALCON_TRACEPC) & 0xffffff);
    }
    n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " PC/SP");
    for (int i = 0; i < 3; i++) {
        switch_mmio_write32(gNvenc, SWITCH_FALCON_ICD_CMD, ICD_RREG | 21u << 8);
        const uint32_t pc = switch_mmio_read32(gNvenc, SWITCH_FALCON_ICD_RDATA);
        switch_mmio_write32(gNvenc, SWITCH_FALCON_ICD_CMD, ICD_RREG | 20u << 8);
        n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " %#x/%#x", pc,
                      switch_mmio_read32(gNvenc, SWITCH_FALCON_ICD_RDATA));
    }
    return text;
}

uint32_t dmem(uint32_t address)
{
    switch_mmio_write32(gNvenc, SWITCH_FALCON_DMEMC, address);
    return switch_mmio_read32(gNvenc, SWITCH_FALCON_DMEMD);
}

// Which way the H.264 app is stuck. An error (0xe2640010 GP timer, 0xe2640060 encoder error, 0xe2640200 I[0x1ac4]
// bits 0-2) with fewer macroblocks than the target: the ISR saw the fault but the spin has no way out. No error and
// fewer: the engine stopped reporting macroblocks, or its interrupt is pending untaken (encoder status bit 1 set).
// Count at the target: past the spin, and the PC shows which of the polls after it holds the app.
const char *appDump()
{
    static char text[160];
    snprintf(text, sizeof(text), "H.264 app error %#x macroblocks %u of %u encoder status %#x (I[%#x] at +%#x)",
             dmem(APP_ERROR), dmem(APP_MBS_DONE), dmem(APP_MBS_TARGET),
             switch_mmio_read32(gNvenc, FALCON_IO + ENCODER_INT_STATUS), ENCODER_INT_STATUS,
             FALCON_IO + ENCODER_INT_STATUS);
    return text;
}

// tegra_powergate_power_up with the T210 MBIST workaround for the MPE partition (no slcg clock to hold on). A
// partition the boot chain left on keeps its state; the engine is still reset and re-clocked so the falcon starts
// from scratch.
bool powerUp(bool *wasOn)
{
    *wasOn = switch_pmc_partition_is_on(kPartition);
    switch_car_reset_assert(kClock);
    IODelay(10);
    if (!*wasOn) {
        if (!switch_pmc_partition_on(kPartition)) {
            snprintf(gLine, sizeof(gLine), "E1 FAIL partition %u did not power on (PWRGATE_TOGGLE %#x STATUS %#x)",
                     kPartition, switch_pmc_read(PWRGATE_TOGGLE), switch_pmc_read(PWRGATE_STATUS));
            return false;
        }
        IODelay(10);
    }
    switch_car_write(CLK_SOURCE_NVENC, SOURCE_PLLP | NVENC_DIV_1);
    IODelay(2);
    switch_car_clock_enable(kClock);
    IODelay(10);
    switch_pmc_remove_clamping(kPartition);
    IODelay(10);
    if (switch_mmio_read32(gMc, MC_HOTRESET_CTRL) & MC_HOTRESET_NVENC)
        switch_mmio_write32(gMc, MC_HOTRESET_CTRL, switch_mmio_read32(gMc, MC_HOTRESET_CTRL) & ~MC_HOTRESET_NVENC);
    switch_car_reset_deassert(kClock);
    IODelay(10);
    // tegra210_generic_mbist_war: only MPE's override bit changes, under the CAR lock.
    const uint32_t overridden = switch_car_read(LVL2_CLK_GATE_OVRE) & OVRE_MPE_MBIST;
    switch_car_rmw(LVL2_CLK_GATE_OVRE, 0, OVRE_MPE_MBIST);
    IODelay(1);
    switch_car_rmw(LVL2_CLK_GATE_OVRE, OVRE_MPE_MBIST & ~overridden, 0);
    IODelay(1);
    if (!switch_car_clock_enabled(kClock) || switch_car_reset_asserted(kClock)) {
        snprintf(gLine, sizeof(gLine), "E1 FAIL clock or reset stuck (CLK_OUT_ENB_Y %#x RST_DEVICES_Y %#x)",
                 switch_car_read(CLK_OUT_ENB_Y), switch_car_read(RST_DEVICES_Y));
        return false;
    }
    return true;
}

// The whole image goes into one contiguous buffer below 4 GiB, cleaned to DRAM: the falcon fetches it by physical
// address (SMMU bypassed), now for its OS and later for the apps.
bool loadImage()
{
    const size_t length =
        reinterpret_cast<uintptr_t>(tegra_nvenc_fw_end) - reinterpret_cast<uintptr_t>(tegra_nvenc_fw_start);
    if (!switch_falcon_parse(tegra_nvenc_fw_start, length, &gImage)) {
        snprintf(gLine, sizeof(gLine), "E1 FAIL embedded firmware is not a falcon v1 image (%lu bytes)",
                 static_cast<unsigned long>(length));
        return false;
    }
    if (!gFirmware.memory) {  // kept for resume: the falcon is reloaded from it after SC7
        if (!switch_dma_alloc(&gFirmware, gImage.size)) {
            snprintf(gLine, sizeof(gLine), "E1 FAIL no contiguous %u-byte firmware buffer below 4 GiB", gImage.size);
            return false;
        }
        memcpy(gFirmware.va, tegra_nvenc_fw_start, gImage.size);
        switch_dcache_clean(gFirmware.va, gImage.size);
    }
    return true;
}

// falcon_boot, minus its final idle poll: that the firmware runs is shown by E2's OP_DONE increment instead.
bool bootFalcon()
{
    if (!loadImage())
        return false;
    switch (switch_falcon_load(gNvenc, gFirmware.pa, &gImage)) {
    case SWITCH_FALCON_SCRUBBING:
        snprintf(gLine, sizeof(gLine), "E1 FAIL IMEM/DMEM scrubbing did not end (DMACTL %#x)",
                 switch_mmio_read32(gNvenc, SWITCH_FALCON_DMACTL));
        return false;
    case SWITCH_FALCON_DMA:
        snprintf(gLine, sizeof(gLine), "E1 FAIL firmware DMA did not finish (DMATRFCMD %#x)",
                 switch_mmio_read32(gNvenc, SWITCH_FALCON_DMATRFCMD));
        return false;
    case SWITCH_FALCON_OK:
        break;
    }
    switch_falcon_start(gNvenc);
    return true;
}

// Channel 0 in NVENC's class: an IMMEDIATE increment is done by the engine's host interface on arrival, an OP_DONE
// one only once the falcon reports the methods before it done.
int answer(uint32_t id, uint32_t from, const char **stage)
{
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(kClass, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, id),
    };
    int ret = tegra_host1x_submit(stream, 3);
    *stage = "submit";
    if (!ret) {
        *stage = "host interface (IMMEDIATE)";
        ret = tegra_host1x_syncpt_wait(id, from + 1, 100000);
    }
    if (!ret) {
        *stage = "falcon (OP_DONE)";
        ret = tegra_host1x_syncpt_wait(id, from + 2, 500000);
    }
    return ret;
}

bool proofOfLife(uint64_t *us, uint32_t *id, uint32_t *from)
{
    if (tegra_host1x_syncpt_alloc(id)) {
        snprintf(gLine, sizeof(gLine), "E2 FAIL no syncpoint left");
        return false;
    }
    *from = tegra_host1x_syncpt_read(*id);
    const uint64_t began = switch_now_us();
    const char *stage = nullptr;
    const int ret = answer(*id, *from, &stage);
    *us = switch_now_us() - began;
    if (ret) {
        char host[200];
        tegra_host1x_describe(host, sizeof(host));
        snprintf(gLine, sizeof(gLine), "E2 FAIL %s %s syncpoint %u (%u->%u, %d); %s; %s", stage,
                 ret == TEGRA_HOST1X_ELOST ? "incremented, but no interrupt came for" : "did not increment", *id, *from,
                 tegra_host1x_syncpt_read(*id), ret, falconState(), host);
        return false;
    }
    return true;
}

void engineSuspend();
void engineResume();

// A fence past its bound is a hardware fault: no job runs before the service is published.
[[noreturn]] void timedOut(uint32_t fence)
{
    char host[200];
    tegra_host1x_describe(host, sizeof(host));
    panic("TegraNVENC: job fence %u not reached in %u ms (syncpoint %u at %u, GP timer bound passed); %s; %s; %s",
          fence, kWaitBoundUs / 1000, gSyncpt, tegra_host1x_syncpt_read(gSyncpt), appDump(), falconDump(), host);
}

// Back to reset with the clock off after a failed bring-up: nothing will use the engine this boot.
void park()
{
    switch_car_reset_assert(kClock);
    IODelay(2);
    switch_car_clock_disable(kClock);
}

void bringUp(void *, wait_result_t)
{
    // A failed bring-up leaves nothing behind: the engine back in reset with its clock off (once it was touched), then
    // its address space and kernel buffers returned.
    const auto giveUp = [](bool touched) {
        if (touched)
            park();
        tegra_nvenc_memory_release();
        switch_dma_free(&gSetup);
        switch_dma_free(&gSink);
    };
    if (tegra_host1x_start()) {
        IOLog("TegraNVENC: E0 FAIL host1x is not up (see the TegraHost1x line)\n");
        return;
    }
    gNvenc = switch_mmio_map(kNvencPA, kNvencSize);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gEngineLock = IOLockAlloc();
    if (!gNvenc || !gMc || !gEngineLock) {
        IOLog("TegraNVENC: E1 FAIL no mapping\n");
        return;
    }
    gLine[0] = 0;
    if (!switch_dma_alloc(&gSetup, static_cast<size_t>(kSetupSlots) * kSetupSlotBytes) ||
        !switch_dma_alloc(&gSink, kSinkBytes) || !loadImage()) {
        IOLog("TegraNVENC: %s\n", gLine[0] ? gLine : "E1 FAIL no contiguous setup slots or scratch below 4 GiB");
        giveUp(false);
        return;
    }
    if (tegra_nvenc_memory_init(&gFirmware, &gSetup, &gSink, gLine, sizeof(gLine))) {
        IOLog("TegraNVENC: %s\n", gLine);
        giveUp(false);
        return;
    }
    bool wasOn = false;
    gLine[0] = 0;
    if (!powerUp(&wasOn) || !bootFalcon()) {
        IOLog("TegraNVENC: %s\n", gLine[0] ? gLine : "E1 FAIL");
        giveUp(true);
        return;
    }
    IOLog("TegraNVENC: E1 ok partition %u %s, 408 MHz, falcon started (OS code %#x+%#x data %#x+%#x, %u apps)\n",
          kPartition, wasOn ? "already on" : "powered here", gImage.code, gImage.code_size, gImage.data,
          gImage.data_size, gImage.apps);
    uint64_t us = 0;
    uint32_t id = 0;
    uint32_t from = 0;
    if (!proofOfLife(&us, &id, &from)) {
        IOLog("TegraNVENC: %s\n", gLine);
        giveUp(true);
        return;
    }
    IOLog("TegraNVENC: E2 ok host interface and falcon incremented syncpoint %u %u->%u by interrupt in %llu us; %s\n",
          id, from, tegra_host1x_syncpt_read(id), us, falconState());
    gSyncpt = id;
    gBase = tegra_host1x_syncpt_read(id);
    gUp = true;
    tegra_host1x_set_sleep_client(engineSuspend, engineResume);
    tegra_nvenc_publish();
}
}  // namespace

extern "C" bool tegra_nvenc_ready(void)
{
    return gUp && !gWedged;
}

namespace {
int run(const TegraNVENCSubmit *s, TegraNVENCResolve resolve, void *context, uint32_t *fence, const char **why)
{
    *why = nullptr;
    if (!gUp)
        return -19;
    if (s->entry_count > TEGRA_NVENC_MAX_ENTRIES || s->clean_count > TEGRA_NVENC_MAX_RANGES ||
        s->setup_bytes > kSetupSlotBytes) {
        *why = "counts";
        return -22;
    }
    if (s->codec >= 32 || !(kTegraNVENCCodecs & (1u << s->codec))) {
        *why = "codec";
        return -22;
    }
    TegraNVENCBlock blocks[TEGRA_NVENC_MAX_ENTRIES];
    uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        if (!resolve(context, e->value, &blocks[i]) || e->offset > blocks[i].size) {
            *why = "relocation outside a buffer";
            return -22;
        }
        avail[i] = blocks[i].size - e->offset;
    }
    *why = tegra_nvenc_check(s, avail);
    if (*why)
        return -22;
    for (uint32_t i = 0; i < s->clean_count; i++) {
        const TegraNVENCRange *r = &s->clean[i];
        TegraNVENCBlock b;
        if (!resolve(context, r->handle, &b) || static_cast<uint64_t>(r->offset) + r->length > b.size) {
            *why = "clean range";
            return -22;
        }
        switch_dcache_clean(b.va + r->offset, r->length);
    }

    // SETCLASS; the submit's methods, SET_APPLICATION_ID first (the checker requires it: the OS selects the app before
    // anything is stored for it), then just before EXECUTE: 0 in every method of kUnusedMethods, and in the ME
    // predictor pair when the submit has none; the scratch buffer as the RC process state (NVIDIA's driver gives every
    // job one; constant QP leaves it unread); the setup slot (INCR METHOD0 x2: method, value, each); OP_DONE increment.
    constexpr uint32_t kUnused = sizeof(kUnusedMethods) / sizeof(kUnusedMethods[0]);
    uint32_t words[1 + 3 * (kUnused + 4 + TEGRA_NVENC_MAX_ENTRIES) + 1];
    uint32_t n = 0;
    bool predictors = false;
    for (uint32_t i = 0; i < s->entry_count; i++)
        predictors |= s->entries[i].method == NVC5B7_SET_IN_MEPRED_DATA;
    words[n++] = tegra_host1x_op_setclass(kClass, 0, 0);
    const auto method = [&](uint32_t offset, uint32_t value) {
        words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
        words[n++] = offset / 4;
        words[n++] = value;
    };
    IOLockLock(gEngineLock);
    while (gSuspended)
        IOLockSleep(gEngineLock, &gSuspended, THREAD_UNINT);
    if (gWedged) {
        IOLockUnlock(gEngineLock);
        return -19;
    }
    const uint32_t job = gIssued + 1;
    if (job > kInFlight) {
        const int ret = tegra_host1x_syncpt_wait(gSyncpt, gBase + job - kInFlight, kWaitBoundUs);
        if (ret) {
            if (ret == TEGRA_HOST1X_ETIMEDOUT)
                timedOut(gBase + job - kInFlight);
            IOLockUnlock(gEngineLock);
            return ret;
        }
    }
    const uint32_t slot = job % kSetupSlots;
    uint8_t *setup = gSetup.va + static_cast<size_t>(slot) * kSetupSlotBytes;
    memcpy(setup, s->setup, s->setup_bytes);
    switch_dcache_clean(setup, s->setup_bytes);
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVENCEntry *e = &s->entries[i];
        if (i == s->entry_count - 1) {  // EXECUTE, last by the check
            for (const uint16_t unused : kUnusedMethods)
                method(unused, 0);
            if (!predictors) {
                method(NVC5B7_SET_IN_MEPRED_DATA, 0);
                method(NVC5B7_SET_OUT_MEPRED_DATA, 0);
            }
            method(NVC5B7_SET_IO_RC_PROCESS, static_cast<uint32_t>(gSink.pa >> 8));
            method(NVC5B7_SET_IN_DRV_PIC_SETUP,
                   static_cast<uint32_t>((gSetup.pa + static_cast<uint64_t>(slot) * kSetupSlotBytes) >> 8));
        }
        method(e->method, e->kind == TEGRA_NVENC_ENTRY_RELOC ? static_cast<uint32_t>((blocks[i].iova + e->offset) >> 8)
                                                             : e->value);
    }
    words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    const int ret = tegra_host1x_submit(words, n);
    if (!ret) {
        __atomic_store_n(&gIssued, job, __ATOMIC_RELEASE);
        *fence = gBase + job;
    }
    IOLockUnlock(gEngineLock);
    return ret;
}
}  // namespace

extern "C" int tegra_nvenc_run(const TegraNVENCSubmit *s, TegraNVENCResolve resolve, void *context, uint32_t *fence,
                               const char **why)
{
    return run(s, resolve, context, fence, why);
}

extern "C" int tegra_nvenc_wait(uint32_t fence)
{
    if (!gUp)
        return -19;
    // A fence no job will reach would never be reached: refused, so it cannot pass for an engine fault.
    if ((int32_t)(fence - (gBase + __atomic_load_n(&gIssued, __ATOMIC_ACQUIRE))) > 0)
        return -22;
    const int ret = tegra_host1x_syncpt_wait(gSyncpt, fence, kWaitBoundUs);
    if (ret == TEGRA_HOST1X_ETIMEDOUT) {
        IOLockLock(gEngineLock);
        timedOut(fence);
        IOLockUnlock(gEngineLock);
    }
    return ret;
}

// Sleep (SC7), through TegraHost1x's sleep clients. Suspend lets the queued jobs finish (interrupt waits) and parks
// the engine in reset before host1x saves its syncpoints; new jobs block until resume. Resume runs after host1x is
// back: the partition is powered, the falcon reloaded from the kept image and proven with an IMMEDIATE and an OP_DONE
// increment, as at E2.
namespace {
void engineSuspend()
{
    IOLockLock(gEngineLock);
    gSuspended = true;
    if (!gWedged && tegra_host1x_syncpt_wait(gSyncpt, gBase + gIssued, kWaitBoundUs) == TEGRA_HOST1X_ETIMEDOUT)
        timedOut(gBase + gIssued);
    park();
    tegra_nvenc_memory_detach();
    IOLockUnlock(gEngineLock);
}

void engineResume()
{
    IOLockLock(gEngineLock);
    bool wasOn = false;
    gLine[0] = 0;
    gWedged = true;
    if (!tegra_nvenc_memory_attach(gLine, sizeof(gLine)) && powerUp(&wasOn) && bootFalcon()) {
        const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
        const char *stage = nullptr;
        const int ret = answer(gSyncpt, from, &stage);
        if (!ret) {
            gWedged = false;
        } else {
            snprintf(gLine, sizeof(gLine), "%s did not answer (%d, syncpoint %u %u->%u); %s", stage, ret, gSyncpt, from,
                     tegra_host1x_syncpt_read(gSyncpt), falconState());
        }
    }
    if (gWedged)
        IOLog("TegraNVENC: resume FAIL %s; NVENC refuses work\n", gLine[0] ? gLine : "E1");
    // Jobs lost to a wedge never incremented the syncpoint: number from where it really is.
    gBase = tegra_host1x_syncpt_read(gSyncpt);
    __atomic_store_n(&gIssued, 0, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gEngineLock, &gSuspended, false);
    IOLockUnlock(gEngineLock);
}
}  // namespace

// switch-no-nvdec turns hardware video encoding off as well as decoding.
extern "C" int tegra_nvenc_probe(void)
{
    static volatile int started;
    if (switch_boot_flag("switch-no-nvdec"))
        return 0;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST))
        return 0;
    thread_t thread = nullptr;
    if (kernel_thread_start(bringUp, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVENC: could not start the bring-up thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
