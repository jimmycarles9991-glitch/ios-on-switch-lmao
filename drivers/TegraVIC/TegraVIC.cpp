// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 VIC (Video Image Compositor 4.0, a falcon behind host1x): bring-up, its SMMU domain, the job engine and
// sleep. tegra_vic_probe() runs the whole bring-up by default on its own thread:
//   V0  host1x is up, and VIC's kernel buffers are allocated: the firmware image, an arena for the configuration slot
//       and the filter structure, and the SMMU page directory with its page tables
//   V1  VIC's SMMU domain (ASID 127 for the "vic" swgroup: the kernel buffers 1:1, two empty surface windows), power
//       partition 23, clock (PLLP, 408 MHz), reset, the VIC MBIST workaround, falcon firmware loaded and started
//   V2  proof of life through host1x channel 0 in VIC's class: an IMMEDIATE syncpoint increment (the engine's host
//       interface), then the firmware's FCE microcode size and offset and an OP_DONE increment (the falcon), each
//       completed by interrupt
//   V3  the AppleM2ScalerCSCDriver service and its IOSurfaceAcceleratorClient published (TegraVICUserClient.cpp)
// One line per stage ("TegraVIC: V1 ok ..."), or the failing stage's FAIL line and nothing after it. Until V3
// publishes, every failure is that one line: the swgroup goes back to bypass, the engine back to reset with its clock
// off, and nothing is published (Core Animation has no IOSurfaceAccelerator, as before this driver). No job runs
// before V3, and from then a job past its bound panics with one line, and a lost syncpoint interrupt panics in
// TegraHost1x. What the engine does with each configuration is kTegraVICTraits (TegraVICConfig.h), measured on the
// unit by a boot self-test that came out the same on every logged boot.
//
// Why the SMMU: the scaler's operands are other processes' IOSurfaces (scattered pages, half of the Switch's RAM above
// the 32-bit boundary), and VIC has no scatter-gather. Each job maps its two surfaces page by page into two windows of
// VIC's address space; everything else in the windows is unmapped, so the SMMU itself keeps the engine inside the job's
// surfaces, behind the checker (TegraVICConfig.h) that keeps every programmed access inside them. switchroot's bl31
// (tegra_memctrl_setup) turns the SMMU on with every client's translation bit set and every swgroup in bypass; the
// global enable is secure-only and reads back as garbage from here; on every logged boot (31) a fill whose page table
// entry sent it to another page landed in that page, not at the bus address.
//
// Firmware: linux-firmware's nvidia/tegra210/vic04_ucode.bin (firmware/SOURCE), booted non-securely as Linux drm/tegra
// vic.c does; its FCE microcode is handed over with two methods (VIC_SET_FCE_UCODE_SIZE/OFFSET), as vic_boot does for
// this firmware generation.
//
// Sources (Linux's drivers are GPL-2.0-only and hekate's GPL-2.0, so this follows them without copying):
//   tegra210.dtsi vic@54340000 (0x40000; clock and reset TEGRA210_CLK_VIC03 178 = CAR X bank bit 18, power domain VIC),
//   include/soc/tegra/pmc.h TEGRA_POWERGATE_VIC 23 (hekate POWER_RAIL_VIC 23), clk-tegra-periph.c "vic03"
//   (CLK_SOURCE_VIC03 0x678, mux pllc/pllp/...: PLLP is code 2; hekate _clock_vic: PLLP, divider 0, 408 MHz),
//   clk-tegra210.c tegra210_vic_mbist_war (LVL2_CLK_GATE_OVRE bit 5, THI SLCG_OVERRIDE_LOW 0x8c bits 0, 7:2, 24),
//   drm/tegra/vic.c vic_boot (NV_PVIC_MISC_PRI_VIC_CG 0x16d0, FCE header/data at firmware words 6/7, size at +8),
//   drm/tegra/falcon.c falcon_boot (shared with TegraNVDEC/TegraNVENC), memory/tegra/tegra210.c (swgroup "vic" ASID
//   register 0x284, clients vicsrd/vicswr translation bits 0x234 bits 12/13, MC hot reset VIC 0x200 bit 18),
//   iommu/tegra-smmu.c (PTB_ASID 0x1c, PTB_DATA 0x20, TLB_FLUSH 0x30, PTC_FLUSH 0x34, PDE/PTE: PFN | R 31 | W 30 |
//   NS 29, NEXT 28 in a PDE; 1024 x 4 MiB directory entries, 1024 x 4 KiB table entries), switch-atf memctrl_v1.c.
// Waits on hardware sequencing with no interrupt of its own (partition status, IMEM/DMEM scrub, the falcon's DMA
// queue) are bounded reads, as in TegraNVDEC/TegraNVENC; every completion of work is a host1x syncpoint interrupt.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "TegraVIC.h"
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
extern const uint8_t tegra_vic_fw_start[] __asm__("_tegra_vic_fw_start");
extern const uint8_t tegra_vic_fw_end[] __asm__("_tegra_vic_fw_end");
}

namespace {
constexpr uint64_t kVicPA = 0x54340000, kVicSize = 0x40000;
constexpr uint32_t kClass = TEGRA_HOST1X_CLASS_VIC;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data)
constexpr uint64_t kMcPA = 0x70019000;
constexpr uint32_t kPartition = 23;                           // TEGRA_POWERGATE_VIC
constexpr unsigned kClock = 178;                              // TEGRA210_CLK_VIC03: CAR bank X bit 18, also its reset
constexpr uint64_t kArenaBytes = 4ULL * SWITCH_HW_PAGE_SIZE;  // the configuration slot and the filter (0x3000)
constexpr uint32_t kAsid = TEGRA_SMMU_ASID_VIC;

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_X = 0x280,
    RST_DEVICES_X = 0x28c,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_VIC = 0x678,
    OVRE_VIC_MBIST = 1u << 5,
    SOURCE_PLLP = 2u << 29,  // this mux: pllc 1, pllp 2
    VIC_DIV_1 = 0,
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    PWRGATE_STATUS = 0x38,
};

enum : uint32_t {  // the SMMU's own registers are TegraSMMU.h's
    MC_HOTRESET_CTRL = 0x200,
    MC_HOTRESET_VIC = 1u << 18,
    TRANSLATION_VIC = (1u << 12) | (1u << 13),  // vicsrd, vicswr in TRANSLATION_ENABLE_3
};

enum : uint32_t {  // VIC private registers
    THI_SLCG_OVERRIDE_LOW = 0x8c,
    SLCG_MBIST = (1u << 0) | (0x3fu << 2) | (1u << 24),
    MISC_PRI_VIC_CG = 0x16d0,
    VIC_CG_BOOT = 4u | (1u << 6) | (4u << 16),  // CG_IDLE_CG_DLY_CNT(4) | CG_IDLE_CG_EN | CG_WAKEUP_DLY_CNT(4)
};

// The firmware image (kits/SwitchFalcon) and VIC's FCE microcode inside it (drm/tegra/vic.h).
switch_falcon_image gImage;
uint32_t gFceSize;

volatile uint8_t *gVic, *gMc;
switch_dma_buffer gFirmwareDma, gArenaDma, gTablesDma;
TegraVICBuffer gFirmware, gArena, gTables;
uint64_t gArenaUsed;
char gLine[256];

// SMMU domain: one page directory, the identity tables of the kernel buffers and the windows' tables.
constexpr uint32_t kWindowTables = static_cast<uint32_t>(kTegraVICWindowBytes >> 22);  // 4 MiB per table
constexpr uint32_t kIdentityTables = 4;  // the arena and the firmware: at most two 4 MiB sections each
constexpr uint32_t kTables = 1 + TEGRA_VIC_WINDOWS * kWindowTables + kIdentityTables;
TegraSMMUDomain gDomain;  // directory = page 0, the windows' tables pages 1.., then the identity tables
uint32_t gWindowMapped[TEGRA_VIC_WINDOWS];  // pages mapped from each window's start

// The job engine. A job's wait is bounded by kQueueUs plus one microsecond per 8 pixels read and written (VIC does
// several per clock at 408 MHz), at most kMaxBoundUs: only a hung engine gets there. A job that first waits for the
// copy engine (an acquire) gets kAcquireBoundUs more, a whole 128 MiB window's copy several times over.
constexpr uint64_t kQueueUs = 2000000, kMaxBoundUs = 30000000, kAcquireBoundUs = 2000000;

inline uint32_t jobBoundUs(const TegraVICJob *job)
{
    const uint64_t us = kQueueUs + job->pixels / 8;
    return static_cast<uint32_t>(us < kMaxBoundUs ? us : kMaxBoundUs);
}

IOLock *gJobLock;    // one job at a time: map, run, unmap
IOLock *gStateLock;  // suspend/resume against jobs
uint32_t gSyncpt, gNext;
bool gUp, gWedged, gSuspended;
TegraVICBuffer gConfig, gFilter;

const char *falconState()
{
    static char text[160];
    return switch_falcon_state(gVic, text, sizeof(text));
}

// One physically contiguous buffer below 4 GiB, zeroed and clean in the caches.
bool contiguous(uint64_t bytes, switch_dma_buffer *dma, TegraVICBuffer *out)
{
    if (!switch_dma_alloc(dma, bytes))
        return false;
    *out = {dma->pa, dma->size, dma->va};
    return true;
}

// falcon_parse_firmware_image (kits/SwitchFalcon), plus vic_boot's FCE header: the FCE microcode inside the file.
bool parse(const uint8_t *fw, size_t length)
{
    if (!switch_falcon_parse(fw, length, &gImage))
        return false;
    // This firmware generation needs its FCE microcode handed over (vic_boot: data offset neither 0 nor 0xa5a5a5a5).
    if (!gImage.fce_header || gImage.fce_header > gImage.size - 12 || !gImage.fce_data ||
        gImage.fce_data == 0xa5a5a5a5u)
        return false;
    gFceSize = switch_falcon_le32(fw + gImage.fce_header + 8);
    return (gImage.fce_data & 0xff) == 0 && (gFceSize != 0u) && gImage.fce_data <= gImage.size &&
           gFceSize <= gImage.size - gImage.fce_data;
}

// --- SMMU domain (TegraSMMUDomain.h; the registers under TegraSMMU.h's lock) ---------------------------------------
inline uint32_t *table(uint32_t index)
{
    return gDomain.table(index);
}

void smmuFlush()
{
    tegra_smmu_flush(kAsid);
}

bool identity(const TegraVICBuffer &b, bool writable)
{
    return gDomain.identity(b.pa, b.size, writable);
}

// The directory, the windows' tables (fixed, empty; installed first, so they are the consecutive pages 1..) and the
// identity maps; then the ASID's directory and the swgroup.
bool smmuSetUp()
{
    if (!gDomain.init(gTables.va, gTables.pa, kTables)) {
        snprintf(gLine, sizeof(gLine), "V1 FAIL the SMMU table buffer at %#llx cannot hold a directory",
                 static_cast<unsigned long long>(gTables.pa));
        return false;
    }
    for (uint32_t w = 0; w < TEGRA_VIC_WINDOWS; w++) {
        if (!gDomain.reserve(kTegraVICWindowBase + w * kTegraVICWindowBytes, kTegraVICWindowBytes)) {
            snprintf(gLine, sizeof(gLine), "V1 FAIL no SMMU tables for window %u", w);
            return false;
        }
    }
    const bool identities = identity(gFirmware, false) && identity(gArena, true);
    switch_dcache_clean(gTables.va, gTables.size);
    uintptr_t low = 0;
    uintptr_t high = 0;
    gDomain.takeDirty(&low, &high);  // all of it was just cleaned
    if (!identities) {
        snprintf(gLine, sizeof(gLine), "V1 FAIL kernel buffers need more than %u identity tables", kIdentityTables);
        return false;
    }
    return true;
}

// Programs the ASID's directory and points the swgroup at it (also after SC7, which resets the MC).
bool smmuAttach()
{
    char why[160];
    if (tegra_smmu_attach(kAsid, gDomain.ptb(), TEGRA_SMMU_VIC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3, TRANSLATION_VIC,
                          why, sizeof(why))) {
        snprintf(gLine, sizeof(gLine), "V1 FAIL vic swgroup: %s", why);
        return false;
    }
    return true;
}

void smmuDetach()
{
    if (!gMc)
        return;
    tegra_smmu_detach(kAsid, TEGRA_SMMU_VIC_ASID);
}

// --- Power, clock, falcon ----------------------------------------------------------------------------------------
// tegra_powergate_power_up with the VIC MBIST workaround (it needs host1x's clock, which the boot chain left on). A
// partition the boot chain left on keeps its state; the engine is still reset and re-clocked so the falcon starts from
// scratch.
bool powerUp(bool *wasOn)
{
    *wasOn = switch_pmc_partition_is_on(kPartition);
    switch_car_reset_assert(kClock);
    IODelay(10);
    if (!*wasOn) {
        if (!switch_pmc_partition_on(kPartition)) {
            snprintf(gLine, sizeof(gLine), "V1 FAIL partition %u did not power on (PWRGATE_TOGGLE %#x STATUS %#x)",
                     kPartition, switch_pmc_read(PWRGATE_TOGGLE), switch_pmc_read(PWRGATE_STATUS));
            return false;
        }
        IODelay(10);
    }
    switch_car_write(CLK_SOURCE_VIC, SOURCE_PLLP | VIC_DIV_1);
    IODelay(2);
    switch_car_clock_enable(kClock);
    IODelay(10);
    switch_pmc_remove_clamping(kPartition);
    IODelay(10);
    if (switch_mmio_read32(gMc, MC_HOTRESET_CTRL) & MC_HOTRESET_VIC)
        switch_mmio_write32(gMc, MC_HOTRESET_CTRL, switch_mmio_read32(gMc, MC_HOTRESET_CTRL) & ~MC_HOTRESET_VIC);
    switch_car_reset_deassert(kClock);
    IODelay(10);
    if (!switch_car_clock_enabled(kClock) || switch_car_reset_asserted(kClock)) {
        snprintf(gLine, sizeof(gLine), "V1 FAIL clock or reset stuck (CLK_OUT_ENB_X %#x RST_DEVICES_X %#x)",
                 switch_car_read(CLK_OUT_ENB_X), switch_car_read(RST_DEVICES_X));
        return false;
    }
    // tegra210_vic_mbist_war: VIC's override bit pulsed under the CAR lock, the register's other bits left alone.
    const bool overridden = (switch_car_read(LVL2_CLK_GATE_OVRE) & OVRE_VIC_MBIST) != 0u;
    switch_car_rmw(LVL2_CLK_GATE_OVRE, 0, OVRE_VIC_MBIST);
    IODelay(1);
    const uint32_t slcg = switch_mmio_read32(gVic, THI_SLCG_OVERRIDE_LOW);
    switch_mmio_write32(gVic, THI_SLCG_OVERRIDE_LOW, slcg | SLCG_MBIST);
    IODelay(1);
    switch_mmio_write32(gVic, THI_SLCG_OVERRIDE_LOW, slcg);
    (void)switch_mmio_read32(gVic, THI_SLCG_OVERRIDE_LOW);
    if (!overridden)
        switch_car_rmw(LVL2_CLK_GATE_OVRE, OVRE_VIC_MBIST, 0);
    IODelay(1);
    return true;
}

// vic_boot's clock gating, then falcon_boot minus its final idle poll: that the firmware runs is shown by V2's OP_DONE
// increment instead. The falcon fetches through VIC's SMMU domain, where the firmware sits 1:1.
bool bootFalcon()
{
    switch_mmio_write32(gVic, MISC_PRI_VIC_CG, VIC_CG_BOOT);
    switch (switch_falcon_load(gVic, gFirmware.pa, &gImage)) {
    case SWITCH_FALCON_SCRUBBING:
        snprintf(gLine, sizeof(gLine), "V1 FAIL IMEM/DMEM scrubbing did not end (DMACTL %#x)",
                 switch_mmio_read32(gVic, SWITCH_FALCON_DMACTL));
        return false;
    case SWITCH_FALCON_DMA:
        snprintf(gLine, sizeof(gLine), "V1 FAIL firmware DMA did not finish (DMATRFCMD %#x)",
                 switch_mmio_read32(gVic, SWITCH_FALCON_DMATRFCMD));
        return false;
    case SWITCH_FALCON_OK:
        break;
    }
    switch_falcon_start(gVic);
    return true;
}

// Channel 0 in VIC's class: an IMMEDIATE increment is done by the engine's host interface on arrival; then the FCE
// microcode's size and address (vic_boot), and an OP_DONE increment the falcon reports once it has taken them.
int answer(uint32_t id, uint32_t from, const char **stage)
{
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(kClass, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id),
        tegra_host1x_op_incr(kThiMethod0 / 4, 2),
        TEGRA_VIC_M_SET_FCE_UCODE_SIZE / 4,
        gFceSize,
        tegra_host1x_op_incr(kThiMethod0 / 4, 2),
        TEGRA_VIC_M_SET_FCE_UCODE_OFFSET / 4,
        static_cast<uint32_t>((gFirmware.pa + gImage.fce_data) >> 8),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, id),
    };
    int ret = tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_VIC, stream, sizeof(stream) / sizeof(stream[0]));
    *stage = "submit";
    if (!ret) {
        *stage = "host interface (IMMEDIATE)";
        ret = tegra_host1x_syncpt_wait(id, from + 1, 100000);
    }
    if (!ret) {
        *stage = "falcon (FCE methods, OP_DONE)";
        ret = tegra_host1x_syncpt_wait(id, from + 2, 500000);
    }
    return ret;
}

// Back to bypass, reset and clock off after a failed bring-up: nothing will use the engine this boot.
void park()
{
    smmuDetach();
    switch_car_reset_assert(kClock);
    IODelay(2);
    switch_car_clock_disable(kClock);
}

[[noreturn]] void timedOut(uint32_t fence, uint64_t us)
{
    char host[200];
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_VIC, host, sizeof(host));
    panic("TegraVIC: job fence %u not reached in %llu ms (syncpoint %u at %u); %s; %s", fence, us / 1000, gSyncpt,
          tegra_host1x_syncpt_read(gSyncpt), falconState(), host);
}

void engineSuspend();
void engineResume();

bool allocate()
{
    const size_t length =
        reinterpret_cast<uintptr_t>(tegra_vic_fw_end) - reinterpret_cast<uintptr_t>(tegra_vic_fw_start);
    if (!parse(tegra_vic_fw_start, length)) {
        snprintf(gLine, sizeof(gLine), "V0 FAIL embedded firmware is not a VIC falcon v1 image with FCE (%lu bytes)",
                 static_cast<unsigned long>(length));
        return false;
    }
    if (!contiguous(switch_align_up(gImage.size, SWITCH_HW_PAGE_SIZE), &gFirmwareDma, &gFirmware) ||
        !contiguous(kArenaBytes, &gArenaDma, &gArena) ||
        !contiguous(static_cast<uint64_t>(kTables) * SWITCH_HW_PAGE_SIZE, &gTablesDma, &gTables)) {
        snprintf(gLine, sizeof(gLine), "V0 FAIL no contiguous firmware, arena or page-table memory below 4 GiB");
        return false;
    }
    memcpy(gFirmware.va, tegra_vic_fw_start, gImage.size);
    switch_dcache_clean(gFirmware.va, gImage.size);
    return tegra_vic_buffer(SWITCH_HW_PAGE_SIZE, &gConfig) == 0 && tegra_vic_buffer(0x3000, &gFilter) == 0;
}

void giveBack()
{
    switch_dma_free(&gFirmwareDma);
    switch_dma_free(&gArenaDma);
    switch_dma_free(&gTablesDma);
}

void bringUp(void *, wait_result_t)
{
    if (tegra_host1x_start()) {
        IOLog("TegraVIC: V0 FAIL host1x is not up (see the TegraHost1x line)\n");
        return;
    }
    gVic = switch_mmio_map(kVicPA, kVicSize);
    gMc = switch_mmio_map(kMcPA, 0x1000);
    gJobLock = IOLockAlloc();
    gStateLock = IOLockAlloc();
    if (!gVic || !gMc || !gJobLock || !gStateLock) {
        IOLog("TegraVIC: V0 FAIL no mapping\n");
        return;
    }
    gLine[0] = 0;
    const uint64_t began = switch_now_us();
    if (!allocate()) {
        IOLog("TegraVIC: %s\n", gLine[0] ? gLine : "V0 FAIL no configuration slot in the arena");
        giveBack();
        return;
    }
    IOLog("TegraVIC: V0 ok firmware %u bytes at %#llx (FCE %#x+%#x), arena %llu KiB at %#llx, %u page tables, in %llu "
          "us\n",
          gImage.size, gFirmware.pa, gImage.fce_data, gFceSize, kArenaBytes >> 10, gArena.pa, kTables,
          switch_now_us() - began);
    bool wasOn = false;
    if (!smmuSetUp() || !smmuAttach() || !powerUp(&wasOn) || !bootFalcon()) {
        IOLog("TegraVIC: %s\n", gLine[0] ? gLine : "V1 FAIL");
        park();
        giveBack();
        return;
    }
    IOLog("TegraVIC: V1 ok ASID %u (directory %#llx), partition %u %s, 408 MHz, falcon started (OS code %#x+%#x data "
          "%#x+%#x)\n",
          kAsid, gTables.pa, kPartition, wasOn ? "already on" : "powered here", gImage.code, gImage.code_size,
          gImage.data, gImage.data_size);
    uint32_t id = 0;
    if (tegra_host1x_syncpt_alloc(&id)) {
        IOLog("TegraVIC: V2 FAIL no syncpoint left\n");
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
        tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_VIC, host, sizeof(host));
        IOLog("TegraVIC: V2 FAIL %s %s syncpoint %u (%u->%u, %d); %s; %s\n", stage,
              ret == TEGRA_HOST1X_ELOST ? "incremented, but no interrupt came for" : "did not increment", id, from,
              tegra_host1x_syncpt_read(id), ret, falconState(), host);
        park();
        giveBack();
        return;
    }
    IOLog(
        "TegraVIC: V2 ok host interface and falcon (FCE %u bytes) incremented syncpoint %u %u->%u by interrupt in %llu "
        "us; %s\n",
        gFceSize, id, from, tegra_host1x_syncpt_read(id), switch_now_us() - start, falconState());
    gSyncpt = id;
    gNext = tegra_host1x_syncpt_read(id);
    gUp = true;
    tegra_host1x_set_sleep_client(engineSuspend, engineResume);
    tegra_host1x_set_strict();
    tegra_vic_publish();  // a FAIL line is its own; the engine stays up, unused
}
}  // namespace

extern "C" void tegra_vic_describe(char *buffer, size_t size)
{
    snprintf(buffer, size, "%s", gVic ? falconState() : "VIC not mapped");
}

extern "C" bool tegra_vic_ready(void)
{
    return gUp && !gWedged;
}

extern "C" const TegraVICTraits *tegra_vic_traits(void)
{
    return &kTegraVICTraits;
}

extern "C" int tegra_vic_buffer(uint64_t size, TegraVICBuffer *out)
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

extern "C" uint64_t tegra_vic_config_address(void)
{
    return gConfig.pa;
}

extern "C" uint64_t tegra_vic_filter_address(void)
{
    return gFilter.pa;
}

extern "C" void tegra_vic_lock(void)
{
    IOLockLock(gJobLock);
}

extern "C" void tegra_vic_unlock(void)
{
    IOLockUnlock(gJobLock);
}

// Job lock held. The window's tables are fixed; pages past `count` are left (or made) unmapped, so an access beyond
// the surface faults in the SMMU instead of reaching memory.
extern "C" uint64_t tegra_vic_window_map(uint32_t window, const uint64_t *pages, uint32_t count, bool writable)
{
    const uint64_t base = kTegraVICWindowBase + window * kTegraVICWindowBytes;
    const uint32_t limit = (uint32_t)(kTegraVICWindowBytes >> SWITCH_HW_PAGE_SHIFT);
    if (window >= TEGRA_VIC_WINDOWS || count > limit)
        return 0;
    uint32_t *pt = table(1 + window * kWindowTables);  // the window's tables are consecutive pages
    for (uint32_t i = 0; i < count; i++)
        pt[i] = TegraSMMUDomain::entry(pages[i], writable);
    for (uint32_t i = count; i < gWindowMapped[window]; i++)
        pt[i] = 0;
    switch_dcache_clean(pt,
                        (uint64_t)(count > gWindowMapped[window] ? count : gWindowMapped[window]) *
                            4);  // pages in a row
    gWindowMapped[window] = count;
    smmuFlush();
    return base;
}

extern "C" void tegra_vic_window_clear(uint32_t window)
{
    if (window >= TEGRA_VIC_WINDOWS || !gWindowMapped[window])
        return;
    uint32_t *pt = table(1 + window * kWindowTables);
    bzero(pt, (uint64_t)gWindowMapped[window] * 4);
    switch_dcache_clean(pt, (uint64_t)gWindowMapped[window] * 4);
    gWindowMapped[window] = 0;
    smmuFlush();
}

// Job lock held. The configuration slot is the kernel's (no client maps it); the stream is SETCLASS, the job's methods
// through THI METHOD0/METHOD1, then an OP_DONE increment of VIC's syncpoint, waited for by interrupt. With an acquire,
// VIC's channel first waits on the hardware for another engine's syncpoint (the copy engine filling a bounce scratch).
extern "C" int tegra_vic_run_after(const TegraVICJob *job, const TegraVICAcquire *acquire, uint64_t *busyUs,
                                   uint32_t *fence)
{
    if (!gUp || job->count > TEGRA_VIC_MAX_METHODS)
        return -19;
    IOLockLock(gStateLock);
    while (gSuspended)
        IOLockSleep(gStateLock, &gSuspended, THREAD_UNINT);
    if (gWedged) {
        IOLockUnlock(gStateLock);
        return -19;
    }
    memcpy(gConfig.va, job->config, TEGRA_VIC_CONFIG_BYTES);
    switch_dcache_clean(gConfig.va, TEGRA_VIC_CONFIG_BYTES);
    uint32_t words[2 + 1 + 3 * TEGRA_VIC_MAX_METHODS + 1];
    uint32_t n = 0;
    if (acquire)
        n += tegra_host1x_op_wait_syncpt(words, acquire->syncpt, acquire->threshold);
    words[n++] = tegra_host1x_op_setclass(kClass, 0, 0);
    for (uint32_t i = 0; i < job->count; i++) {
        words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
        words[n++] = job->method[i] / 4;
        words[n++] = job->value[i];
    }
    words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    const uint32_t next = gNext + 1;
    const uint64_t start = switch_now_us();
    int ret = tegra_host1x_submit_on(TEGRA_HOST1X_CHANNEL_VIC, words, n);
    if (!ret) {
        gNext = next;
        ret = tegra_host1x_syncpt_wait(gSyncpt, next, (uint32_t)(jobBoundUs(job) + (acquire ? kAcquireBoundUs : 0)));
        if (ret == TEGRA_HOST1X_ETIMEDOUT)
            timedOut(next, switch_now_us() - start);
    }
    if (busyUs)
        *busyUs = switch_now_us() - start;
    if (fence)
        *fence = next;
    IOLockUnlock(gStateLock);
    return ret;
}

extern "C" int tegra_vic_run(const TegraVICJob *job, uint64_t *busyUs)
{
    return tegra_vic_run_after(job, nullptr, busyUs, nullptr);
}

extern "C" uint32_t tegra_vic_syncpt(void)
{
    return gSyncpt;
}

// Sleep (SC7), through TegraHost1x's sleep clients. Suspend waits for the job lock (no job runs), parks the engine
// in reset and drops the swgroup to bypass before host1x saves its syncpoints; new jobs block until resume. Resume
// runs after host1x is back: the SMMU domain is programmed again (SC7 resets the MC), the partition powered, the falcon
// reloaded from the kept image and proven with the V2 stream.
namespace {
void engineSuspend()
{
    IOLockLock(gJobLock);
    IOLockLock(gStateLock);
    gSuspended = true;
    smmuDetach();
    switch_car_reset_assert(kClock);
    IODelay(2);
    switch_car_clock_disable(kClock);
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
        if (!ret) {
            gWedged = false;
        } else {
            snprintf(gLine, sizeof(gLine), "%s did not answer (%d, syncpoint %u %u->%u); %s", stage, ret, gSyncpt, from,
                     tegra_host1x_syncpt_read(gSyncpt), falconState());
        }
    }
    if (gWedged)
        IOLog("TegraVIC: resume FAIL %s; VIC refuses work\n", gLine[0] ? gLine : "V1");
    gNext = tegra_host1x_syncpt_read(gSyncpt);
    gSuspended = false;
    IOLockWakeup(gStateLock, &gSuspended, false);
    IOLockUnlock(gStateLock);
}
}  // namespace

// On by default (no boot-arg): VIC is the scaler Core Animation needs to clone the display (Screen Recording).
extern "C" int tegra_vic_probe(void)
{
    static volatile int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST))
        return 0;
    thread_t thread = nullptr;
    if (kernel_thread_start(bringUp, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraVIC: could not start the bring-up thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
