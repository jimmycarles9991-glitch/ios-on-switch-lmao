// SPDX-License-Identifier: BSD-2-Clause-Patent
// The swgroups TegraSMMU has pointed at a directory, as plain C for the host test. SC7 resets the MC's non-secure SMMU
// registers (the directory of each ASID, each swgroup's ASID register); BL31 restores only its own MC setup, so
// tegra_smmu_resume programs every entry here again.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One per swgroup and translation register its clients are in (NVENC, AFI, GPUB and HC take two each).
#define TEGRA_SMMU_ATTACHMENTS 32u

struct tegra_smmu_attachment {
    uint32_t asid, ptb, asidReg, translationReg, translationBits;
    bool wide;  // the swgroup register has an ASID field for each value of va[33:32], all set to asid
    // GPUB's enable bit reads 1 whatever is written (T210 TRM 18.11.1.136): the swgroup can never bypass, so a detach
    // points every field at TEGRA_SMMU_ASID_FAULT instead of clearing the register.
    bool alwaysTranslated;
    // Nonzero: a split attachment. Field n (va[33:32] = n, n = 1..3) translates through upperAsid[n - 1] and its
    // directory upperPtb[n - 1]; field 0 through asid and ptb.
    uint32_t upperAsid[3], upperPtb[3];
};

struct tegra_smmu_attachments {
    struct tegra_smmu_attachment entry[TEGRA_SMMU_ATTACHMENTS];
    uint32_t count;
};

static inline struct tegra_smmu_attachment tegra_smmu_attachment_of(uint32_t asid, uint32_t ptb, uint32_t asidReg,
                                                                    uint32_t translationReg, uint32_t translationBits,
                                                                    bool wide)
{
    struct tegra_smmu_attachment a = {asid, ptb,   asidReg,   translationReg, translationBits,
                                      wide, false, {0, 0, 0}, {0, 0, 0}};
    return a;
}

static inline bool tegra_smmu_attachment_split(const struct tegra_smmu_attachment *a)
{
    return a->upperAsid[0] != 0;
}

// The ASID and directory of field n (0..3) of the swgroup register; a narrow attachment has only field 0.
static inline uint32_t tegra_smmu_attachment_fields(const struct tegra_smmu_attachment *a)
{
    return a->wide || tegra_smmu_attachment_split(a) ? 4u : 1u;
}

static inline uint32_t tegra_smmu_attachment_asid(const struct tegra_smmu_attachment *a, uint32_t field)
{
    return field && tegra_smmu_attachment_split(a) ? a->upperAsid[field - 1] : a->asid;
}

static inline uint32_t tegra_smmu_attachment_ptb(const struct tegra_smmu_attachment *a, uint32_t field)
{
    return field && tegra_smmu_attachment_split(a) ? a->upperPtb[field - 1] : a->ptb;
}

// Whether a and b select the same directories through the swgroup register (their translation registers aside).
static inline bool tegra_smmu_attachment_same_directories(const struct tegra_smmu_attachment *a,
                                                          const struct tegra_smmu_attachment *b)
{
    if (a->wide != b->wide || a->alwaysTranslated != b->alwaysTranslated ||
        tegra_smmu_attachment_fields(a) != tegra_smmu_attachment_fields(b))
        return false;
    for (uint32_t field = 0; field < tegra_smmu_attachment_fields(a); ++field) {
        if (tegra_smmu_attachment_asid(a, field) != tegra_smmu_attachment_asid(b, field) ||
            tegra_smmu_attachment_ptb(a, field) != tegra_smmu_attachment_ptb(b, field))
            return false;
    }
    return true;
}

// A swgroup can have clients in multiple translation registers; every one survives SC7.
static inline bool tegra_smmu_attachments_note(struct tegra_smmu_attachments *t, const struct tegra_smmu_attachment *a)
{
    for (uint32_t i = 0; i < t->count; ++i) {
        if (t->entry[i].asidReg == a->asidReg) {
            if (!tegra_smmu_attachment_same_directories(&t->entry[i], a)) {
                for (uint32_t j = 0; j < t->count; ++j) {
                    if (j != i && t->entry[j].asidReg == a->asidReg)
                        return false;
                }
                t->entry[i] = *a;
                return true;
            }
            if (t->entry[i].translationReg == a->translationReg) {
                t->entry[i].translationBits |= a->translationBits;
                return true;
            }
        }
    }
    if (t->count >= TEGRA_SMMU_ATTACHMENTS)
        return false;
    t->entry[t->count++] = *a;
    return true;
}

// The index of the first retained entry for swgroup register asidReg, or UINT32_MAX.
static inline uint32_t tegra_smmu_attachments_find(const struct tegra_smmu_attachments *t, uint32_t asidReg)
{
    for (uint32_t i = 0; i < t->count; ++i) {
        if (t->entry[i].asidReg == asidReg)
            return i;
    }
    return UINT32_MAX;
}

// A detach of the swgroup register: its entry goes, the others keep their order.
static inline void tegra_smmu_attachments_forget(struct tegra_smmu_attachments *t, uint32_t asidReg)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < t->count; ++i) {
        if (t->entry[i].asidReg != asidReg)
            t->entry[kept++] = t->entry[i];
    }
    t->count = kept;
}

#ifdef __cplusplus
}
#endif
