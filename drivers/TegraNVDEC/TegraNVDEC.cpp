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
// VideoToolbox keeps Apple's software decoders. No job runs before N3. From then a job past its bound is cancelled
// with one line and fails, the engine reset and its falcon rebooted, and the next job runs (TegraNVDECCancel.cpp); a
// lost syncpoint interrupt panics in TegraHost1x.
//
// The codecs are those the unit decoded right: on every boot of 11-06 an H.264 picture, a VP9 key frame and an HEVC IDR
// matched x264, libvpx and FFmpeg, and VP9 and HEVC Main clips matched through VideoToolbox
// (drivers/SwitchVideoDecoders/test/vdectest.c). HEVC Main 10 written as 8 bits did not match FFmpeg's there, so Main
// 10 is decoded into 16-bit samples instead (TEGRA_NVDEC_HEVC_MAIN10). MPEG-4 Part 2 and MPEG-1/2 are on from host
// proof alone (drivers/SwitchVideoDecoders/test/host mpeg4_host, mpeg2_host); hw_test_app's decode.mpeg4_* and
// decode.mpeg2_* clips are their first device run.
//
// Jobs (tegra_nvdec_run): host1x channel 0 in NVDEC's class, each method written through the engine's THI
// METHOD0/METHOD1 pair (averne's av_nvtegra_cmdbuf_push_value; L4T nvhost), the picture setup from a kernel-only slot,
// then an OP_DONE increment of NVDEC's syncpoint: the job's fence. At most 8 jobs are queued, which bounds both the
// push buffer and the 16 setup slots. Every job runs with the engine's GP timer on (the checker requires it), which
// ends a picture the firmware cannot finish after 1 << 27 cycles (329 ms at 408 MHz, 291 ms at 460.8 MHz) with an
// error status, so bad input never holds the engine: waits are bounded by kJobBoundUs per queued job, whatever timeout
// a client asks for, and a job past that bound means the engine stopped making progress.
//
// Firmware: the non-secure falcon image L4T loads as nvhost_nvdec020_ns.fw when CONFIG_NVDEC_BOOTLOADER is off
// (switchroot's tegra_linux_defconfig), here linux-firmware's nvidia/tegra210/nvdec.bin (firmware/SOURCE). The secure
// path (nvhost_nvdec_bl020_prod.fw + nvhost_nvdec020_prod.fw, WPR carveout set up by the TSEC) is not used.
//
// Sources (Linux's drivers are GPL-2.0-only, so this follows them without copying):
//   tegra210.dtsi nvdec@54480000 (CAR clock and reset 194, power domain "nvdec" = PMC partition 25), L4T r32.7.4
//   powergate-t21x.c (partition 25 after NVJPG's 26, its MC client flush and release, the MBIST workaround on T210
//   before B01 only: LVL2_CLK_GATE_OVRE bits 9 and 31 with NVJPG's clock) and t210/cg_regs.c (the prod registers),
//   through SwitchEnginePower.h, clk-tegra-periph.c (CLK_SOURCE_NVDEC 0x698, PLLP is mux code 4),
//   memory/tegra/tegra210.c (MC SMMU NVDEC ASID 0xab4, client hot reset NVDEC 0x970 bit 5), drm/tegra/falcon.c
//   falcon_boot (scrub wait, DMATRFBASE = OS data, data then code in 256-byte DMA chunks, IRQMSET/IRQDEST, ITFEN,
//   BOOTVEC 0, STARTCPU) and nvdec.c (tegra210: no stream IDs, no RISC-V, firmware nvidia/tegra210/nvdec.bin,
//   embedded here by firmware.S). L4T nvhost t210.c,
//   nvdec.c and flcn.c agree on the order; DMA context 0 as L4T uses it on T210 (no context isolation there).
//   Clock: PLLC3, as L4T's c3bus, at the rate tegra210b01-core-dvfs.c allows at VDD_CORE's 637 mV minimum for this
//   chip's SoC process (460.8 MHz for process 1), or PLLP's 408 MHz where that is lower or PLLC3 is not ours
//   (kits/SwitchCAR/SwitchEngineClock.h).
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
#include "TegraNVDECEngine.h"
#include "SwitchNVDECJobs.h"
#include "../../kits/SwitchBootArgs/SwitchBootArgs.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchEngineClock.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchEngineGate/SwitchEngineGateKernel.h"
#include "../../kits/SwitchEngineGate/SwitchEnginePower.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
extern const uint8_t tegra_nvdec_fw_start[] __asm__("_tegra_nvdec_fw_start");
extern const uint8_t tegra_nvdec_fw_end[] __asm__("_tegra_nvdec_fw_end");
IOLock *tegra_nvdec_power_lock(void);
}

namespace {
constexpr uint64_t kNvdecPA = 0x54480000, kNvdecSize = 0x40000;
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kPartition = 25;       // TEGRA_POWERGATE_NVDEC
constexpr unsigned kClock = 194;          // TEGRA210_CLK_NVDEC: CAR bank Y bit 2, also its reset
constexpr uint32_t kNvjpgPartition = 26;  // TEGRA_POWERGATE_NVJPG, which NVDEC's needs on
constexpr unsigned kNvjpgClock = 195;     // TEGRA210_CLK_NVJPG: NVDEC's slcg clock

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_Y = 0x298,
    RST_DEVICES_Y = 0x2a4,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_NVDEC = 0x698,
    OVRE_NVDEC_MBIST = (1u << 9) | (1u << 31),
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    PWRGATE_STATUS = 0x38,
};

enum : uint32_t {
    MC_INTSTATUS = 0x0,
    MC_ERR_STATUS = 0x8,
    MC_ERR_ADR = 0xc,
    MC_HOTRESET_CTRL_1 = 0x970,
    MC_HOTRESET_STATUS_1 = 0x974,
    MC_HOTRESET_NVDEC = 1u << 5,
    MC_HOTRESET_NVJPG = 1u << 8,
};

volatile uint8_t *gNvdec, *gMc;
uint32_t gClockKHz;
switch_dma_buffer gFirmware;  // stays: the falcon OS loads its apps from it through DMATRFBASE
switch_falcon_image gImage;
char gLine[256];

constexpr uint32_t kSetupSlots = 16, kSetupSlotBytes = 1024;
SwitchEngineGate<SwitchEngineGateKernel> gGate;
uint32_t gSyncpt;
bool gUp, gWedged, gPublished;
constexpr uint32_t kCodecs = (1u << TEGRA_NVDEC_CODEC_H264) | (1u << TEGRA_NVDEC_CODEC_VP9) |
                             (1u << TEGRA_NVDEC_CODEC_HEVC) | TEGRA_NVDEC_HEVC_MAIN10 |
                             (1u << TEGRA_NVDEC_CODEC_MPEG4) | (1u << TEGRA_NVDEC_CODEC_MPEG12);
switch_dma_buffer gSetup;  // kSetupSlots picture setups, never mapped into a client

const char *falconState()
{
    static char text[160];
    if (__atomic_load_n(&gUp, __ATOMIC_ACQUIRE) && !gGate.powered())
        return "falcon powered off (idle)";
    return switch_falcon_state(gNvdec, text, sizeof(text));
}

// NVDEC's partition and memory client in L4T's order (SwitchEnginePower.h), then its clock-gating prod registers
// (t210/cg_regs.c t21x_nvdec_gating_registers). Partition 25 needs NVJPG's 26 on (powergate-t21x.c powers 26 first and
// NVJPG's idle power-off keeps it on while 25 is): 26 comes up here if it is off, with NVJPG's clock left off. A
// partition the boot chain left on keeps its state; the engine is still reset so the falcon starts from scratch.
constexpr unsigned kSlcgClocks[] = {kNvjpgClock}, kNvjpgSlcgClocks[] = {kClock};
const SwitchEnginePower kPower = {kPartition,         kClock,           MC_HOTRESET_CTRL_1, MC_HOTRESET_STATUS_1,
                                  MC_HOTRESET_NVDEC,  OVRE_NVDEC_MBIST, kSlcgClocks,        1,
                                  tegra_mc_hot_reset, tegra_smmu_read};
const SwitchEnginePower kNvjpgPower = {kNvjpgPartition,    kNvjpgClock,      MC_HOTRESET_CTRL_1, MC_HOTRESET_STATUS_1,
                                       MC_HOTRESET_NVJPG,  OVRE_NVDEC_MBIST, kNvjpgSlcgClocks,   1,
                                       tegra_mc_hot_reset, tegra_smmu_read};
constexpr SwitchEngineProd kProd[] = {
    {0xe00, 0},                      // THI_CLK_OVERRIDE
    {0x88, 0},                       // THI_SLCG_OVERRIDE_HIGH_A
    {0x8c, 0},                       // THI_SLCG_OVERRIDE_LOW_A
    {0x10a0, 0},                     // FALCON_CGCTL
    {0x1604, 0}, {0x117c, 0x30008},  // FALCON_CG2
    {0x2328, 0}, {0x232c, 0x800000}, {0x2330, 0xfffffff8},
};

bool powerUp(bool *wasOn)
{
    const auto failed = [](unsigned partition) {
        snprintf(gLine, sizeof(gLine),
                 "N1 FAIL partition %u did not come up (PWRGATE_TOGGLE %#x STATUS %#x CLAMP_STATUS %#x "
                 "CLK_OUT_ENB_Y %#x RST_DEVICES_Y %#x)",
                 partition, switch_pmc_read(PWRGATE_TOGGLE), switch_pmc_read(PWRGATE_STATUS),
                 switch_pmc_read(switch_engine_power::kPmcClampStatus), switch_car_read(CLK_OUT_ENB_Y),
                 switch_car_read(RST_DEVICES_Y));
        return false;
    };
    if (!switch_pmc_partition_is_on(kNvjpgPartition)) {
        bool nvjpgWasOn = false;
        if (!switch_engine_power_up(kNvjpgPower, &nvjpgWasOn))
            return failed(kNvjpgPartition);
        switch_car_clock_disable(kNvjpgClock);
    }
    switch_car_write(CLK_SOURCE_NVDEC, switch_engine_clock_source(SWITCH_ENGINE_BUS_C3, &gClockKHz));
    IODelay(2);
    if (!switch_engine_power_up(kPower, wasOn))
        return failed(kPartition);
    switch_engine_prod(gNvdec, kProd, sizeof(kProd) / sizeof(kProd[0]));
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
    int ret = switch_host1x_job_copy(TEGRA_HOST1X_CHANNEL_DEFAULT, stream, 3, *id, *from + 2);
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

// N2 again on the job syncpoint, after a power-on or a reset: one line in gLine when the falcon does not answer.
bool proven()
{
    const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, gSyncpt),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt),
    };
    const int ret = switch_host1x_job_copy(TEGRA_HOST1X_CHANNEL_DEFAULT, stream, 3, gSyncpt, from + 2);
    if (!ret && !tegra_host1x_syncpt_wait(gSyncpt, from + 2, 500000))
        return true;
    char state[160];
    snprintf(gLine, sizeof(gLine), "falcon did not answer (%d, syncpoint %u %u->%u); %s", ret, gSyncpt, from,
             tegra_host1x_syncpt_read(gSyncpt), switch_falcon_state(gNvdec, state, sizeof(state)));
    return false;
}

void engineSuspend();
void engineResume();
int gateOn(void *);
int gateOff(void *, bool drain);

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
    if (!gNvdec || !gMc) {
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
    IOLog("TegraNVDEC: N1 ok SMMU ASID %u, partition %u %s, %u kHz, falcon started (OS code %#x+%#x data %#x+%#x, %u "
          "apps)\n",
          static_cast<unsigned>(TEGRA_SMMU_ASID_NVDEC), kPartition, wasOn ? "already on" : "powered here", gClockKHz,
          gImage.code, gImage.code_size, gImage.data, gImage.data_size, gImage.apps);
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
    if (!switch_nvdec_jobs_start(gSetup, id, tegra_host1x_syncpt_read(id)) || !switch_nvdec_cancel_start(id) ||
        !gGate.start({"TegraNVDEC", nullptr, gateOn, gateOff}, tegra_nvdec_power_lock())) {
        IOLog("SwitchNVDEC: completion worker or idle timer could not start\n");
        giveUp(true);
        return;
    }
    __atomic_store_n(&gUp, true, __ATOMIC_RELEASE);
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
    return __atomic_load_n(&gUp, __ATOMIC_ACQUIRE) && !__atomic_load_n(&gWedged, __ATOMIC_ACQUIRE);
}

[[noreturn]] void switch_nvdec_job_fault(uint32_t fence, int error, uint64_t elapsedUs)
{
    char host[200];
    tegra_host1x_describe(host, sizeof(host));
    panic("SwitchNVDEC: fence %u failed (%d) after %llu us, syncpoint %u at %u; %s; %s", fence, error, elapsedUs,
          gSyncpt, tegra_host1x_syncpt_read(gSyncpt), falconState(), host);
}

bool switch_nvdec_engine_powered()
{
    return gGate.powered();
}

void switch_nvdec_engine_describe(char *text, size_t size)
{
    char falcon[400];
    snprintf(text, size, "%s; MC INTSTATUS %#x ERR_STATUS %#x ERR_ADR %#x",
             switch_falcon_stall(gNvdec, falcon, sizeof(falcon)), switch_mmio_read32(gMc, MC_INTSTATUS),
             switch_mmio_read32(gMc, MC_ERR_STATUS), switch_mmio_read32(gMc, MC_ERR_ADR));
}

// The memory controller stops taking NVDEC's requests and drains those in flight (tegra_mc_flush), so the reset cannot
// cut a transfer in half; powerUp releases the flush once the engine is out of reset again.
bool switch_nvdec_engine_reset(bool *drained)
{
    *drained = switch_engine_power::flush(kPower);
    switch_car_reset_assert(kClock);
    IODelay(10);
    bool wasOn = false;
    gLine[0] = 0;
    return powerUp(&wasOn);
}

bool switch_nvdec_engine_boot()
{
    return bootFalcon() && proven();
}

const char *switch_nvdec_engine_wedge()
{
    __atomic_store_n(&gWedged, true, __ATOMIC_RELEASE);
    park();
    return gLine[0] ? gLine : "falcon";
}

// Idle power gating (SwitchEngineGate.h), as mainline's runtime PM: off 500 ms after the last job, on at the next. On
// is N1 and N2 again on the kept image and address space: partition, clock, reset and the MBIST workaround, the falcon
// reloaded and proven with an IMMEDIATE and an OP_DONE increment, the job queue rebased on the syncpoint. A failure
// there is one line and NVDEC refuses work from then on. Off parks the queue and powers the partition down. NVJPG's
// gate shares the lock (tegra_nvdec_power_lock), so neither engine's power-up borrows a clock the other is switching.
namespace {
int gateOn(void *)
{
    bool wasOn = false;
    gLine[0] = 0;
    const bool answered = powerUp(&wasOn) && switch_nvdec_engine_boot();
    if (!answered) {
        __atomic_store_n(&gWedged, true, __ATOMIC_RELEASE);
        park();
        IOLog("TegraNVDEC: power-on FAIL %s; NVDEC refuses work\n", gLine[0] ? gLine : "N1");
    }
    switch_nvdec_jobs_resume(answered, tegra_host1x_syncpt_read(gSyncpt));
    return answered ? 0 : -19;
}

int gateOff(void *, bool drain)
{
    if (!switch_nvdec_jobs_park(drain))
        return SWITCH_ENGINE_GATE_BUSY;
    return switch_engine_power_down(kPower) ? SWITCH_ENGINE_GATE_OFF : -5;
}

// Sleep (SC7), through tegra_nvdec_suspend/resume in TegraHost1x.cpp. Suspend powers the engine off once its queued
// jobs are done and a cancellation under way has released their fences (it waits for the power lock suspend holds),
// before host1x saves its syncpoints, and detaches its address space; new jobs wait for resume. Resume runs after
// host1x is back and attaches the address space again (SC7 resets the MC); the next job powers NVDEC on.
void engineSuspend()
{
    gGate.suspend();
    switch_nvdec_jobs_settle();
    tegra_nvdec_memory_detach();
}

void engineResume()
{
    gLine[0] = 0;
    const bool attached = !tegra_nvdec_memory_attach(gLine, sizeof(gLine));
    if (!attached) {
        __atomic_store_n(&gWedged, true, __ATOMIC_RELEASE);
        IOLog("TegraNVDEC: resume FAIL %s; NVDEC refuses work\n", gLine);
        switch_nvdec_jobs_resume(false, tegra_host1x_syncpt_read(gSyncpt));
    }
    gGate.resume(attached);
}
}  // namespace

int switch_nvdec_power_use()
{
    return gGate.use();
}

void switch_nvdec_power_done()
{
    gGate.done();
}

void switch_nvdec_power_idle()
{
    gGate.idle();
}

extern "C" IOLock *tegra_nvdec_power_lock(void)
{
    static IOLock *lock;
    if (!__atomic_load_n(&lock, __ATOMIC_ACQUIRE)) {
        IOLock *fresh = IOLockAlloc();
        IOLock *expected = nullptr;  // NOLINT(misc-const-correctness): the compare-exchange writes it
        if (!__atomic_compare_exchange_n(&lock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    return lock;
}

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
