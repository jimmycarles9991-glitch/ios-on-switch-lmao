// SPDX-License-Identifier: GPL-2.0-only
/*
 * The CDHash lists the boot package puts in /chosen (tools/package/adt_edit.py with_cdhash_lists): each role's
 * CDHashes, 20 bytes each, ascending in memcmp order and each once. The kernel answers whether a file belongs to a role
 * with one binary search over the device tree's bytes (kernel/switch/vfs/auth_fs.c, pretranslated.c).
 */
#ifndef SWITCH_CDHASH_H
#define SWITCH_CDHASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_CDHASH_BYTES 20u

struct switch_cdhash_list {
    const uint8_t *hashes;
    uint32_t count;
};

/* A property's bytes as a list; empty unless they are whole CDHashes. */
static inline struct switch_cdhash_list switch_cdhash_list_of(const void *bytes, size_t size)
{
    struct switch_cdhash_list list = {NULL, 0};
    if (bytes != NULL && size != 0 && size % SWITCH_CDHASH_BYTES == 0 && size / SWITCH_CDHASH_BYTES <= UINT32_MAX) {
        list.hashes = (const uint8_t *)bytes;
        list.count = (uint32_t)(size / SWITCH_CDHASH_BYTES);
    }
    return list;
}

static inline bool switch_cdhash_listed(const struct switch_cdhash_list *list, const uint8_t *cdhash)
{
    uint32_t low = 0, high = list->count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        const int order =
            __builtin_memcmp(list->hashes + (size_t)middle * SWITCH_CDHASH_BYTES, cdhash, SWITCH_CDHASH_BYTES);
        if (order == 0) {
            return true;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return false;
}

#endif
