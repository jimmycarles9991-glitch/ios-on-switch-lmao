// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 NVJPG (JPEG codec 1.0, a falcon behind host1x): bring-up, its SMMU domain, the job engine and sleep.
// tegra_nvjpg_probe() runs the whole bring-up by default on its own thread:
//   J0  NVDEC's bring-up has ended (NVJPG's MBIST workaround runs on NVDEC's clock),
//       host1x is up, and NVJPG's kernel buffers are allocated: the firmware image, an arena (the setup and status
//       slots, the service's sink page) and the SMMU page directory with its tables
//   J1  NVJPG's SMMU domain (ASID 125 for the "nvjpg" swgroup, through TegraSMMU: the kernel buffers 1:1, two empty
//       windows), power partition 26, clock (PLLC2 at 422.4 MHz on this unit, or PLLP's 408 MHz), reset, the MBIST
//       workaround, falcon firmware loaded and started
//   J2  proof of life through host1x channel 0 in NVJPG's class: an IMMEDIATE syncpoint increment (the engine's host
//       interface) then an OP_DONE one (the falcon firmware), each completed by interrupt
//   J3  the AppleJPEGDriver service and its AppleJPEGDriverUserClient published (TegraNVJPGUserClient.cpp)
// One line per stage ("TegraNVJPG: J1 ok ..."), or the failing stage's FAIL line and nothing after it. Until J3
// publishes, every failure is that one line: the swgroup goes back to bypass, the engine back to reset with its clock
// off, and nothing is published (CMPhoto and VideoToolbox find no hardware JPEG codec and decode on the CPU). No job
// runs before J3. From then a job past its bound is cancelled with one line (channel torn down, memory client flushed,
// engine reset, falcon rebooted and proven) and fails, and the next job runs; a lost syncpoint interrupt panics in
// TegraHost1x.
//
// What the engine does (kTraits) is what the unit measured on every boot of 11-05 and 11-06 (23 boots), decoding
// libjpeg-turbo's JPEGs through the SMMU windows with their pages out of order: NV12, I420, BGRA, RGBA, ARGB and grey
// within a few steps of libjpeg-turbo, 4:2:0, 4:2:2, 4:4:4 and grey input, grey to BGRA, 1/2 scaling, whole MCUs
// written past the picture. 1/4 and 1/8 scaling missed libjpeg-turbo's BGRA by up to 7 and 15 steps on every one of
// those boots and stay off. '422f', '444f' and 'yuvf' are the 4:2:0 path's other chroma and memory modes.
//
// Why the SMMU: a decode's destination is another process's IOSurface (scattered pages, half of the Switch's RAM above
// the 32-bit boundary) and NVJPG has no scatter-gather, so each job maps the destination page by page into a window of
// NVJPG's address space, and the scan's bytes (copied into a kernel buffer the parser bounded) into another; the rest
// of each window stays unmapped, so the SMMU itself keeps the engine inside the job's memory, behind the checker
// (TegraNVJPGJpeg.h: the extent a decode writes against the destination's planes). switchroot's bl31 leaves every
// swgroup in bypass with the translation bits set.
//
// Firmware: linux-firmware's nvidia/tegra210/nvjpg.bin (firmware/SOURCE), booted non-securely as Linux drm/tegra
// nvjpg.c does with falcon_boot (no FCE section, no clock gating register of its own, unlike VIC).
//
// Sources (Linux's drivers are GPL-2.0-only and oss-nvjpg GPL-3.0, so this follows them without copying):
//   tegra210.dtsi nvjpg@54380000 (0x40000; clock and reset TEGRA210_CLK_NVJPG 195 = CAR Y bank bit 3, power domain
//   nvjpg; no interrupt line: completions are host1x syncpoints), include/soc/tegra/pmc.h TEGRA_POWERGATE_NVJPG 26,
//   clk-tegra-periph.c "nvjpg" (CLK_SOURCE_NVJPG 0x69c, mux pllc2/c/c3/pllp/...: PLLP is code 4, as NVDEC),
//   L4T r32.7.4 powergate-t21x.c (partition 26, its MC client flush and release, the MBIST workaround on T210 before
//   B01 only) and t210/cg_regs.c (the prod registers), through SwitchEnginePower.h,
//   memory/tegra/tegra210.c (swgroup "nvjpg" ASID register 0xac0; clients nvjpgsrd/nvjpgswr: translation bits
//   0x234 bits 30/31; MC hot reset NVJPG 0x970 bit 8), drm/tegra/nvjpg.c (tegra210, host1x class 0xc0) and falcon.c
//   falcon_boot (shared with TegraNVDEC/TegraNVENC/TegraVIC), iommu/tegra-smmu.c (as TegraVIC). The job's methods:
//   nvidia/cle7d0.h, in the order averne's FFmpeg nvtegra MJPEG submits them on the Switch.
//   Clock: PLLC2, as L4T's c2bus, at the rate tegra210b01-core-dvfs.c allows NVJPG at VDD_CORE's 637 mV minimum for this
//   chip's SoC process (422.4 MHz for process 1), or PLLP's 408 MHz where that is lower or PLLC2 is not ours
//   (kits/SwitchCAR/SwitchEngineClock.h).
// Waits on hardware sequencing with no interrupt of its own (partition status, IMEM/DMEM scrub, the falcon's DMA
// queue) are bounded reads, as in TegraNVDEC/TegraVIC; every completion of work is a host1x syncpoint interrupt.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "TegraNVJPG.h"
#include "nvidia/cle7d0.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchEngineClock.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../kits/SwitchEngineGate/SwitchEngineGateKernel.h"
#include "../../kits/SwitchEngineGate/SwitchEnginePower.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchMath/SwitchMath.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
int tegra_nvdec_await(uint32_t timeoutMs);
IOLock *tegra_nvdec_power_lock(void);
extern const uint8_t tegra_nvjpg_fw_start[] __asm__("_tegra_nvjpg_fw_start");
extern const uint8_t tegra_nvjpg_fw_end[] __asm__("_tegra_nvjpg_fw_end");
}

namespace {
constexpr uint64_t kNvjpgPA = 0x54380000, kNvjpgSize = 0x40000;
constexpr uint32_t kClass = TEGRA_HOST1X_CLASS_NVJPG;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data)
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kPartition = 26;                           // TEGRA_POWERGATE_NVJPG
constexpr unsigned kClock = 195;                              // TEGRA210_CLK_NVJPG: CAR bank Y bit 3, also its reset
constexpr unsigned kNvdecClock = 194;                         // TEGRA210_CLK_NVDEC: NVJPG's slcg clock
constexpr uint64_t kArenaBytes = 3ULL * SWITCH_HW_PAGE_SIZE;  // the setup slot, the status slot, the service's sink
constexpr uint32_t kNvdecWaitMs = 20000;
constexpr uint32_t kAsid = TEGRA_SMMU_ASID_NVJPG;  // 125 (VIC 127, the display 126)
uint32_t gClockKHz;

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_Y = 0x298,
    RST_DEVICES_Y = 0x2a4,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_NVJPG = 0x69c,
    OVRE_NVJPG_MBIST = (1u << 9) | (1u << 31),
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    PWRGATE_STATUS = 0x38,
};

enum : uint32_t {                                 // the SMMU's own registers are TegraSMMU.h's
    TRANSLATION_NVJPG = (1u << 30) | (1u << 31),  // nvjpgsrd, nvjpgswr (MC_SMMU_TRANSLATION_ENABLE_3)
    MC_INTSTATUS = 0x0,
    MC_ERR_STATUS = 0x8,
    MC_ERR_ADR = 0xc,
    MC_HOTRESET_CTRL_1 = 0x970,
    MC_HOTRESET_STATUS_1 = 0x974,
    MC_HOTRESET_NVJPG = 1u << 8,
};

switch_falcon_image gImage;

volatile uint8_t *gJpg, *gMc;
switch_dma_buffer gFirmwareDma, gArenaDma, gTablesDma;
TegraNVJPGBuffer gFirmware, gArena, gTables;
uint64_t gArenaUsed;
char gLine[256];

// SMMU domain: one page directory, the windows' tables (fixed) and the identity tables of the kernel buffers.
constexpr uint32_t kWindowTables[TEGRA_NVJPG_WINDOWS] = {static_cast<uint32_t>(kTegraNVJPGWindowBytes[0] >> 22),
                                                         static_cast<uint32_t>(kTegraNVJPGWindowBytes[1] >> 22)};
constexpr uint32_t kIdentityTables = 4;  // the arena and the firmware: at most two 4 MiB sections each
constexpr uint32_t kTables = 1 + kWindowTables[0] + kWindowTables[1] + kIdentityTables;
TegraSMMUDomain gDomain;  // directory = page 0, the windows' tables pages 1.., then the identity tables
uint32_t gWindowMapped[TEGRA_NVJPG_WINDOWS];  // pages mapped from each window's start

// The job engine. A job's wait is bounded by kQueueUs (NVJPG shares host1x channel 0 with NVDEC, NVENC and VIC, whose
// queued jobs may run first: up to 2 s by their own bounds) plus one microsecond per 16 pixels of the MCU grid (NVJPG
// does several pixels per clock at 408 MHz or more; the firmware's GP timer, on for every job, ends a picture it cannot finish
// with an error status), at most kMaxBoundUs: only a hung engine gets there.
constexpr uint64_t kQueueUs = 2000000, kMaxBoundUs = 30000000;

inline uint32_t jobBoundUs(uint64_t pixels)
{
    const uint64_t us = kQueueUs + pixels / 16;
    return static_cast<uint32_t>(us < kMaxBoundUs ? us : kMaxBoundUs);
}

IOLock *gJobLock;    // one job at a time: map, run, unmap
IOLock *gStateLock;  // power transitions against jobs
SwitchEngineGate<SwitchEngineGateKernel> gGate;
uint32_t gSyncpt, gNext, gPicture;
uint32_t gPowerOns;     // by the idle gate, since boot
uint64_t gPoweredAtUs;  // the last of them
bool gUp, gWedged, gSuspended;
TegraNVJPGBuffer gSetup, gStatus;
const TegraNVJPGTraits kTraits = {
    .clipped = false,
    .formats = (1u << TEGRA_NVJPG_OUT_COUNT) - 1,
    .chroma = 1u << TEGRA_NVJPG_CHROMA_MONO | 1u << TEGRA_NVJPG_CHROMA_420 | 1u << TEGRA_NVJPG_CHROMA_422H |
              1u << TEGRA_NVJPG_CHROMA_444,
    .scales = 0x3,
    .greyRgb = true,
};

const char *falconState()
{
    static char text[160];
    if (gUp && !gGate.powered())
        return "falcon powered off (idle)";
    return switch_falcon_state(gJpg, text, sizeof(text));
}

// One physically contiguous buffer below 4 GiB, zeroed and clean in the caches.
bool contiguous(uint64_t bytes, switch_dma_buffer *dma, TegraNVJPGBuffer *out)
{
    if (!switch_dma_alloc(dma, bytes))
        return false;
    *out = {dma->pa, dma->size, dma->va};
    return true;
}

inline uint32_t *table(uint32_t index)
{
    return gDomain.table(index);
}

inline uint32_t windowTable(uint32_t window)
{
    return 1 + (window ? kWindowTables[0] : 0);
}

void smmuFlush()
{
    tegra_smmu_flush(kAsid);
}

// The directory, the windows' tables (fixed, empty; installed first, so they are the consecutive pages 1..) and the
// identity maps of the kernel buffers.
bool smmuSetUp()
{
    if (!gDomain.init(gTables.va, gTables.pa, kTables)) {
        snprintf(gLine, sizeof(gLine), "J1 FAIL the SMMU table buffer at %#llx cannot hold a directory",
                 static_cast<unsigned long long>(gTables.pa));
        return false;
    }
    for (uint32_t w = 0; w < TEGRA_NVJPG_WINDOWS; w++) {
        if (!gDomain.reserve(kTegraNVJPGWindowBase[w], kTegraNVJPGWindowBytes[w])) {
            snprintf(gLine, sizeof(gLine), "J1 FAIL no SMMU tables for window %u", w);
            return false;
        }
    }
    const bool identities =
        gDomain.identity(gFirmware.pa, gFirmware.size, false) && gDomain.identity(gArena.pa, gArena.size, true);
    switch_dcache_clean(gTables.va, gTables.size);
    uintptr_t low = 0;
    uintptr_t high = 0;
    gDomain.takeDirty(&low, &high);  // all of it was just cleaned
    if (!identities) {
        snprintf(gLine, sizeof(gLine), "J1 FAIL kernel buffers need more than %u identity tables", kIdentityTables);
        return false;
    }
    return true;
}

// Programs the ASID's directory and points the swgroup at it (also after SC7, which resets the MC).
bool smmuAttach()
{
    char why[160];
    if (tegra_smmu_attach(kAsid, gDomain.ptb(), TEGRA_SMMU_NVJPG_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3,
                          TRANSLATION_NVJPG, why, sizeof(why))) {
        snprintf(gLine, sizeof(gLine), "J1 FAIL nvjpg swgroup: %s", why);
        return false;
    }
    return true;
}

void smmuDetach()
{
    if (!gMc)
        return;
    tegra_smmu_detach(kAsid, TEGRA_SMMU_NVJPG_ASID);
}

// NVJPG's partition and memory client in L4T's order (SwitchEnginePower.h), then its clock-gating prod registers
// (t210/cg_regs.c t21x_nvjpg_gating_registers). The MBIST workaround (LVL2 bits 9 and 31 with NVDEC's clock, NVJPG's
// slcg clock) runs only on T210 before B01, as powergate-t21x.c's. NVDEC's partition (25) needs NVJPG's (26) on
// (powergate-t21x.c powers 26 before 25 and keeps it while 25 is on): an idle power-off leaves 26 on while 25 is.
constexpr unsigned kSlcgClocks[] = {kNvdecClock};
const SwitchEnginePower kPower = {kPartition,         kClock,           MC_HOTRESET_CTRL_1, MC_HOTRESET_STATUS_1,
                                  MC_HOTRESET_NVJPG,  OVRE_NVJPG_MBIST, kSlcgClocks,        1,
                                  tegra_mc_hot_reset, tegra_smmu_read};
constexpr SwitchEngineProd kProd[] = {
    {0xe00, 0},         // THI_CLK_OVERRIDE
    {0x88, 0},          // THI_SLCG_OVERRIDE_HIGH_A
    {0x8c, 0},          // THI_SLCG_OVERRIDE_LOW_A
    {0x10a0, 0},        // FALCON_CGCTL
    {0x1404, 0},        // TFBIF_MCCIF_FIFOCTRL
    {0x117c, 0x30008},  // FALCON_CG2
};
constexpr unsigned kNvdecPartition = 25;

bool powerUp(bool *wasOn)
{
    switch_car_write(CLK_SOURCE_NVJPG, switch_engine_clock_source(SWITCH_ENGINE_BUS_C2, &gClockKHz));
    IODelay(2);
    if (!switch_engine_power_up(kPower, wasOn)) {
        snprintf(gLine, sizeof(gLine),
                 "J1 FAIL partition %u did not come up (PWRGATE_STATUS %#x CLAMP_STATUS %#x CLK_OUT_ENB_Y %#x "
                 "RST_DEVICES_Y %#x)",
                 kPartition, switch_pmc_read(PWRGATE_STATUS), switch_pmc_read(0x2c), switch_car_read(CLK_OUT_ENB_Y),
                 switch_car_read(RST_DEVICES_Y));
        return false;
    }
    switch_engine_prod(gJpg, kProd, sizeof(kProd) / sizeof(kProd[0]));
    return true;
}

// falcon_boot minus its final idle poll: that the firmware runs is shown by J2's OP_DONE increment instead. The falcon
// fetches through NVJPG's SMMU domain, where the firmware sits 1:1 (and its OS loads its apps from there later).
bool bootFalcon()
{
    switch (switch_falcon_load(gJpg, gFirmware.pa, &gImage)) {
    case SWITCH_FALCON_SCRUBBING:
        snprintf(gLine, sizeof(gLine), "J1 FAIL IMEM/DMEM scrubbing did not end (DMACTL %#x)",
                 switch_mmio_read32(gJpg, SWITCH_FALCON_DMACTL));
        return false;
    case SWITCH_FALCON_DMA:
        snprintf(gLine, sizeof(gLine), "J1 FAIL firmware DMA did not finish (DMATRFCMD %#x)",
                 switch_mmio_read32(gJpg, SWITCH_FALCON_DMATRFCMD));
        return false;
    case SWITCH_FALCON_OK:
        break;
    }
    switch_falcon_start(gJpg);
    return true;
}

// Channel 0 in NVJPG's class: an IMMEDIATE increment is done by the engine's host interface on arrival, an OP_DONE
// one only once the falcon reports the methods before it done.
int answer(uint32_t id, uint32_t from, const char **stage)
{
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(kClass, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, id),
    };
    int ret =
        switch_host1x_job_copy(TEGRA_HOST1X_CHANNEL_NVJPG, stream, sizeof(stream) / sizeof(stream[0]), id, from + 2);
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

void engineOff()
{
    switch_car_reset_assert(kClock);
    IODelay(2);
    switch_car_clock_disable(kClock);
}

// Back to bypass, reset and clock off after a failed bring-up: nothing will use the engine this boot.
void park()
{
    smmuDetach();
    engineOff();
}

// A job past its bound: the engine stopped making progress on it (a lost interrupt panics in TegraHost1x instead).
// As L4T's nvhost does on a channel timeout, the job is cancelled, not the engine: channel 3 is torn down so nothing of
// the stream reaches the engine again, the memory client is flushed (tegra_mc_flush), the engine reset with its
// partition kept on and brought up again as at J1 (powerUp releases the flush once the reset is), the falcon rebooted
// and proven as at J2, and the job fails; the next one runs. One line per cancellation, with what the
// falcon and the memory controller held. gStateLock held, and gJobLock (the caller's): no NVJPG power transition can
// be waiting for gStateLock while this takes the power lock (gateOn runs only in a job, gateOff only with no user, and
// suspend first takes gJobLock).
int cancelled(const TegraNVJPGJob *job, uint32_t fence, uint64_t us)
{
    char host[200];
    char falcon[400];
    char what[200];
    const nvjpg_dec_drv_pic_setup_s &s = job->setup;
    snprintf(
        what, sizeof(what),
        "selector %u, %dx%d (MCU %dx%d), stream chroma %d to chroma %d pixel format %d memory mode %d block height "
        "%d, %d bitstream bytes, %u planes, target format %u",
        job->selector, s.frame_width, s.frame_height, s.mcu_width, s.mcu_height, s.stream_chroma_mode,
        s.output_chroma_mode, s.output_pixel_format, s.memory_mode, s.block_linear_height, s.bitstream_size,
        job->planes, job->target);
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_NVJPG, host, sizeof(host));
    const uint32_t reached = tegra_host1x_syncpt_read(gSyncpt);
    switch_falcon_stall(gJpg, falcon, sizeof(falcon));
    const uint32_t mcStatus = switch_mmio_read32(gMc, MC_INTSTATUS), mcError = switch_mmio_read32(gMc, MC_ERR_STATUS),
                   mcAddress = switch_mmio_read32(gMc, MC_ERR_ADR);
    tegra_host1x_channel_teardown(TEGRA_HOST1X_CHANNEL_NVJPG);
    IOLock *power = tegra_nvdec_power_lock();
    IOLockLock(power);
    gLine[0] = 0;
    bool wasOn = false;
    const bool drained = switch_engine_power::flush(kPower);
    switch_car_reset_assert(kClock);
    IODelay(10);
    const bool reset = powerUp(&wasOn);
    tegra_host1x_channel_restart(TEGRA_HOST1X_CHANNEL_NVJPG);
    const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
    const char *stage = nullptr;
    const bool back = reset && bootFalcon() && !answer(gSyncpt, from, &stage);
    IOLockUnlock(power);
    gNext = tegra_host1x_syncpt_read(gSyncpt);
    gWedged = !back;
    if (!back)
        engineOff();
    IOLog("TegraNVJPG: job fence %u not reached in %llu ms (syncpoint %u at %u; powered on by the idle gate %u times, "
          "the last %llu ms ago); job %s; %s; MC INTSTATUS %#x ERR_STATUS %#x ERR_ADR %#x; %s; job cancelled, memory "
          "client %s, engine reset: %s%s\n",
          fence, us / 1000, gSyncpt, reached, gPowerOns,
          gPowerOns ? static_cast<unsigned long long>((switch_now_us() - gPoweredAtUs) / 1000) : 0ULL, what, falcon,
          mcStatus, mcError, mcAddress, host, drained ? "drained" : "did not drain in 10 ms",
          back ? "running again" : "it did not come back, NVJPG refuses work (decodes go to the CPU): ",
          back ? ""
               : (gLine[0] ? gLine
                  : stage  ? stage
                           : "falcon"));
    return -5;
}

void engineSuspend();
void engineResume();
int gateOn(void *);
int gateOff(void *, bool);

bool allocate()
{
    const size_t length =
        reinterpret_cast<uintptr_t>(tegra_nvjpg_fw_end) - reinterpret_cast<uintptr_t>(tegra_nvjpg_fw_start);
    if (!switch_falcon_parse(tegra_nvjpg_fw_start, length, &gImage)) {
        snprintf(gLine, sizeof(gLine), "J0 FAIL embedded firmware is not a falcon v1 image (%lu bytes)",
                 static_cast<unsigned long>(length));
        return false;
    }
    if (!contiguous(switch_align_up(gImage.size, SWITCH_HW_PAGE_SIZE), &gFirmwareDma, &gFirmware) ||
        !contiguous(kArenaBytes, &gArenaDma, &gArena) ||
        !contiguous(static_cast<uint64_t>(kTables) * SWITCH_HW_PAGE_SIZE, &gTablesDma, &gTables)) {
        snprintf(gLine, sizeof(gLine), "J0 FAIL no contiguous firmware, arena or page-table memory below 4 GiB");
        return false;
    }
    memcpy(gFirmware.va, tegra_nvjpg_fw_start, gImage.size);
    switch_dcache_clean(gFirmware.va, gImage.size);
    if (tegra_nvjpg_buffer(sizeof(nvjpg_dec_drv_pic_setup_s), &gSetup) ||
        tegra_nvjpg_buffer(sizeof(nvjpg_dec_status), &gStatus)) {
        snprintf(gLine, sizeof(gLine), "J0 FAIL no setup or status slot in the arena");
        return false;
    }
    return true;
}

void giveBack()
{
    switch_dma_free(&gFirmwareDma);
    switch_dma_free(&gArenaDma);
    switch_dma_free(&gTablesDma);
}

void bringUp(void *, wait_result_t)
{
    // NVDEC first, whatever it decides: its clock is NVJPG's MBIST workaround clock (powerUp leaves NVDEC's clock as it
    // finds it).
    (void)tegra_nvdec_await(kNvdecWaitMs);
    if (tegra_host1x_start()) {
        IOLog("TegraNVJPG: J0 FAIL host1x is not up (see the TegraHost1x line)\n");
        return;
    }
    gJpg = switch_mmio_map(kNvjpgPA, kNvjpgSize);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gJobLock = IOLockAlloc();
    gStateLock = IOLockAlloc();
    IOLock *power = tegra_nvdec_power_lock();
    if (!gJpg || !gMc || !gJobLock || !gStateLock || !power) {
        IOLog("TegraNVJPG: J0 FAIL no mapping\n");
        return;
    }
    gLine[0] = 0;
    const uint64_t began = switch_now_us();
    if (!allocate()) {
        IOLog("TegraNVJPG: %s\n", gLine);
        giveBack();
        return;
    }
    IOLog("TegraNVJPG: J0 ok firmware %u bytes at %#llx, arena %llu KiB at %#llx, %u page tables, in %llu us\n",
          gImage.size, gFirmware.pa, kArenaBytes >> 10, gArena.pa, kTables, switch_now_us() - began);
    bool wasOn = false;
    // NVDEC's gate may power NVDEC off or on meanwhile, switching the clock powerUp's MBIST workaround borrows.
    IOLockLock(power);
    const bool up = smmuSetUp() && smmuAttach() && powerUp(&wasOn) && bootFalcon();
    IOLockUnlock(power);
    if (!up) {
        IOLog("TegraNVJPG: %s\n", gLine[0] ? gLine : "J1 FAIL");
        park();
        giveBack();
        return;
    }
    IOLog("TegraNVJPG: J1 ok ASID %u (directory %#llx), partition %u %s, %u kHz, falcon started (OS code %#x+%#x data "
          "%#x+%#x, %u apps)\n",
          kAsid, gTables.pa, kPartition, wasOn ? "already on" : "powered here", gClockKHz, gImage.code,
          gImage.code_size, gImage.data, gImage.data_size, gImage.apps);
    uint32_t id = 0;
    if (tegra_host1x_syncpt_alloc(&id)) {
        IOLog("TegraNVJPG: J2 FAIL no syncpoint left\n");
        park();
        giveBack();
        return;
    }
    const uint32_t from = tegra_host1x_syncpt_read(id);
    const char *stage = nullptr;
    const uint64_t start = switch_now_us();
    const int ret = answer(id, from, &stage);
    if (ret) {
        char host[200];
        tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_NVJPG, host, sizeof(host));
        IOLog("TegraNVJPG: J2 FAIL %s %s syncpoint %u (%u->%u, %d); %s; %s\n", stage,
              ret == TEGRA_HOST1X_ELOST ? "incremented, but no interrupt came for" : "did not increment", id, from,
              tegra_host1x_syncpt_read(id), ret, falconState(), host);
        park();
        giveBack();
        return;
    }
    IOLog("TegraNVJPG: J2 ok host interface and falcon incremented syncpoint %u %u->%u by interrupt in %llu us; %s\n",
          id, from, tegra_host1x_syncpt_read(id), switch_now_us() - start, falconState());
    gSyncpt = id;
    gNext = tegra_host1x_syncpt_read(id);
    if (!gGate.start({"TegraNVJPG", nullptr, gateOn, gateOff}, power)) {
        IOLog("TegraNVJPG: J2 FAIL no idle timer\n");
        park();
        giveBack();
        return;
    }
    gUp = true;
    tegra_host1x_set_sleep_client(engineSuspend, engineResume);
    tegra_host1x_set_strict();
    tegra_nvjpg_publish();  // a FAIL line is its own; the engine stays up, unused
}
}  // namespace

extern "C" bool tegra_nvjpg_ready(void)
{
    return gUp && !gWedged;
}

extern "C" const TegraNVJPGTraits *tegra_nvjpg_traits(void)
{
    return &kTraits;
}

extern "C" int tegra_nvjpg_buffer(uint64_t size, TegraNVJPGBuffer *out)
{
    size = switch_align_up(size, SWITCH_HW_PAGE_SIZE);
    if (!gArena.va || gArenaUsed + size > gArena.size)
        return -12;
    out->pa = gArena.pa + gArenaUsed;
    out->va = gArena.va + gArenaUsed;
    out->size = size;
    gArenaUsed += size;
    return 0;
}

extern "C" void tegra_nvjpg_lock(void)
{
    IOLockLock(gJobLock);
}

extern "C" void tegra_nvjpg_unlock(void)
{
    IOLockUnlock(gJobLock);
}

// Job lock held. The window's tables are fixed; pages past `count` are left (or made) unmapped, so an access beyond
// the job's memory faults in the SMMU instead of reaching memory.
extern "C" uint64_t tegra_nvjpg_window_map(uint32_t window, const uint64_t *pages, uint32_t count, bool writable)
{
    if (window >= TEGRA_NVJPG_WINDOWS || count > (uint32_t)(kTegraNVJPGWindowBytes[window] >> SWITCH_HW_PAGE_SHIFT))
        return 0;
    uint32_t *pt = table(windowTable(window));  // the window's tables are consecutive pages
    for (uint32_t i = 0; i < count; i++)
        pt[i] = TegraSMMUDomain::entry(pages[i], writable);
    for (uint32_t i = count; i < gWindowMapped[window]; i++)
        pt[i] = 0;
    switch_dcache_clean(pt, (uint64_t)(count > gWindowMapped[window] ? count : gWindowMapped[window]) * 4);
    gWindowMapped[window] = count;
    smmuFlush();
    return kTegraNVJPGWindowBase[window];
}

extern "C" void tegra_nvjpg_window_clear(uint32_t window)
{
    if (window >= TEGRA_NVJPG_WINDOWS || !gWindowMapped[window])
        return;
    uint32_t *pt = table(windowTable(window));
    bzero(pt, (uint64_t)gWindowMapped[window] * 4);
    switch_dcache_clean(pt, (uint64_t)gWindowMapped[window] * 4);
    gWindowMapped[window] = 0;
    smmuFlush();
}

// Job lock held. The setup and status slots are the kernel's (no client maps them); the stream is SETCLASS, the
// decoder's methods through THI METHOD0/METHOD1 in averne's order (application, control with the GP timer on, picture
// index, setup, bitstream, status, output planes, EXECUTE with AWAKEN), then an OP_DONE increment of NVJPG's
// syncpoint, waited for by interrupt.
extern "C" int tegra_nvjpg_run(const TegraNVJPGJob *job, nvjpg_dec_status *status, uint64_t *busyUs)
{
    if (!gUp || !job->planes || job->planes > 3)
        return -19;
    if ((job->bitstream & 0xff) || (job->plane[0] & 0xff) || (job->planes > 1 && (job->plane[1] & 0xff)) ||
        (job->planes > 2 && (job->plane[2] & 0xff)))
        return -22;
    if (const int error = gGate.use())
        return error;
    IOLockLock(gStateLock);
    while (gSuspended)
        IOLockSleep(gStateLock, &gSuspended, THREAD_UNINT);
    if (gWedged) {
        IOLockUnlock(gStateLock);
        gGate.done();
        return -19;
    }
    memcpy(gSetup.va, &job->setup, sizeof(job->setup));
    switch_dcache_clean_lines(gSetup.va, sizeof(job->setup));
    bzero(gStatus.va, sizeof(nvjpg_dec_status));
    switch_dcache_clean_invalidate(gStatus.va, sizeof(nvjpg_dec_status));
    uint32_t *words = nullptr;
    if (const int error = switch_host1x_job_begin(TEGRA_HOST1X_CHANNEL_NVJPG, 1 + 3 * 11 + 1, &words)) {
        IOLockUnlock(gStateLock);
        gGate.done();
        return error;
    }
    uint32_t n = 0;
    words[n++] = tegra_host1x_op_setclass(kClass, 0, 0);
    const auto method = [&](uint32_t offset, uint32_t value) {
        words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
        words[n++] = offset / 4;
        words[n++] = value;
    };
    method(NVE7D0_SET_APPLICATION_ID, NVE7D0_SET_APPLICATION_ID_ID_NVJPG_DECODER);
    method(NVE7D0_SET_CONTROL_PARAMS, 1u << 0 | 1u << 1);  // GPTIMER_ON, DUMP_CYCLE_COUNT
    method(NVE7D0_SET_PICTURE_INDEX, gPicture++);
    method(NVE7D0_SET_IN_DRV_PIC_SETUP, (uint32_t)(gSetup.pa >> 8));
    method(NVE7D0_SET_BITSTREAM, (uint32_t)(job->bitstream >> 8));
    method(NVE7D0_SET_OUT_STATUS, (uint32_t)(gStatus.pa >> 8));
    method(NVE7D0_SET_CUR_PIC, (uint32_t)(job->plane[0] >> 8));
    if (job->planes > 1)
        method(NVE7D0_SET_CUR_PIC_CHROMA_U, (uint32_t)(job->plane[1] >> 8));
    if (job->planes > 2)
        method(NVE7D0_SET_CUR_PIC_CHROMA_V, (uint32_t)(job->plane[2] >> 8));
    method(NVE7D0_EXECUTE, 1u << 8);  // AWAKEN
    words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    const uint32_t fence = gNext + 1;
    const uint64_t start = switch_now_us();
    int ret = switch_host1x_job_submit(TEGRA_HOST1X_CHANNEL_NVJPG, words, n, gSyncpt, fence);
    if (!ret) {
        gNext = fence;
        ret = tegra_host1x_syncpt_wait(gSyncpt, fence, jobBoundUs(job->pixels));
        if (ret == TEGRA_HOST1X_ETIMEDOUT)
            ret = cancelled(job, fence, switch_now_us() - start);
    }
    if (busyUs)
        *busyUs = switch_now_us() - start;
    switch_dcache_clean_invalidate(gStatus.va, sizeof(nvjpg_dec_status));
    memcpy(status, gStatus.va, sizeof(*status));
    IOLockUnlock(gStateLock);
    gGate.done();
    return ret;
}

// Idle power gating (SwitchEngineGate.h), as mainline's runtime PM does for VIC and NVDEC: off 500 ms after the last
// decode, on at the next. On is J1 and J2 again on the kept image and SMMU domain (an idle power-off does not reset the
// MC): partition, clock, reset and the MBIST workaround, the falcon reloaded and proven with J2's stream. A failure
// there is one line and NVJPG refuses work (decodes go to the CPU). Off waits for no job (a decode holds the gate) and
// powers the partition down. NVDEC's gate shares the lock (tegra_nvdec_power_lock): each one's power-up borrows the
// other's clock for its MBIST workaround.
namespace {
int gateOn(void *)
{
    IOLockLock(gStateLock);
    bool wasOn = false;
    bool answered = false;
    gLine[0] = 0;
    if (powerUp(&wasOn) && bootFalcon()) {
        const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
        const char *stage = nullptr;
        const int ret = answer(gSyncpt, from, &stage);
        answered = !ret;
        if (ret) {
            char state[160];
            snprintf(gLine, sizeof(gLine), "%s did not answer (%d, syncpoint %u %u->%u); %s", stage, ret, gSyncpt, from,
                     tegra_host1x_syncpt_read(gSyncpt), switch_falcon_state(gJpg, state, sizeof(state)));
        }
    }
    if (!answered) {
        gWedged = true;
        engineOff();
        IOLog("TegraNVJPG: power-on FAIL %s; NVJPG refuses work (decodes go to the CPU)\n", gLine[0] ? gLine : "J1");
    }
    gPowerOns++;
    gPoweredAtUs = switch_now_us();
    gNext = tegra_host1x_syncpt_read(gSyncpt);
    gSuspended = false;
    IOLockWakeup(gStateLock, &gSuspended, false);
    IOLockUnlock(gStateLock);
    return answered ? 0 : -19;
}

int gateOff(void *, bool)
{
    IOLockLock(gStateLock);
    gSuspended = true;
    const bool off =
        switch_pmc_partition_is_on(kNvdecPartition) ? switch_engine_quiesce(kPower) : switch_engine_power_down(kPower);
    IOLockUnlock(gStateLock);
    return off ? SWITCH_ENGINE_GATE_OFF : -5;
}

// Sleep (SC7), through TegraHost1x's sleep clients. Suspend waits for the job lock (no job runs, none maps a window),
// powers the engine off and drops the swgroup to bypass before host1x saves its syncpoints; new jobs wait for resume.
// Resume runs after host1x (and NVDEC, an earlier client) is back and programs the SMMU domain again (SC7 resets the
// MC); the next decode powers NVJPG on.
void engineSuspend()
{
    IOLockLock(gJobLock);
    gGate.suspend();
    smmuDetach();
    IOLockUnlock(gJobLock);
}

void engineResume()
{
    gLine[0] = 0;
    const bool attached = smmuAttach();
    if (!attached) {
        IOLockLock(gStateLock);
        gWedged = true;
        gSuspended = false;
        IOLockWakeup(gStateLock, &gSuspended, false);
        IOLockUnlock(gStateLock);
        IOLog("TegraNVJPG: resume FAIL %s; NVJPG refuses work (decodes go to the CPU)\n", gLine);
    }
    gGate.resume(attached);
}
}  // namespace

// On by default (no boot-arg).
extern "C" int tegra_nvjpg_probe(void)
{
    static volatile int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST))
        return 0;
    thread_t thread = nullptr;
    if (kernel_thread_start(bringUp, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVJPG: could not start the bring-up thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
