/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * The tree's translations of files that are installed somewhere the redirect never sees (the staged system apps
 * installd copies into app containers), found by the original's CDHash: Data:/private/var/Switch/pretranslated/
 * .originals, which tools/install/overlay_index.py writes.
 *
 * Little-endian: "SWOR", u32 version (1), u32 pair count, u32 0; then per pair, by original CDHash ascending and unique,
 * 20 bytes the original's CDHash and 20 bytes its translation's, the CDHash a record of the index carries.
 */
#pragma once

#include "switch_pretranslated.h"

#define SWITCH_PT_ORIGINALS_NAME    ".originals"
#define SWITCH_PT_ORIGINALS_MAGIC   0x524f5753u
#define SWITCH_PT_ORIGINALS_VERSION 1u
#define SWITCH_PT_ORIGINALS_HEADER  16u
#define SWITCH_PT_ORIGINALS_PAIR    (2u * SWITCH_PT_CDHASH)
#define SWITCH_PT_MAX_ORIGINALS     (1u << 16)
#define SWITCH_PT_MAX_ORIGINALS_BYTES \
    (SWITCH_PT_ORIGINALS_HEADER + (size_t)SWITCH_PT_ORIGINALS_PAIR * SWITCH_PT_MAX_ORIGINALS)

struct switch_pt_originals {
    const uint8_t *pairs;
    uint32_t count;
};

static inline const uint8_t *switch_pt_original_at(const struct switch_pt_originals *originals, uint32_t i)
{
    return originals->pairs + (size_t)SWITCH_PT_ORIGINALS_PAIR * i;
}

static inline enum switch_pt_error switch_pt_originals_parse(const uint8_t *raw, size_t size,
                                                             struct switch_pt_originals *out)
{
    if (size < SWITCH_PT_ORIGINALS_HEADER) {
        return SWITCH_PT_SHORT;
    }
    const uint32_t count = switch_pt_u32(raw + 8);
    if (switch_pt_u32(raw) != SWITCH_PT_ORIGINALS_MAGIC || switch_pt_u32(raw + 4) != SWITCH_PT_ORIGINALS_VERSION ||
        switch_pt_u32(raw + 12) != 0 || count > SWITCH_PT_MAX_ORIGINALS) {
        return SWITCH_PT_HEADER_BAD;
    }
    if (size != SWITCH_PT_ORIGINALS_HEADER + (size_t)SWITCH_PT_ORIGINALS_PAIR * count) {
        return SWITCH_PT_SIZE_BAD;
    }
    out->pairs = raw + SWITCH_PT_ORIGINALS_HEADER;
    out->count = count;
    for (uint32_t i = 1; i < count; ++i) {
        if (__builtin_memcmp(switch_pt_original_at(out, i - 1), switch_pt_original_at(out, i), SWITCH_PT_CDHASH) >= 0) {
            return SWITCH_PT_ORDER_BAD;
        }
    }
    return SWITCH_PT_OK;
}

/* files[i]: the index record carrying pair i's translation, so a lookup costs no scan of the index. Every translation
 * must have one. */
static inline enum switch_pt_error switch_pt_originals_link(const struct switch_pt_index *index,
                                                            const struct switch_pt_originals *originals,
                                                            uint32_t *files)
{
    for (uint32_t i = 0; i < originals->count; ++i) {
        const uint8_t *translated = switch_pt_original_at(originals, i) + SWITCH_PT_CDHASH;
        uint32_t file = 0;
        while (file < index->count &&
               __builtin_memcmp(switch_pt_file_at(index, file).cdhash, translated, SWITCH_PT_CDHASH) != 0) {
            ++file;
        }
        if (file == index->count) {
            return SWITCH_PT_RECORD_BAD;
        }
        files[i] = file;
    }
    return SWITCH_PT_OK;
}

/* The index record of the translation of the file signed as `original`, or -1. */
static inline int64_t switch_pt_original_file(const struct switch_pt_originals *originals, const uint32_t *files,
                                              const uint8_t *original)
{
    uint32_t lo = 0, hi = originals->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const int order = __builtin_memcmp(switch_pt_original_at(originals, mid), original, SWITCH_PT_CDHASH);
        if (order == 0) {
            return files[mid];
        }
        if (order < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return -1;
}
