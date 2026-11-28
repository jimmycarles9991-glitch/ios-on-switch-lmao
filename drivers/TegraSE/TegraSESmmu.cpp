// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraSESmmu.h.
#include <IOKit/IOLib.h>
#include "TegraSESmmu.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMURamView.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

namespace {
constexpr unsigned kEngines = 2, kOwned = 4;
constexpr uint32_t kAsid[kEngines] = {TEGRA_SMMU_ASID_SE1, TEGRA_SMMU_ASID_SE2};
// SE1's clients in the SE swgroup, sesrd and seswr (TRANSLATION_ENABLE_4 bits 0/1, T210 TRM 18.11.1.153). No document
// on hand names the clients of SE1's second swgroup or of SE2's (T210B01's): their bits are not checked.
constexpr uint32_t kClients[kEngines][2] = {{1u << 0 | 1u << 1, 0}, {0, 0}};
constexpr uint32_t kTranslationReg[kEngines][2] = {{TEGRA_SMMU_TRANSLATION_ENABLE_4, TEGRA_SMMU_TRANSLATION_ENABLE_4},
                                                   {TEGRA_SMMU_TRANSLATION_ENABLE_4, TEGRA_SMMU_TRANSLATION_ENABLE_4}};
constexpr uint64_t k4GiB = 1ULL << 32;

struct Owned {
    uint64_t pa, bytes;
};

TegraSMMURamView gView[kEngines];
bool gAttached[kEngines];
Owned gOwned[kEngines][kOwned];

bool owned(unsigned engine, uint64_t pa)
{
    for (const Owned &o : gOwned[engine]) {
        if (pa - o.pa < o.bytes)
            return true;
    }
    return false;
}

uint32_t entries(const uint32_t *list)
{
    return list[0] + 1;
}

bool listAllOwned(unsigned engine, const uint32_t *list)
{
    if (!list)
        return true;
    const uint32_t n = entries(list);
    for (uint32_t i = 0; i < n; ++i) {
        if (!owned(engine, list[1 + 2 * i]))
            return false;
    }
    return true;
}
}  // namespace

tegra_se_smmu_attach_result tegra_se_smmu_attach(unsigned engine, const switch_dma_buffer *const *buffers,
                                                 unsigned count, char *line, size_t size)
{
    if (engine >= kEngines) {
        snprintf(line, size, "no engine %u", engine);
        return TEGRA_SE_SMMU_UNTRANSLATED;
    }
    const uint32_t *swgroups = tegra_smmu_se_swgroups[engine];
    bool translated = false;
    for (unsigned i = 0; i < 2; ++i)
        translated |= (tegra_smmu_read(swgroups[i]) & (1u << 31)) != 0u;
    if (count > kOwned || !gView[engine].init(kAsid[engine], kAsid[engine], false)) {
        snprintf(line, size, "no memory for its view of RAM");
        return translated ? TEGRA_SE_SMMU_TRANSLATED : TEGRA_SE_SMMU_UNTRANSLATED;
    }
    unsigned failedIndex = count;
    gView[engine].unlocked([&](auto &core) {
        for (unsigned i = 0; i < count; ++i) {
            if (buffers[i]->pa + buffers[i]->size > k4GiB || !core.hold(buffers[i]->pa, buffers[i]->size)) {
                failedIndex = i;
                for (unsigned j = 0; j < i; ++j)
                    (void)core.release(buffers[j]->pa, buffers[j]->size);
                return;
            }
            gOwned[engine][i] = {buffers[i]->pa, buffers[i]->size};
        }
        core.commit();
    });
    if (failedIndex < count) {
        for (unsigned j = 0; j < failedIndex; ++j)
            gOwned[engine][j] = {};
        snprintf(line, size, "its buffer at %#llx is outside RAM below 4 GiB",
                 static_cast<unsigned long long>(buffers[failedIndex]->pa));
        return translated ? TEGRA_SE_SMMU_TRANSLATED : TEGRA_SE_SMMU_UNTRANSLATED;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (gView[engine].attachNarrow(swgroups[i], kTranslationReg[engine][i], kClients[engine][i], line, size)) {
            if (i)
                tegra_smmu_detach(kAsid[engine], swgroups[0]);
            return translated ? TEGRA_SE_SMMU_TRANSLATED : TEGRA_SE_SMMU_UNTRANSLATED;
        }
    }
    gAttached[engine] = true;
    return TEGRA_SE_SMMU_ATTACHED;
}

bool tegra_se_smmu_add_owned(unsigned engine, const switch_dma_buffer *buffer)
{
    if (engine >= kEngines || !gAttached[engine])
        return true;
    if (!buffer || !buffer->size || buffer->pa + buffer->size > k4GiB)
        return false;
    unsigned slot = kOwned;
    for (unsigned i = 0; i < kOwned; ++i) {
        if (gOwned[engine][i].bytes == 0) {
            slot = i;
            break;
        }
    }
    if (slot == kOwned)
        return false;
    bool ok = false;
    gView[engine].unlocked([&](auto &core) {
        ok = core.hold(buffer->pa, buffer->size);
        if (ok)
            core.commit();
    });
    if (ok)
        gOwned[engine][slot] = {buffer->pa, buffer->size};
    return ok;
}

bool tegra_se_smmu_hold_lists(unsigned engine, const uint32_t *a, const uint32_t *b)
{
    if (engine >= kEngines || !gAttached[engine])
        return true;
    if (listAllOwned(engine, a) && listAllOwned(engine, b))
        return true;
    const uint32_t *lists[2] = {a, b};
    const uint32_t counts[2] = {a ? entries(a) : 0, b ? entries(b) : 0};
    uint32_t held[2] = {0, 0};
    bool ok = true;
    gView[engine].unlocked([&](auto &core) {
        bool changed = false;
        for (unsigned li = 0; li < 2 && ok; ++li) {
            for (; held[li] < counts[li]; ++held[li]) {
                const uint64_t pa = lists[li][1 + 2 * held[li]];
                const uint64_t bytes = lists[li][2 + 2 * held[li]];
                if (owned(engine, pa))
                    continue;
                if (pa + bytes > k4GiB || !core.hold(pa, bytes)) {
                    ok = false;
                    break;
                }
                changed = true;
            }
        }
        if (!ok && changed) {
            for (unsigned li = 0; li < 2; ++li) {
                for (uint32_t i = 0; i < held[li]; ++i) {
                    if (!owned(engine, lists[li][1 + 2 * i]))
                        (void)core.release(lists[li][1 + 2 * i], lists[li][2 + 2 * i]);
                }
            }
            changed = false;
        }
        if (changed)
            core.commit();
    });
    return ok;
}

void tegra_se_smmu_release_lists(unsigned engine, const uint32_t *a, const uint32_t *b)
{
    if (engine >= kEngines || !gAttached[engine])
        return;
    if (listAllOwned(engine, a) && listAllOwned(engine, b))
        return;
    const uint32_t *lists[2] = {b, a};
    bool released = true;
    gView[engine].unlocked([&](auto &core) {
        bool changed = false;
        for (const uint32_t *list : lists) {
            if (!list)
                continue;
            const uint32_t n = entries(list);
            for (uint32_t i = 0; i < n; ++i) {
                if (!owned(engine, list[1 + 2 * i])) {
                    released &= core.release(list[1 + 2 * i], list[2 + 2 * i]);
                    changed = true;
                }
            }
        }
        if (changed)
            core.commit();
    });
    if (!released)
        panic("TegraSE: SE%u released a list entry its SMMU view had no hold on", engine + 1);
}

bool tegra_se_smmu_hold(unsigned engine, const uint32_t *list)
{
    return tegra_se_smmu_hold_lists(engine, list, nullptr);
}

void tegra_se_smmu_release(unsigned engine, const uint32_t *list)
{
    tegra_se_smmu_release_lists(engine, list, nullptr);
}
