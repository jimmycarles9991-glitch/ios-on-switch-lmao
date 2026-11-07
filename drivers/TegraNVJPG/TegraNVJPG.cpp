// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 NVJPG (JPEG codec 1.0, a falcon behind host1x): bring-up, its SMMU domain, the job engine and sleep.
// tegra_nvjpg_probe() runs the whole bring-up by default on its own thread:
//   J0  NVDEC's bring-up has ended (NVJPG's MBIST workaround runs on NVDEC's clock),
//       host1x is up, and NVJPG's kernel buffers are allocated: the firmware image, an arena (the setup and status
//       slots, the self-test's pictures) and the SMMU page directory with its tables
//   J1  NVJPG's SMMU domain (ASID 125 for the "nvjpg" swgroup, through TegraSMMU: the kernel buffers 1:1, two empty
//       windows), power partition 26, clock (PLLP, 408 MHz), reset, the MBIST workaround, falcon firmware loaded and
//       started
//   J2  proof of life through host1x channel 0 in NVJPG's class: an IMMEDIATE syncpoint increment (the engine's host
//       interface) then an OP_DONE one (the falcon firmware), each completed by interrupt
//   J3  decodes of embedded JPEGs checked against libjpeg-turbo's decode of the same bytes, the SMMU's translation and
//       the engine's write extent measured (TegraNVJPGSelfTest.cpp)
//   J4  the AppleJPEGDriver service and its AppleJPEGDriverUserClient published (TegraNVJPGUserClient.cpp)
// One line per stage ("TegraNVJPG: J1 ok ..."), or the failing stage's FAIL line and nothing after it. Until J4
// publishes, every failure is that one line: the swgroup goes back to bypass, the engine back to reset with its clock
// off, and nothing is published (CMPhoto and VideoToolbox find no hardware JPEG codec and decode on the CPU). From J4
// on the engine is proven: a job past its bound panics with one line, and a lost syncpoint interrupt panics in
// TegraHost1x.
//
// Why the SMMU: a decode's destination is another process's IOSurface (scattered pages, half of the Switch's RAM above
// the 32-bit boundary) and NVJPG has no scatter-gather, so each job maps the destination page by page into a window of
// NVJPG's address space, and the scan's bytes (copied into a kernel buffer the parser bounded) into another; the rest
// of each window stays unmapped, so the SMMU itself keeps the engine inside the job's memory, behind the checker
// (TegraNVJPGJpeg.h: the extent a decode writes against the destination's planes). switchroot's bl31 leaves every
// swgroup in bypass with the translation bits set; J3 proves translation by decoding through a window whose pages are
// mapped out of order.
//
// Firmware: linux-firmware's nvidia/tegra210/nvjpg.bin (firmware/SOURCE), booted non-securely as Linux drm/tegra
// nvjpg.c does with falcon_boot (no FCE section, no clock gating register of its own, unlike VIC).
//
// Sources (Linux's drivers are GPL-2.0-only and oss-nvjpg GPL-3.0, so this follows them without copying):
//   tegra210.dtsi nvjpg@54380000 (0x40000; clock and reset TEGRA210_CLK_NVJPG 195 = CAR Y bank bit 3, power domain
//   nvjpg; no interrupt line: completions are host1x syncpoints), include/soc/tegra/pmc.h TEGRA_POWERGATE_NVJPG 26,
//   clk-tegra-periph.c "nvjpg" (CLK_SOURCE_NVJPG 0x69c, mux pllc2/c/c3/pllp/...: PLLP is code 4, as NVDEC),
//   clk-tegra210.c tegra210_pg_mbist_war[NVJPG] (generic: LVL2_CLK_GATE_OVRE bits 9 and 31 pulsed with NVDEC's clock
//   running: nvjpg_slcg_clkids), pmc.c tegra_powergate_power_up (reset, partition, clock, clamps, reset, MBIST),
//   memory/tegra/tegra210.c (swgroup "nvjpg" ASID register 0xac0; clients nvjpgsrd/nvjpgswr: translation bits
//   0x234 bits 30/31; MC hot reset NVJPG 0x970 bit 8), drm/tegra/nvjpg.c (tegra210, host1x class 0xc0) and falcon.c
//   falcon_boot (shared with TegraNVDEC/TegraNVENC/TegraVIC), iommu/tegra-smmu.c (as TegraVIC). The job's methods:
//   nvidia/cle7d0.h, in the order averne's FFmpeg nvtegra MJPEG submits them on the Switch.
//   Clock: PLLP undivided, 408 MHz, below the lowest T210B01 DVFS point of the NVDEC/NVJPG rail, so it is safe at any
//   core voltage the boot chain left (TegraNVDEC.cpp makes the same choice).
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
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchMath/SwitchMath.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
int tegra_nvdec_await(uint32_t timeoutMs);
extern const uint8_t tegra_nvjpg_fw_start[] __asm__("_tegra_nvjpg_fw_start");
extern const uint8_t tegra_nvjpg_fw_end[] __asm__("_tegra_nvjpg_fw_end");
}

namespace {
constexpr uint64_t kNvjpgPA = 0x54380000, kNvjpgSize = 0x40000;
constexpr uint32_t kClass = TEGRA_HOST1X_CLASS_NVJPG;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data)
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kPartition = 26;    // TEGRA_POWERGATE_NVJPG
constexpr unsigned kClock = 195;       // TEGRA210_CLK_NVJPG: CAR bank Y bit 3, also its reset
constexpr unsigned kNvdecClock = 194;  // TEGRA210_CLK_NVDEC: NVJPG's slcg clock
constexpr uint64_t kArenaBytes = 1ull << 20;
constexpr uint32_t kNvdecWaitMs = 20000;
constexpr uint32_t kAsid = TEGRA_SMMU_ASID_NVJPG;  // 125 (VIC 127, the display 126)
constexpr uint32_t kClockMHz = 408;

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_Y = 0x298,
    RST_DEVICES_Y = 0x2a4,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_NVJPG = 0x69c,
    OVRE_NVJPG_MBIST = (1u << 9) | (1u << 31),
    SOURCE_PLLP = 4u << 29,
    NVJPG_DIV_1 = 0,  // 7.1 divider: 408 MHz * 2 / (0 + 2) = 408 MHz
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    PWRGATE_STATUS = 0x38,
};

enum : uint32_t {                                 // the SMMU's own registers are TegraSMMU.h's
    TRANSLATION_NVJPG = (1u << 30) | (1u << 31),  // nvjpgsrd, nvjpgswr (MC_SMMU_TRANSLATION_ENABLE_3)
    MC_HOTRESET_CTRL_1 = 0x970,
    MC_HOTRESET_NVJPG = 1u << 8,
};

switch_falcon_image gImage;

volatile uint8_t *gJpg, *gMc;
switch_dma_buffer gFirmwareDma, gArenaDma, gTablesDma;
TegraNVJPGBuffer gFirmware, gArena, gTables;
uint64_t gArenaUsed;
char gLine[256];

// SMMU domain: one page directory, the windows' tables (fixed) and the identity tables of the kernel buffers.
constexpr uint32_t kWindowTables[TEGRA_NVJPG_WINDOWS] = {(uint32_t)(kTegraNVJPGWindowBytes[0] >> 22),
                                                         (uint32_t)(kTegraNVJPGWindowBytes[1] >> 22)};
constexpr uint32_t kIdentityTables = 4;  // the arena and the firmware: at most two 4 MiB sections each
constexpr uint32_t kTables = 1 + kWindowTables[0] + kWindowTables[1] + kIdentityTables;
TegraSMMUDomain gDomain;  // directory = page 0, the windows' tables pages 1.., then the identity tables
uint32_t gWindowMapped[TEGRA_NVJPG_WINDOWS];  // pages mapped from each window's start

// The job engine. A job's wait is bounded by kQueueUs (NVJPG shares host1x channel 0 with NVDEC, NVENC and VIC, whose
// queued jobs may run first: up to 2 s by their own bounds) plus one microsecond per 16 pixels of the MCU grid (NVJPG
// does several pixels per clock at 408 MHz; the firmware's GP timer, on for every job, ends a picture it cannot finish
// with an error status), at most kMaxBoundUs: only a hung engine gets there.
constexpr uint64_t kQueueUs = 2000000, kMaxBoundUs = 30000000;

inline uint32_t jobBoundUs(uint64_t pixels)
{
    const uint64_t us = kQueueUs + pixels / 16;
    return (uint32_t)(us < kMaxBoundUs ? us : kMaxBoundUs);
}

IOLock *gJobLock;    // one job at a time: map, run, unmap
IOLock *gStateLock;  // suspend/resume against jobs
uint32_t gSyncpt, gNext, gPicture;
bool gUp, gWedged, gSuspended, gPublished;
TegraNVJPGBuffer gSetup, gStatus;
TegraNVJPGTraits gTraits;

const char *falconState()
{
    static char text[160];
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

// The table entries just written, to memory (the SMMU does not snoop the CPU's caches).
void cleanDomain()
{
    uintptr_t low, high;
    if (gDomain.takeDirty(&low, &high))
        switch_dcache_clean(reinterpret_cast<const void *>(low), high - low);
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
                 (unsigned long long)gTables.pa);
        return false;
    }
    for (uint32_t w = 0; w < TEGRA_NVJPG_WINDOWS; w++)
        if (!gDomain.reserve(kTegraNVJPGWindowBase[w], kTegraNVJPGWindowBytes[w])) {
            snprintf(gLine, sizeof(gLine), "J1 FAIL no SMMU tables for window %u", w);
            return false;
        }
    const bool identities =
        gDomain.identity(gFirmware.pa, gFirmware.size, false) && gDomain.identity(gArena.pa, gArena.size, true);
    switch_dcache_clean(gTables.va, gTables.size);
    uintptr_t low, high;
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

// tegra_powergate_power_up with the T210 MBIST workaround (tegra210_generic_mbist_war on NVJPG's slcg clock, NVDEC's).
// A partition the boot chain left on keeps its state; the engine is still reset and re-clocked so the falcon starts
// from scratch.
bool powerUp(bool *wasOn)
{
    *wasOn = switch_pmc_partition_is_on(kPartition);
    switch_car_reset_assert(kClock);
    IODelay(10);
    if (!*wasOn) {
        if (!switch_pmc_partition_on(kPartition)) {
            snprintf(gLine, sizeof(gLine), "J1 FAIL partition %u did not power on (PWRGATE_TOGGLE %#x STATUS %#x)",
                     kPartition, switch_pmc_read(PWRGATE_TOGGLE), switch_pmc_read(PWRGATE_STATUS));
            return false;
        }
        IODelay(10);
    }
    switch_car_write(CLK_SOURCE_NVJPG, SOURCE_PLLP | NVJPG_DIV_1);
    IODelay(2);
    switch_car_clock_enable(kClock);
    IODelay(10);
    switch_pmc_remove_clamping(kPartition);
    IODelay(10);
    // MC_HOTRESET_CTRL_1 holds NVDEC's and the GPU's bits too: read-modify-write only when NVJPG's is set (NVDEC's
    // bring-up, the other writer of this register at boot, has ended by now).
    if (switch_mmio_read32(gMc, MC_HOTRESET_CTRL_1) & MC_HOTRESET_NVJPG)
        switch_mmio_write32(gMc, MC_HOTRESET_CTRL_1, switch_mmio_read32(gMc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_NVJPG);
    switch_car_reset_deassert(kClock);
    IODelay(10);
    // tegra210_generic_mbist_war with NVJPG's slcg clock (NVDEC's) running for the pulse; NVDEC's own state is kept,
    // and only NVJPG's override bits change, under the CAR lock.
    const bool nvdecOn = switch_car_clock_enabled(kNvdecClock);
    if (!nvdecOn)
        switch_car_clock_enable(kNvdecClock);
    const uint32_t overridden = switch_car_read(LVL2_CLK_GATE_OVRE) & OVRE_NVJPG_MBIST;
    switch_car_rmw(LVL2_CLK_GATE_OVRE, 0, OVRE_NVJPG_MBIST);
    IODelay(1);
    switch_car_rmw(LVL2_CLK_GATE_OVRE, OVRE_NVJPG_MBIST & ~overridden, 0);
    IODelay(1);
    if (!nvdecOn)
        switch_car_clock_disable(kNvdecClock);
    if (!switch_car_clock_enabled(kClock) || switch_car_reset_asserted(kClock)) {
        snprintf(gLine, sizeof(gLine), "J1 FAIL clock or reset stuck (CLK_OUT_ENB_Y %#x RST_DEVICES_Y %#x)",
                 switch_car_read(CLK_OUT_ENB_Y), switch_car_read(RST_DEVICES_Y));
        return false;
    }
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
    int ret = tegra_host1x_submit(stream, sizeof(stream) / sizeof(stream[0]));
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

void timedOut(uint32_t fence, uint64_t us)
{
    char host[200];
    tegra_host1x_describe(host, sizeof(host));
    if (gPublished)
        panic("TegraNVJPG: job fence %u not reached in %llu ms (syncpoint %u at %u); %s; %s", fence, us / 1000, gSyncpt,
              tegra_host1x_syncpt_read(gSyncpt), falconState(), host);
    if (gWedged)
        return;
    gWedged = true;
    IOLog("TegraNVJPG: job fence %u not reached in %llu us (syncpoint %u at %u); %s; %s\n", fence, us, gSyncpt,
          tegra_host1x_syncpt_read(gSyncpt), falconState(), host);
}

void engineSuspend();
void engineResume();

bool allocate()
{
    const size_t length = (size_t)(tegra_nvjpg_fw_end - tegra_nvjpg_fw_start);
    if (!switch_falcon_parse(tegra_nvjpg_fw_start, length, &gImage)) {
        snprintf(gLine, sizeof(gLine), "J0 FAIL embedded firmware is not a falcon v1 image (%lu bytes)",
                 (unsigned long)length);
        return false;
    }
    if (!contiguous(switch_align_up(gImage.size, SWITCH_HW_PAGE_SIZE), &gFirmwareDma, &gFirmware) ||
        !contiguous(kArenaBytes, &gArenaDma, &gArena) ||
        !contiguous((uint64_t)kTables * SWITCH_HW_PAGE_SIZE, &gTablesDma, &gTables)) {
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
    if (!gJpg || !gMc || !gJobLock || !gStateLock) {
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
    if (!smmuSetUp() || !smmuAttach() || !powerUp(&wasOn) || !bootFalcon()) {
        IOLog("TegraNVJPG: %s\n", gLine[0] ? gLine : "J1 FAIL");
        park();
        giveBack();
        return;
    }
    IOLog("TegraNVJPG: J1 ok ASID %u (directory %#llx), partition %u %s, %u MHz, falcon started (OS code %#x+%#x data "
          "%#x+%#x, %u apps)\n",
          kAsid, gTables.pa, kPartition, wasOn ? "already on" : "powered here", kClockMHz, gImage.code,
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
        tegra_host1x_describe(host, sizeof(host));
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
    gUp = true;
    if (tegra_nvjpg_self_test(&gTraits) || gWedged) {  // J3; its FAIL line is out
        gUp = false;
        park();
        giveBack();
        return;
    }
    tegra_host1x_set_sleep_client(engineSuspend, engineResume);
    tegra_host1x_set_strict();
    // Proven: from here a job past its bound panics (TegraNVJPG.h).
    gPublished = true;
    if (tegra_nvjpg_publish())
        gPublished = false;  // its FAIL line is out; the engine stays up, unused
}
}  // namespace

extern "C" bool tegra_nvjpg_ready(void)
{
    return gUp && !gWedged;
}

extern "C" const TegraNVJPGTraits *tegra_nvjpg_traits(void)
{
    return &gTraits;
}

extern "C" uint32_t tegra_nvjpg_clock_mhz(void)
{
    return kClockMHz;
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

extern "C" void tegra_nvjpg_cache_pages(switch_dcache_op op, const uint64_t *pages, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (switch_dma_pa_managed(pages[i]))
            switch_dcache_phys_lines(op, pages[i] & ~SWITCH_HW_PAGE_MASK, SWITCH_HW_PAGE_SIZE);
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

extern "C" void tegra_nvjpg_identity_override(uint64_t iova, uint64_t pa)
{
    uint32_t *p = gDomain.pte(iova, true);
    if (!p)
        return;
    *p = TegraSMMUDomain::entry(pa, true);
    gDomain.touch(p, sizeof(*p));
    cleanDomain();
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
    IOLockLock(gStateLock);
    while (gSuspended)
        IOLockSleep(gStateLock, &gSuspended, THREAD_UNINT);
    if (gWedged) {
        IOLockUnlock(gStateLock);
        return -19;
    }
    memcpy(gSetup.va, &job->setup, sizeof(job->setup));
    switch_dcache_clean_lines(gSetup.va, sizeof(job->setup));
    bzero(gStatus.va, sizeof(nvjpg_dec_status));
    switch_dcache_clean_invalidate(gStatus.va, sizeof(nvjpg_dec_status));
    uint32_t words[1 + 3 * 11 + 1], n = 0;
    words[n++] = tegra_host1x_op_setclass(kClass, 0, 0);
    auto method = [&](uint32_t offset, uint32_t value) {
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
    int ret = tegra_host1x_submit(words, n);
    if (!ret) {
        gNext = fence;
        ret = tegra_host1x_syncpt_wait(gSyncpt, fence, jobBoundUs(job->pixels));
        if (ret == TEGRA_HOST1X_ETIMEDOUT)
            timedOut(fence, switch_now_us() - start);
    }
    if (busyUs)
        *busyUs = switch_now_us() - start;
    switch_dcache_clean_invalidate(gStatus.va, sizeof(nvjpg_dec_status));
    memcpy(status, gStatus.va, sizeof(*status));
    IOLockUnlock(gStateLock);
    return ret;
}

// Sleep (SC7), through TegraHost1x's sleep clients. Suspend waits for the job lock (no job runs), parks the engine in
// reset and drops the swgroup to bypass before host1x saves its syncpoints; new jobs block until resume. Resume runs
// after host1x (and NVDEC, an earlier client, whose clock the MBIST workaround borrows) is back: the SMMU domain is
// programmed again (SC7 resets the MC), the partition powered, the falcon reloaded from the kept image and proven
// with J2's stream.
namespace {
void engineSuspend()
{
    IOLockLock(gJobLock);
    IOLockLock(gStateLock);
    gSuspended = true;
    smmuDetach();
    engineOff();
    IOLockUnlock(gStateLock);
    IOLockUnlock(gJobLock);
}

void engineResume()
{
    IOLockLock(gStateLock);
    bool wasOn = false;
    gLine[0] = 0;
    gWedged = true;
    if (smmuAttach() && powerUp(&wasOn) && bootFalcon()) {
        const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
        const char *stage = nullptr;
        const int ret = answer(gSyncpt, from, &stage);
        if (!ret)
            gWedged = false;
        else
            snprintf(gLine, sizeof(gLine), "%s did not answer (%d, syncpoint %u %u->%u); %s", stage, ret, gSyncpt, from,
                     tegra_host1x_syncpt_read(gSyncpt), falconState());
    }
    if (gWedged)
        IOLog("TegraNVJPG: resume FAIL %s; NVJPG refuses work (decodes go to the CPU)\n", gLine);
    gNext = tegra_host1x_syncpt_read(gSyncpt);
    gSuspended = false;
    IOLockWakeup(gStateLock, &gSuspended, false);
    IOLockUnlock(gStateLock);
}
}  // namespace

// On by default (no boot-arg).
extern "C" int tegra_nvjpg_probe(void)
{
    static volatile int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST))
        return 0;
    thread_t thread;
    if (kernel_thread_start(bringUp, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVJPG: could not start the bring-up thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
