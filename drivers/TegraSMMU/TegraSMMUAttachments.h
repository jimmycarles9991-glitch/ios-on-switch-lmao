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

#define TEGRA_SMMU_ATTACHMENTS 16u

struct tegra_smmu_attachment {
    uint32_t asid, ptb, asidReg, translationReg, translationBits;
    bool wide;  // the swgroup register has an ASID field for each value of va[33:32], all set to asid
};

struct tegra_smmu_attachments {
    struct tegra_smmu_attachment entry[TEGRA_SMMU_ATTACHMENTS];
    uint32_t count;
};

// A swgroup can have clients in multiple translation registers; every one survives SC7.
static inline bool tegra_smmu_attachments_note(struct tegra_smmu_attachments *t, const struct tegra_smmu_attachment *a)
{
    for (uint32_t i = 0; i < t->count; ++i) {
        if (t->entry[i].asidReg == a->asidReg) {
            if (t->entry[i].asid != a->asid || t->entry[i].ptb != a->ptb || t->entry[i].wide != a->wide) {
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
