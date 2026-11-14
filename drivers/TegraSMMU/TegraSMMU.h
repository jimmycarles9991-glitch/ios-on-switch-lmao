// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra210 memory controller's SMMU registers, shared by every driver that translates a swgroup (TegraVIC, TegraNVJPG,
// TegraNVDEC, the display). Selecting a directory takes two global registers in turn (PTB_ASID, then PTB_DATA) and
// the PTC/TLB flushes are global too, so two drivers programming at once would program each other's ASID: every access
// goes through here, under one lock. Page tables are TegraSMMUDomain.h.
//
// Registers (Linux memory/tegra/tegra210.c, iommu/tegra-smmu.c): PTB_ASID 0x1c, PTB_DATA 0x20, TLB_FLUSH 0x30 (ASID match
// bit 31, ASID in 30:24 here written as asid << 24), PTC_FLUSH 0x34 (0: all). A swgroup's ASID register is ENABLE 31 |
// ASID; each memory client has a translation bit in TRANSLATION_ENABLE_0..4 (0x228..0x238), which switchroot's bl31 sets
// for every client while leaving every swgroup in bypass. The global SMMU enable is secure-only (reads back garbage), so a
// whether translation is on is known from what the unit's engines did (TegraVIC.cpp), not read back.
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
    TEGRA_SMMU_ASID_NVENC = 122
};

enum tegra_smmu_register {
    TEGRA_SMMU_TRANSLATION_ENABLE_0 = 0x228,
    TEGRA_SMMU_TRANSLATION_ENABLE_1 = 0x22c,
    TEGRA_SMMU_TRANSLATION_ENABLE_2 = 0x230,
    TEGRA_SMMU_TRANSLATION_ENABLE_3 = 0x234,
    TEGRA_SMMU_DC_ASID = 0x240,
    TEGRA_SMMU_VIC_ASID = 0x284,
    TEGRA_SMMU_XUSB_HOST_ASID = 0x288,  // swgroup "xusb_host" (xusb_hostr/xusb_hostw: TRANSLATION_ENABLE_2 bits 10/11)
    TEGRA_SMMU_NVJPG_ASID = 0xac0,      // swgroup "nvjpg" (clients nvjpgsrd/nvjpgswr: TRANSLATION_ENABLE_3 bits 30/31)
    TEGRA_SMMU_NVDEC_ASID = 0xab4,      // swgroup "nvdec" (clients nvdecsrd/nvdecswr: TRANSLATION_ENABLE_3 bits 24/25)
    TEGRA_SMMU_NVENC_ASID = 0x264,      // swgroup "nvenc" (nvencsrd: TRANSLATION_ENABLE_0 bit 28, nvencswr: _1 bit 11)
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
        break;
    }
    switch (reg) {
    case TEGRA_SMMU_TRANSLATION_ENABLE_0:
    case TEGRA_SMMU_TRANSLATION_ENABLE_1:
    case TEGRA_SMMU_TRANSLATION_ENABLE_2:
    case TEGRA_SMMU_TRANSLATION_ENABLE_3:
    case TEGRA_SMMU_DC_ASID:
    case TEGRA_SMMU_VIC_ASID:
    case TEGRA_SMMU_XUSB_HOST_ASID:
    case TEGRA_SMMU_NVJPG_ASID:
    case TEGRA_SMMU_NVDEC_ASID:
    case TEGRA_SMMU_NVENC_ASID:
        break;
    }
}

// Points swgroup register asidReg at the directory ptb (TegraSMMUDomain::ptb) under asid: the directory (read back),
// a flush, the clients' translation bits in translationReg, the swgroup, a flush. 0, or -1 with one line in `line`: the
// swgroup is translated by another ASID, or a register did not take.
int tegra_smmu_attach(uint32_t asid, uint32_t ptb, uint32_t asidReg, uint32_t translationReg, uint32_t translationBits,
                      char *line, size_t size);
// Restore a retained attachment after reset, before that client resumes DMA. A different or detached identity is refused.
int switch_smmu_restore(uint32_t asid, uint32_t ptb, uint32_t asidReg, char *line, size_t size);
// The swgroup back to bypass, the ASID's directory cleared, a flush.
void tegra_smmu_detach(uint32_t asid, uint32_t asidReg);
// After a change to the ASID's tables (cleaned to memory first): PTC all, then the ASID's TLB entries.
void tegra_smmu_flush(uint32_t asid);
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
// the MC's errors in its own line reads tegra_mc_error_latest. Thread context, at boot; -1 with one line when the
// interrupt cannot be had.
int tegra_mc_errors_start(void);
// How many errors the interrupt took since boot, and the latest one (zeros before the first). Any context.
uint32_t tegra_mc_error_latest(uint32_t *intstatus, uint32_t *status, uint32_t *address);
void switch_mc_errors_resume(void);

#ifdef __cplusplus
}
#endif
