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

#define TEGRA_SMMU_ATTACHMENTS 8u

struct tegra_smmu_attachment {
    uint32_t asid, ptb, asidReg, translationReg, translationBits;
};

struct tegra_smmu_attachments {
    struct tegra_smmu_attachment entry[TEGRA_SMMU_ATTACHMENTS];
    uint32_t count;
};

// An attach that took: replaces the entry of the same swgroup register, or adds one. False when the table is full.
static inline bool tegra_smmu_attachments_note(struct tegra_smmu_attachments *t, const struct tegra_smmu_attachment *a)
{
    for (uint32_t i = 0; i < t->count; ++i) {
        if (t->entry[i].asidReg == a->asidReg) {
            t->entry[i] = *a;
            return true;
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
