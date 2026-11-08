// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 NVDEC (video decoder 2.0, a falcon behind host1x): bring-up, the job engine and sleep. tegra_nvdec_probe()
// runs the whole bring-up (boot-arg switch-no-nvdec turns it off) on its own thread:
//   N0  host1x up (drivers/TegraHost1x: clock adopted, syncpoint interrupt, channel 0)
//   N1  NVDEC's SMMU address space (TegraNVDECMemory.cpp), power partition, clock, reset, falcon firmware loaded and
//       started
//   N2  proof of life through host1x channel 0 in NVDEC's class: an IMMEDIATE syncpoint increment (the engine's host
//       interface answers) then an OP_DONE one (the falcon firmware answers), each completed by interrupt
//   N3  the TegraNVDEC service and its user client published (TegraNVDECUserClient.cpp)
// One line per stage ("TegraNVDEC: N1 ok ..."), or the failing stage's FAIL line and nothing after it. Until N3
// publishes, every failure (a register that does not settle, an engine that does not answer, an interrupt that never
// comes) is that one line: the engine is put back in reset with its clock off and nothing is published, so
// VideoToolbox keeps Apple's software decoders. No job runs before N3, and from then a lost syncpoint interrupt
// (TegraHost1x) or a job past its bound panics with one line.
//
// The codecs are those the unit decoded right: on every boot of 11-06 an H.264 picture, a VP9 key frame and an HEVC IDR
// matched x264, libvpx and FFmpeg, and VP9 and HEVC Main clips matched through VideoToolbox
// (drivers/SwitchVideoDecoders/test/vdectest.c). HEVC Main 10 written as 8 bits did not match FFmpeg's there, so it
// is not offered (TEGRA_NVDEC_HEVC_MAIN10).
//
// Jobs (tegra_nvdec_run): host1x channel 0 in NVDEC's class, each method written through the engine's THI
// METHOD0/METHOD1 pair (averne's av_nvtegra_cmdbuf_push_value; L4T nvhost), the picture setup from a kernel-only slot,
// then an OP_DONE increment of NVDEC's syncpoint: the job's fence. At most 8 jobs are queued, which bounds both the
// push buffer and the 16 setup slots. Every job runs with the engine's GP timer on (the checker requires it), which
// ends a picture the firmware cannot finish after 1 << 27 cycles (329 ms at 408 MHz) with an error status, so bad
// input never holds the engine: waits are bounded by kJobBoundUs per queued job, whatever timeout a client asks for,
// and a job past that bound is a hardware fault.
//
// Firmware: the non-secure falcon image L4T loads as nvhost_nvdec020_ns.fw when CONFIG_NVDEC_BOOTLOADER is off
// (switchroot's tegra_linux_defconfig), here linux-firmware's nvidia/tegra210/nvdec.bin (firmware/SOURCE). The secure
// path (nvhost_nvdec_bl020_prod.fw + nvhost_nvdec020_prod.fw, WPR carveout set up by the TSEC) is not used.
//
// Sources (Linux's drivers are GPL-2.0-only, so this follows them without copying):
//   tegra210.dtsi nvdec@54480000 (CAR clock and reset 194, power domain "nvdec" = PMC partition 25), Linux
//   soc/tegra/pmc.c tegra_powergate_power_up (reset asserted, partition on, clocks, clamps off, reset released, MBIST
//   workaround), clk-tegra210.c tegra210_pg_mbist_war[NVDEC] (NVJPG clock on, LVL2_CLK_GATE_OVRE bits 9 and 31
//   pulsed), clk-tegra-periph.c (CLK_SOURCE_NVDEC 0x698, PLLP is mux code 4), memory/tegra/tegra210.c (MC SMMU NVDEC
//   ASID 0xab4, client hot reset NVDEC 0x970 bit 5), drm/tegra/falcon.c falcon_boot (scrub wait, DMATRFBASE = OS data,
//   data then code in 256-byte DMA chunks, IRQMSET/IRQDEST, ITFEN, BOOTVEC 0, STARTCPU) and nvdec.c (tegra210: no
//   stream IDs, no RISC-V, firmware nvidia/tegra210/nvdec.bin, embedded here by firmware.S). L4T nvhost t210.c,
//   nvdec.c and flcn.c agree on the order; DMA context 0 as L4T uses it on T210 (no context isolation there).
//   Clock: PLLP undivided, 408 MHz, below the lowest T210B01 NVDEC DVFS point (460.8 MHz at minimum core voltage,
//   tegra210b01-core-dvfs.c), so it is safe at any core voltage the boot chain left.
// Waits on hardware sequencing with no interrupt of its own (partition status, IMEM/DMEM scrub, the falcon's DMA
// queue) are bounded reads, as in drivers/SwitchUSB and nouveau; every completion of work is a host1x syncpoint
// interrupt (TegraHost1x), and a lost one panics there.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "TegraNVDEC.h"
#include "TegraNVDECCheck.h"
#include "../../kits/SwitchBootArgs/SwitchBootArgs.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
extern const uint8_t tegra_nvdec_fw_start[] __asm__("_tegra_nvdec_fw_start");
extern const uint8_t tegra_nvdec_fw_end[] __asm__("_tegra_nvdec_fw_end");
}

namespace {
constexpr uint64_t kNvdecPA = 0x54480000, kNvdecSize = 0x40000;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data), averne nvtegra_host1x.h
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kPartition = 25;    // TEGRA_POWERGATE_NVDEC
constexpr unsigned kClock = 194;       // TEGRA210_CLK_NVDEC: CAR bank Y bit 2, also its reset
constexpr unsigned kNvjpgClock = 195;  // TEGRA210_CLK_NVJPG: NVDEC's slcg clock

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_Y = 0x298,
    RST_DEVICES_Y = 0x2a4,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_NVDEC = 0x698,
    OVRE_NVDEC_MBIST = (1u << 9) | (1u << 31),
    SOURCE_PLLP = 4u << 29,
    NVDEC_DIV_1 = 0,  // 7.1 divider: 408 MHz * 2 / (0 + 2) = 408 MHz
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    PWRGATE_STATUS = 0x38,
};

enum : uint32_t {
    MC_HOTRESET_CTRL_1 = 0x970,
    MC_HOTRESET_NVDEC = 1u << 5,
};

volatile uint8_t *gNvdec, *gMc;
switch_dma_buffer gFirmware;  // stays: the falcon OS loads its apps from it through DMATRFBASE
switch_falcon_image gImage;
char gLine[256];

// The job engine (tegra_nvdec_run / tegra_nvdec_wait / sleep).
// kJobBoundUs: the GP timer's 329 ms plus margin; a wait covers at most kInFlight queued jobs.
constexpr uint32_t kInFlight = 8, kSetupSlots = 16, kSetupSlotBytes = 1024, kJobBoundUs = 400000;
constexpr uint32_t kWaitBoundUs = kInFlight * kJobBoundUs;
IOLock *gEngineLock;
uint32_t gSyncpt, gBase, gIssued;  // job n's fence is gBase + n
bool gUp, gWedged, gSuspended, gPublished;
constexpr uint32_t kCodecs =
    (1u << TEGRA_NVDEC_CODEC_H264) | (1u << TEGRA_NVDEC_CODEC_VP9) | (1u << TEGRA_NVDEC_CODEC_HEVC);
switch_dma_buffer gSetup;  // kSetupSlots picture setups, never mapped into a client

const char *falconState()
{
    static char text[160];
    return switch_falcon_state(gNvdec, text, sizeof(text));
}

// tegra_powergate_power_up with the T210 MBIST workaround. A partition the boot chain left on keeps its state; the
// engine is still reset and re-clocked so the falcon starts from scratch.
bool powerUp(bool *wasOn)
{
    *wasOn = switch_pmc_partition_is_on(kPartition);
    switch_car_reset_assert(kClock);
    IODelay(10);
    if (!*wasOn) {
        if (!switch_pmc_partition_on(kPartition)) {
            snprintf(gLine, sizeof(gLine), "N1 FAIL partition %u did not power on (PWRGATE_TOGGLE %#x STATUS %#x)",
                     kPartition, switch_pmc_read(PWRGATE_TOGGLE), switch_pmc_read(PWRGATE_STATUS));
            return false;
        }
        IODelay(10);
    }
    switch_car_write(CLK_SOURCE_NVDEC, SOURCE_PLLP | NVDEC_DIV_1);
    IODelay(2);
    switch_car_clock_enable(kClock);
    IODelay(10);
    switch_pmc_remove_clamping(kPartition);
    IODelay(10);
    if (switch_mmio_read32(gMc, MC_HOTRESET_CTRL_1) & MC_HOTRESET_NVDEC)
        switch_mmio_write32(gMc, MC_HOTRESET_CTRL_1, switch_mmio_read32(gMc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_NVDEC);
    switch_car_reset_deassert(kClock);
    IODelay(10);
    // tegra210_generic_mbist_war with NVDEC's slcg clock (NVJPG's) running for the pulse; only NVDEC's override bits
    // change, under the CAR lock.
    const bool nvjpgOn = switch_car_clock_enabled(kNvjpgClock);
    if (!nvjpgOn)
        switch_car_clock_enable(kNvjpgClock);
    const uint32_t overridden = switch_car_read(LVL2_CLK_GATE_OVRE) & OVRE_NVDEC_MBIST;
    switch_car_rmw(LVL2_CLK_GATE_OVRE, 0, OVRE_NVDEC_MBIST);
    IODelay(1);
    switch_car_rmw(LVL2_CLK_GATE_OVRE, OVRE_NVDEC_MBIST & ~overridden, 0);
    IODelay(1);
    if (!nvjpgOn)
        switch_car_clock_disable(kNvjpgClock);
    if (!switch_car_clock_enabled(kClock) || switch_car_reset_asserted(kClock)) {
        snprintf(gLine, sizeof(gLine), "N1 FAIL clock or reset stuck (CLK_OUT_ENB_Y %#x RST_DEVICES_Y %#x)",
                 switch_car_read(CLK_OUT_ENB_Y), switch_car_read(RST_DEVICES_Y));
        return false;
    }
    return true;
}

// The whole image goes into one contiguous buffer below 4 GiB, cleaned to DRAM and identity-mapped in NVDEC's address
// space: the falcon fetches it by physical address, now for its OS and later for the apps.
bool loadImage()
{
    const size_t length =
        reinterpret_cast<uintptr_t>(tegra_nvdec_fw_end) - reinterpret_cast<uintptr_t>(tegra_nvdec_fw_start);
    if (!switch_falcon_parse(tegra_nvdec_fw_start, length, &gImage)) {
        snprintf(gLine, sizeof(gLine), "N1 FAIL embedded firmware is not a falcon v1 image (%lu bytes)",
                 static_cast<unsigned long>(length));
        return false;
    }
    if (!gFirmware.memory) {  // kept for resume: the falcon is reloaded from it after SC7
        if (!switch_dma_alloc(&gFirmware, gImage.size)) {
            snprintf(gLine, sizeof(gLine), "N1 FAIL no contiguous %u-byte firmware buffer below 4 GiB", gImage.size);
            return false;
        }
        memcpy(gFirmware.va, tegra_nvdec_fw_start, gImage.size);
        switch_dcache_clean(gFirmware.va, gImage.size);
    }
    return true;
}

// falcon_boot, minus its final idle poll: that the firmware runs is shown by N2's OP_DONE increment instead.
bool bootFalcon()
{
    if (!loadImage())
        return false;
    switch (switch_falcon_load(gNvdec, gFirmware.pa, &gImage)) {
    case SWITCH_FALCON_SCRUBBING:
        snprintf(gLine, sizeof(gLine), "N1 FAIL IMEM/DMEM scrubbing did not end (DMACTL %#x)",
                 switch_mmio_read32(gNvdec, SWITCH_FALCON_DMACTL));
        return false;
    case SWITCH_FALCON_DMA:
        snprintf(gLine, sizeof(gLine), "N1 FAIL firmware DMA did not finish (DMATRFCMD %#x)",
                 switch_mmio_read32(gNvdec, SWITCH_FALCON_DMATRFCMD));
        return false;
    case SWITCH_FALCON_OK:
        break;
    }
    switch_falcon_start(gNvdec);
    return true;
}

// Channel 0 in NVDEC's class: an IMMEDIATE increment is done by the engine's host interface on arrival, an OP_DONE
// one only once the falcon reports the methods before it done.
bool proofOfLife(uint64_t *us, uint32_t *id, uint32_t *from)
{
    if (tegra_host1x_syncpt_alloc(id)) {
        snprintf(gLine, sizeof(gLine), "N2 FAIL no syncpoint left");
        return false;
    }
    *from = tegra_host1x_syncpt_read(*id);
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, *id),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, *id),
    };
    const uint64_t began = switch_now_us();
    int ret = tegra_host1x_submit(stream, 3);
    const char *stage = "submit";
    if (!ret) {
        stage = "host interface (IMMEDIATE)";
        ret = tegra_host1x_syncpt_wait(*id, *from + 1, 100000);
    }
    if (!ret) {
        stage = "falcon (OP_DONE)";
        ret = tegra_host1x_syncpt_wait(*id, *from + 2, 500000);
    }
    *us = switch_now_us() - began;
    if (ret) {
        char host[200];
        tegra_host1x_describe(host, sizeof(host));
        snprintf(gLine, sizeof(gLine), "N2 FAIL %s %s syncpoint %u (%u->%u, %d); %s; %s", stage,
                 ret == TEGRA_HOST1X_ELOST ? "incremented, but no interrupt came for" : "did not increment", *id, *from,
                 tegra_host1x_syncpt_read(*id), ret, falconState(), host);
        return false;
    }
    return true;
}

void engineSuspend();
void engineResume();

// A fence past its bound is a hardware fault.
[[noreturn]] void timedOut(uint32_t fence)
{
    char host[200];
    tegra_host1x_describe(host, sizeof(host));
    panic("TegraNVDEC: job fence %u not reached in %u ms (syncpoint %u at %u, GP timer bound passed); %s; %s", fence,
          kWaitBoundUs / 1000, gSyncpt, tegra_host1x_syncpt_read(gSyncpt), falconState(), host);
}

// Back to reset with the clock off after a failed bring-up: nothing will use the engine this boot. The engine is in
// reset before its swgroups go back to bypass, where an IOVA would reach the physical address of the same number.
void park()
{
    switch_car_reset_assert(kClock);
    IODelay(2);
    switch_car_clock_disable(kClock);
}

// TegraNVJPG starts once this bring-up has ended either way (tegra_nvdec_await).
IOLock *gSettleLock;
int gSettled;  // 0 under way, 1 NVDEC decodes (published), -1 it does not

IOLock *settleLock()
{
    if (!__atomic_load_n(&gSettleLock, __ATOMIC_ACQUIRE)) {
        IOLock *fresh = IOLockAlloc();
        IOLock *expected = nullptr;  // NOLINT(misc-const-correctness): the compare-exchange writes it
        if (!__atomic_compare_exchange_n(&gSettleLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    return gSettleLock;
}

void settle(int how)
{
    IOLock *lock = settleLock();
    IOLockLock(lock);
    gSettled = how;
    IOLockWakeup(lock, &gSettled, false);
    IOLockUnlock(lock);
}

void bringUpStages()
{
    // A failed bring-up leaves nothing behind: the engine back in reset with its clock off (once it was touched), then
    // its address space and setup slots returned.
    const auto giveUp = [](bool touched) {
        if (touched)
            park();
        tegra_nvdec_memory_release();
        switch_dma_free(&gSetup);
    };
    if (tegra_host1x_start()) {
        IOLog("TegraNVDEC: N0 FAIL host1x is not up (see the TegraHost1x line)\n");
        return;
    }
    gNvdec = switch_mmio_map(kNvdecPA, kNvdecSize);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gEngineLock = IOLockAlloc();
    if (!gNvdec || !gMc || !gEngineLock) {
        IOLog("TegraNVDEC: N1 FAIL no mapping\n");
        giveUp(false);
        return;
    }
    bool wasOn = false;
    gLine[0] = 0;
    if (!switch_dma_alloc(&gSetup, static_cast<size_t>(kSetupSlots) * kSetupSlotBytes) || !loadImage()) {
        IOLog("TegraNVDEC: %s\n", gLine[0] ? gLine : "N1 FAIL no contiguous setup slots below 4 GiB");
        giveUp(false);
        return;
    }
    if (tegra_nvdec_memory_init(&gFirmware, &gSetup, gLine, sizeof(gLine))) {
        IOLog("TegraNVDEC: %s\n", gLine);
        giveUp(false);
        return;
    }
    if (!powerUp(&wasOn) || !bootFalcon()) {
        IOLog("TegraNVDEC: %s\n", gLine[0] ? gLine : "N1 FAIL");
        giveUp(true);
        return;
    }
    IOLog("TegraNVDEC: N1 ok SMMU ASID %u, partition %u %s, 408 MHz, falcon started (OS code %#x+%#x data %#x+%#x, %u "
          "apps)\n",
          static_cast<unsigned>(TEGRA_SMMU_ASID_NVDEC), kPartition, wasOn ? "already on" : "powered here", gImage.code,
          gImage.code_size, gImage.data, gImage.data_size, gImage.apps);
    uint64_t us = 0;
    uint32_t id = 0;
    uint32_t from = 0;
    if (!proofOfLife(&us, &id, &from)) {
        IOLog("TegraNVDEC: %s\n", gLine);
        giveUp(true);
        return;
    }
    IOLog("TegraNVDEC: N2 ok host interface and falcon incremented syncpoint %u %u->%u by interrupt in %llu us; %s\n",
          id, from, tegra_host1x_syncpt_read(id), us, falconState());
    gSyncpt = id;
    gBase = tegra_host1x_syncpt_read(id);
    gUp = true;
    tegra_host1x_set_sleep_client(engineSuspend, engineResume);
    gPublished = true;
    tegra_host1x_set_strict();
    tegra_nvdec_publish();
}

void bringUp(void *, wait_result_t)  // NOLINT(misc-const-correctness): thread_continue_t's signature
{
    bringUpStages();
    settle(gPublished ? 1 : -1);
}
}  // namespace

extern "C" uint32_t tegra_nvdec_codecs(void)
{
    return kCodecs;
}

extern "C" int tegra_nvdec_await(uint32_t timeoutMs)
{
    IOLock *lock = settleLock();
    uint64_t deadline = 0;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    IOLockLock(lock);
    while (!gSettled && IOLockSleepDeadline(lock, &gSettled, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    const int settled = gSettled;
    IOLockUnlock(lock);
    return static_cast<int>(settled == 1);
}

extern "C" bool tegra_nvdec_ready(void)
{
    return gUp && !gWedged;
}

extern "C" int tegra_nvdec_run(const TegraNVDECSubmit *s, TegraNVDECResolve resolve, void *context, uint32_t *fence,
                               const char **why)
{
    *why = nullptr;
    if (!gUp)
        return -19;
    if (s->entry_count > TEGRA_NVDEC_MAX_ENTRIES || s->clean_count > TEGRA_NVDEC_MAX_RANGES ||
        s->setup_bytes > kSetupSlotBytes) {
        *why = "counts";
        return -22;
    }
    TegraNVDECBlock blocks[TEGRA_NVDEC_MAX_ENTRIES];
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        if (!resolve(context, e->value, &blocks[i]) || e->offset > blocks[i].size) {
            *why = "relocation outside a buffer";
            return -22;
        }
        avail[i] = blocks[i].size - e->offset;
    }
    *why = tegra_nvdec_check(s, avail);
    if (*why)
        return -22;
    for (uint32_t i = 0; i < s->clean_count; i++) {
        const TegraNVDECRange *r = &s->clean[i];
        TegraNVDECBlock b;
        if (!resolve(context, r->handle, &b) || (uint64_t)r->offset + r->length > b.size) {
            *why = "clean range";
            return -22;
        }
        switch_dcache_clean(b.va + r->offset, r->length);
    }

    // SETCLASS, (INCR METHOD0 x2: method, value) per method with the setup slot before EXECUTE, OP_DONE increment.
    uint32_t words[1 + 3 * (TEGRA_NVDEC_MAX_ENTRIES + 1) + 1];
    uint32_t n = 0;
    words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0);
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
    memcpy(gSetup.va + static_cast<size_t>(slot) * kSetupSlotBytes, s->setup, s->setup_bytes);
    switch_dcache_clean(gSetup.va + static_cast<size_t>(slot) * kSetupSlotBytes, s->setup_bytes);
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (i == s->entry_count - 1) {  // EXECUTE, last by the check
            method(NVC5B0_SET_DRV_PIC_SETUP_OFFSET,
                   static_cast<uint32_t>((gSetup.pa + static_cast<uint64_t>(slot) * kSetupSlotBytes) >> 8));
        }
        method(e->method,
               e->kind == TEGRA_NVDEC_ENTRY_RELOC ? (uint32_t)((blocks[i].iova + e->offset) >> 8) : e->value);
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

extern "C" int tegra_nvdec_wait(uint32_t fence)
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

// Sleep (SC7), through tegra_nvdec_suspend/resume in TegraHost1x.cpp. Suspend lets the queued jobs finish
// (interrupt waits) and parks the engine in reset before host1x saves its syncpoints; new jobs block until resume.
// Resume runs after host1x is back: the partition is powered, the falcon reloaded from the kept image and proven
// with an IMMEDIATE and an OP_DONE increment, as at N2.
namespace {
void engineSuspend()
{
    IOLockLock(gEngineLock);
    gSuspended = true;
    if (!gWedged && tegra_host1x_syncpt_wait(gSyncpt, gBase + gIssued, kWaitBoundUs) == TEGRA_HOST1X_ETIMEDOUT)
        timedOut(gBase + gIssued);
    park();
    tegra_nvdec_memory_detach();
    IOLockUnlock(gEngineLock);
}

void engineResume()
{
    IOLockLock(gEngineLock);
    bool wasOn = false;
    gLine[0] = 0;
    gWedged = true;
    if (!tegra_nvdec_memory_attach(gLine, sizeof(gLine)) && powerUp(&wasOn) && bootFalcon()) {
        const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
        const uint32_t stream[] = {
            tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0),
            tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, gSyncpt),
            tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt),
        };
        const int ret = tegra_host1x_submit(stream, 3);
        if (!ret && !tegra_host1x_syncpt_wait(gSyncpt, from + 2, 500000)) {
            gWedged = false;
        } else {
            snprintf(gLine, sizeof(gLine), "falcon did not answer (%d, syncpoint %u %u->%u); %s", ret, gSyncpt, from,
                     tegra_host1x_syncpt_read(gSyncpt), falconState());
        }
    }
    if (gWedged)
        IOLog("TegraNVDEC: resume FAIL %s; NVDEC refuses work\n", gLine[0] ? gLine : "N1");
    // Jobs lost to a wedge never incremented the syncpoint: number from where it really is.
    gBase = tegra_host1x_syncpt_read(gSyncpt);
    __atomic_store_n(&gIssued, 0, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gEngineLock, &gSuspended, false);
    IOLockUnlock(gEngineLock);
}
}  // namespace

extern "C" int tegra_nvdec_probe(void)
{
    static volatile int started;
    if (switch_boot_flag("switch-no-nvdec")) {
        settle(-1);
        return 0;
    }
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST))
        return 0;
    thread_t thread = nullptr;
    if (kernel_thread_start(bringUp, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVDEC: could not start the bring-up thread\n");
        settle(-1);
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
