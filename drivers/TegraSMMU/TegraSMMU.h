// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra210 memory controller's SMMU registers, shared by every driver that translates a swgroup (TegraVIC, TegraNVJPG,
// TegraNVDEC, the display). Selecting a directory takes two global registers in turn (PTB_ASID, then PTB_DATA) and
// the PTC/TLB flushes are global too, so two drivers programming at once would program each other's ASID: every access
// goes through here, under one lock. Page tables are TegraSMMUDomain.h.
//
// Registers (Linux memory/tegra/tegra210.c, iommu/tegra-smmu.c): PTB_ASID 0x1c, PTB_DATA 0x20, TLB_FLUSH 0x30 (ASID match
// bit 31, ASID in 30:24 here written as asid << 24), PTC_FLUSH 0x34 (bit 0: all or one PTE group, bit 3 kept set),
// PTC_FLUSH_1 0x9b8 (PA[33:32]; Tegra X1 TRM 18.11.1.10, .11, .122). The client translation registers used here are
// TRANSLATION_ENABLE_0..3 (0x228..0x234) and _4 (0xb98), secure-only. Switchroot's bl31 sets the client bits and
// leaves the swgroups in bypass. The global SMMU enable is secure-only too. The engines show whether translation is on
// (TegraVIC.cpp).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ASIDs in use, one swgroup each.
enum tegra_smmu_asid {
    TEGRA_SMMU_ASID_VIC = 127,
    TEGRA_SMMU_ASID_DISPLAY = 126,
    TEGRA_SMMU_ASID_NVJPG = 125,
    TEGRA_SMMU_ASID_XUSB_HOST = 124,
    TEGRA_SMMU_ASID_NVDEC = 123,
    TEGRA_SMMU_ASID_NVENC = 122,
    TEGRA_SMMU_ASID_XUSB_DEV = 121,
    TEGRA_SMMU_ASID_DISPLAY_B = 120,
    TEGRA_SMMU_ASID_HDA = 119,
    TEGRA_SMMU_ASID_AFI = 118,
    TEGRA_SMMU_ASID_GPU = 117,       // GM20B's compaction window (SwitchGpuSmmu.cpp), field 2 of GPUB
    TEGRA_SMMU_ASID_FAULT = 116,     // an empty directory: every access through it faults (tegra_smmu_fault_ptb)
    TEGRA_SMMU_ASID_GPU_LOW = 115,   // GM20B's RAM below 4 GiB at its own address, field 0 of GPUB
    TEGRA_SMMU_ASID_GPU_HIGH = 114,  // GM20B's RAM above 4 GiB at its own address less 4 GiB, field 1 of GPUB
    TEGRA_SMMU_ASID_SDMMC1_LOW = 113,
    TEGRA_SMMU_ASID_SDMMC1_HIGH = 112,
    TEGRA_SMMU_ASID_HOST1X = 111,
    TEGRA_SMMU_ASID_APE = 110,
    TEGRA_SMMU_ASID_SE1 = 109,
    TEGRA_SMMU_ASID_SE2 = 108
};

enum tegra_smmu_register {
    TEGRA_SMMU_TRANSLATION_ENABLE_0 = 0x228,
    TEGRA_SMMU_TRANSLATION_ENABLE_1 = 0x22c,
    TEGRA_SMMU_TRANSLATION_ENABLE_2 = 0x230,
    TEGRA_SMMU_TRANSLATION_ENABLE_3 = 0x234,
    TEGRA_SMMU_TRANSLATION_ENABLE_4 = 0xb98,
    TEGRA_SMMU_AFI_ASID = 0x238,
    TEGRA_SMMU_DC_ASID = 0x240,
    TEGRA_SMMU_DCB_ASID = 0x244,
    TEGRA_SMMU_HDA_ASID = 0x254,
    TEGRA_SMMU_VIC_ASID = 0x284,
    TEGRA_SMMU_XUSB_HOST_ASID = 0x288,  // swgroup "xusb_host" (xusb_hostr/xusb_hostw: TRANSLATION_ENABLE_2 bits 10/11)
    TEGRA_SMMU_XUSB_DEV_ASID = 0x28c,   // swgroup "xusb_dev" (xusb_devr/xusb_devw: TRANSLATION_ENABLE_2 bits 12/13)
    // swgroup GPUB, what GM20B's a[34] = 1 accesses use (gpusrd/gpuswr: _2 bits 24/25; gpusrd2/gpuswr2: _4 bits 8/9; all
    // four read-only and set). Its enable bit reads 1 whatever is written: GPUB always translates (TRM 18.11.1.136).
    // Swgroup GPU (0xaa8, a[34] = 0) never does.
    TEGRA_SMMU_GPUB_ASID = 0xaac,
    TEGRA_SMMU_SDMMC1A_ASID = 0xa94,  // swgroup "sdmmc1a" (sdmmcra/sdmmcwa: TRANSLATION_ENABLE_3 bits 0/4)
    TEGRA_SMMU_HC_ASID = 0x250,       // swgroup "hc" (host1xdmar/host1xr: _0 bits 22/23; host1xw: _1 bit 22)
    // host1xdmar's in-band SWID "hc1" (TRM 18.5.3 client table, 18.11.1.142): one ASID field, disabled at reset.
    TEGRA_SMMU_HC1_ASID = 0xac4,
    TEGRA_SMMU_APE_ASID = 0xab8,  // swgroup "ape" (aper/apew: TRANSLATION_ENABLE_3 bits 26/27)
    // The Security Engines' swgroups (L4T tegra210b01-soc-base.dtsi iommus; tegra_smmu_se_swgroups): SE1 in SE and SE1
    // (TRM 18.11.1.140, .143), SE2 in ISP2B (18.11.1.134) and ISP2B1. ISP2B1 is T210B01's: L4T's tegra-swgroup.h
    // "TEGRA_SWGROUP_ISP2B1 53 /* 0x808, SE2 on t210b01 */". The T210 TRM has no MC register at 0x808.
    TEGRA_SMMU_SE_ASID = 0xabc,
    TEGRA_SMMU_SE1_ASID = 0xac8,
    TEGRA_SMMU_ISP2B_ASID = 0xaa4,
    TEGRA_SMMU_ISP2B1_ASID = 0x808,
    TEGRA_SMMU_NVJPG_ASID = 0xac0,  // swgroup "nvjpg" (clients nvjpgsrd/nvjpgswr: TRANSLATION_ENABLE_3 bits 30/31)
    TEGRA_SMMU_NVDEC_ASID = 0xab4,  // swgroup "nvdec" (clients nvdecsrd/nvdecswr: TRANSLATION_ENABLE_3 bits 24/25)
    TEGRA_SMMU_NVENC_ASID = 0x264,  // swgroup "nvenc" (nvencsrd: TRANSLATION_ENABLE_0 bit 28, nvencswr: _1 bit 11)
};

// Two drivers on one ASID or one swgroup register would translate through each other's tables. Every enumerator above
// is a case here (-Wswitch names one left out), so a value taken twice fails to compile: duplicate case values.
static inline void tegra_smmu_ids_distinct(enum tegra_smmu_asid asid, enum tegra_smmu_register reg)
{
    switch (asid) {
    case TEGRA_SMMU_ASID_VIC:
    case TEGRA_SMMU_ASID_DISPLAY:
    case TEGRA_SMMU_ASID_NVJPG:
    case TEGRA_SMMU_ASID_XUSB_HOST:
    case TEGRA_SMMU_ASID_NVDEC:
    case TEGRA_SMMU_ASID_NVENC:
    case TEGRA_SMMU_ASID_XUSB_DEV:
    case TEGRA_SMMU_ASID_DISPLAY_B:
    case TEGRA_SMMU_ASID_HDA:
    case TEGRA_SMMU_ASID_AFI:
    case TEGRA_SMMU_ASID_GPU:
    case TEGRA_SMMU_ASID_FAULT:
    case TEGRA_SMMU_ASID_GPU_LOW:
    case TEGRA_SMMU_ASID_GPU_HIGH:
    case TEGRA_SMMU_ASID_SDMMC1_LOW:
    case TEGRA_SMMU_ASID_SDMMC1_HIGH:
    case TEGRA_SMMU_ASID_HOST1X:
    case TEGRA_SMMU_ASID_APE:
    case TEGRA_SMMU_ASID_SE1:
    case TEGRA_SMMU_ASID_SE2:
        break;
    }
    switch (reg) {
    case TEGRA_SMMU_TRANSLATION_ENABLE_0:
    case TEGRA_SMMU_TRANSLATION_ENABLE_1:
    case TEGRA_SMMU_TRANSLATION_ENABLE_2:
    case TEGRA_SMMU_TRANSLATION_ENABLE_3:
    case TEGRA_SMMU_TRANSLATION_ENABLE_4:
    case TEGRA_SMMU_AFI_ASID:
    case TEGRA_SMMU_DC_ASID:
    case TEGRA_SMMU_DCB_ASID:
    case TEGRA_SMMU_HDA_ASID:
    case TEGRA_SMMU_VIC_ASID:
    case TEGRA_SMMU_XUSB_HOST_ASID:
    case TEGRA_SMMU_XUSB_DEV_ASID:
    case TEGRA_SMMU_GPUB_ASID:
    case TEGRA_SMMU_SDMMC1A_ASID:
    case TEGRA_SMMU_HC_ASID:
    case TEGRA_SMMU_HC1_ASID:
    case TEGRA_SMMU_APE_ASID:
    case TEGRA_SMMU_SE_ASID:
    case TEGRA_SMMU_SE1_ASID:
    case TEGRA_SMMU_ISP2B_ASID:
    case TEGRA_SMMU_ISP2B1_ASID:
    case TEGRA_SMMU_NVJPG_ASID:
    case TEGRA_SMMU_NVDEC_ASID:
    case TEGRA_SMMU_NVENC_ASID:
        break;
    }
}

// Each Security Engine's swgroup registers, SE1 first: whether the SMMU translates an engine's memory clients is bit 31
// of both of its registers.
static const uint32_t tegra_smmu_se_swgroups[2][2] = {{TEGRA_SMMU_SE_ASID, TEGRA_SMMU_SE1_ASID},
                                                      {TEGRA_SMMU_ISP2B_ASID, TEGRA_SMMU_ISP2B1_ASID}};

// Points swgroup register asidReg at the directory ptb (TegraSMMUDomain::ptb) under asid: the directory (read back),
// a flush, the clients' translation bits in translationReg checked (the secure world sets them), the swgroup, a flush.
// 0, or -1 with one line in `line`: the swgroup is translated by another ASID, a translation bit is off, or a register
// did not take.
int tegra_smmu_attach(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg, uint32_t translationBits,
                      char *line, size_t size);
// The same for a 34-bit client's swgroup (XUSB_HOST, XUSB_DEV): asid in all four of its ASID fields, one per va[33:32].
int tegra_smmu_attach_wide(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg,
                           uint32_t translationBits, char *line, size_t size);
// A swgroup register's four fields (va[33:32] = 0..3), each through its own ASID and directory: for a client whose
// addresses reach RAM above 4 GiB (TegraSMMUIdentity.h). alwaysTranslated: the swgroup's enable bit is hardwired to 1
// (GPUB), so its reset value names ASID 0 without anyone owning it, and its detach parks every field on
// TEGRA_SMMU_ASID_FAULT. Otherwise as tegra_smmu_attach.
int tegra_smmu_attach_split(const uint32_t asids[4], const uint32_t ptbs[4], uint32_t asidReg, uint32_t translationReg,
                            uint32_t translationBits, bool alwaysTranslated, char *line, size_t size);
// PTB_DATA of an empty directory, made by the first caller (thread context): every access through
// TEGRA_SMMU_ASID_FAULT faults (INVALID_SMMU_PAGE). 0 when no memory.
uint32_t tegra_smmu_fault_ptb(void);
// Restore a retained attachment after reset, before that client resumes DMA. A different or detached identity is refused.
int switch_smmu_restore(uint32_t asid, uint32_t ptb, uint32_t asidReg, char *line, size_t size);
// The swgroup back to bypass (an always-translated one onto the fault ASID), its ASIDs' directories cleared, a flush.
void tegra_smmu_detach(uint32_t asid, uint32_t asidReg);
// After a change to the ASID's directory (cleaned to memory first): every ASID's PTC entries, then the ASID's TLB.
void tegra_smmu_flush(uint32_t asid);
// After a change to page entries only (cleaned to memory first): the PTC entries of the table bytes [tablesPa,
// tablesPa + tableBytes), then the ASID's TLB entries in the 4 MiB sections of [iova, iova + iovaBytes). Other ASIDs,
// the display's isochronous fetches among them, keep their cached translations. TegraSMMUPublish.h calls it.
void switch_smmu_flush_range(uint32_t asid, uint64_t tablesPa, uint64_t tableBytes, uint64_t iova, uint64_t iovaBytes);

// One page table's part of switch_smmu_flush_tables: the table's 64-byte groups of 16 entries in `groups` (bit g:
// entries 16g to 16g + 15) and the 4 MiB section at `iova`, the one the table maps.
struct switch_smmu_table_flush {
    uint64_t pa;
    uint64_t groups;
    uint32_t iova;
};

// As switch_smmu_flush_range for each table's groups and section, under one hold of the MC lock and with one read-back
// at the end.
void switch_smmu_flush_tables(uint32_t asid, const struct switch_smmu_table_flush *tables, uint32_t count);
// The MC register at offset (for a driver's own checks and FAIL lines).
uint32_t tegra_smmu_read(uint32_t offset);
// Sets or clears a memory client's hot-reset bit in MC_CLIENT_HOTRESET_CTRL (0x200) or CTRL_1 (0x970); the flush has
// drained once its bit reads set in the matching STATUS register (tegra_smmu_read). The only writer of both registers:
// each holds the bits of clients that different drivers power (CTRL: VIC, NVENC, HC, DCB; CTRL_1: GPU, NVDEC, NVJPG),
// and a read-modify-write anywhere else can put back a bit another driver just changed. STATUS reads done for a client
// with no flush requested (TRM 18.11.1.114), so the bit is read back: one error line names the client when it did not
// take, and a flush trusts STATUS only while its request bit still reads set.
void tegra_mc_hot_reset(uint32_t ctrl, uint32_t bit, bool on);
// After SC7, before any translated client runs (SwitchSuspend's wake): every attached swgroup programmed again, and the
// error interrupt's mask (SC7 resets the MC). Returns how many swgroups could not be (one line each); their clients
// then fault at the MC instead of translating.
int tegra_smmu_resume(void);

// The memory controller's error interrupt (TegraMCErrors.cpp), the one owner of MC_INTSTATUS, INTMASK, ERR_STATUS and
// ERR_ADR: each decode, security or SMMU error a client raises is one line naming its kind, client, direction and
// address, and its INTSTATUS bit is cleared so the next is captured. Nobody else clears INTSTATUS: a driver that wants
// the MC's errors in its own line reads tegra_mc_error_latest. A client's run of errors prints its first few in full and
// then one count line when it ends (TegraMCErrors.h Runs). Thread context, at boot; returns 0 at once, its own thread
// attaching once the GIC is up. An interrupt that cannot be attached or unmasked panics.
int tegra_mc_errors_start(void);
// How many errors the interrupt took since boot, and the latest one (zeros before the first). Any context.
uint32_t tegra_mc_error_latest(uint32_t *intstatus, uint32_t *status, uint32_t *address);
// After SC7 (the MC was reset): the runs end, INTMASK again. Thread context.
void switch_mc_errors_resume(void);
// Swgroup register `asidReg` went back to bypass (tegra_smmu_detach): its clients' runs end. Thread context, no lock held.
void switch_mc_errors_detached(uint32_t asidReg);

#if DEVELOPMENT || DEBUG
// The SMMU's TLB and PTC hits and misses since the previous call, every client's together (the counters are global, T210
// TRM 18.5.4.3), as "tlb H hits M misses (P% hit), ptc ...". The counters are on from the first call (TLB_CONFIG and
// PTC_CONFIG bit 31, 18.11.1.6-7), and again after SC7 reset them; the line then names both registers and counts from
// there. Their offsets (0x1f0 + 8 x {TLB, PTC} + 4 x {hit, miss}) are L4T's tegra-smmu.h. Thread context.
void switch_smmu_stats_window(char *line, size_t size);

// A deliberate SMMU fault (TegraVIC's MC fault probe). From switch_mc_errors_expect until the end, the interrupt's
// filter takes each INVALID_SMMU_PAGE that `client` raises at an address in [low, high) below 4 GiB without panicking,
// and counts it; every other error goes the usual way. The fault must be at an address the client's page directory
// leaves empty: the SMMU keeps no page-table group from it.
struct switch_mc_expected {
    uint32_t attached;                    // 0: the interrupt was never attached (nothing was taken)
    uint32_t taken;                       // the expected errors the interrupt delivered
    uint32_t intstatus, status, address;  // the first of them
};

void switch_mc_errors_expect(uint32_t client, uint32_t low, uint32_t high);
// Once the faulting DMA is over: waits up to `ms` for the interrupt to have delivered one and for INTSTATUS to hold no
// SMMU fault, then ends the expectation. An SMMU fault still latched then panics: the interrupt was lost. Thread
// context.
void switch_mc_errors_expect_end(uint32_t ms, struct switch_mc_expected *out);
#endif

#ifdef __cplusplus
}
#endif
