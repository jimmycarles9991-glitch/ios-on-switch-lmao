// SPDX-License-Identifier: GPL-2.0-only
/*
 * dyld shared cache function-variant slots that the shared region pager fills (osfmk/vm/vm_shared_region_pager.c).
 *
 * iOS 27's caches keep some function-variant pointers in __TPRO_CONST and ship them as 0: dyld selects and stores
 * each one in every process (Loader::adjustFunctionVariantsInDyldCache). Apple's kernel tells a TPRO-capable process
 * dyld_hw_tpro_pagers=1, and the store goes straight to the TPRO page. The A57 has no TPRO, so dyld's lazy cache
 * writer instead remaps every __DATA_CONST and __AUTH_CONST region of the cache VM_PROT_COPY to store those words,
 * which takes about 356 MB of read-only cache out of the shared nested pmap in each process. The pager delivers the
 * page with the value dyld selects on this system already stored, dyld's compare matches, and it writes nothing.
 *
 * dyld compares each slot with its own choice and stores that whenever they differ, so a pinned value it would not
 * choose is overwritten and the process remaps the cache as it would without the fill.
 */
#ifndef SWITCH_FVARIANT_H
#define SWITCH_FVARIANT_H

#include <stddef.h>
#include <stdint.h>

#include "fvariant_types.h"

/* The kind the kernel is built for (the owner header's SWITCH_IPSW_KIND_<build>); host tests take a kind's table. */
#if defined(SWITCH_IPSW_KIND_24B119)
#include "fvariant_24B119.h"
#define SWITCH_FVARIANT_CACHES switch_fvariant_24B119
#elif defined(SWITCH_IPSW_BUILD)
#error "no dyld function-variant slots for this IPSW kind: add switch/fvariant_<build>.h"
#endif

/* dyld_cache_header (dyld's dyld_cache_format.h): the fields read, and the prefix that holds them. */
#define SWITCH_FVARIANT_HEADER_MAPPING_OFFSET 0x10
#define SWITCH_FVARIANT_HEADER_UUID           0x58
#define SWITCH_FVARIANT_HEADER_INFO_SIZE      0x210
#define SWITCH_FVARIANT_HEADER_SIZE           0x218

enum switch_fvariant_header {
    SWITCH_FVARIANT_NO_SLOTS, /* a cache without function-variant patch locations */
    SWITCH_FVARIANT_PINNED,   /* one of `caches` */
    SWITCH_FVARIANT_UNPINNED, /* a cache with patch locations none of `caches` pins */
};

/*
 * What a main cache file's header (its first SWITCH_FVARIANT_HEADER_SIZE bytes) says about its function-variant
 * slots, and which of `caches` it is. A header whose mappings start before functionVariantInfoSize predates the field,
 * as dyld reads it; a table of one 8-byte header and no entries lists no slots.
 */
static inline enum switch_fvariant_header switch_fvariant_classify(const uint8_t *header,
                                                                   const struct switch_fvariant_cache *caches,
                                                                   size_t count,
                                                                   const struct switch_fvariant_cache **cache)
{
    uint32_t mapping_offset = 0;
    uint64_t info_size = 0;
    __builtin_memcpy(&mapping_offset, header + SWITCH_FVARIANT_HEADER_MAPPING_OFFSET, sizeof(mapping_offset));
    __builtin_memcpy(&info_size, header + SWITCH_FVARIANT_HEADER_INFO_SIZE, sizeof(info_size));
    *cache = NULL;
    if (mapping_offset < SWITCH_FVARIANT_HEADER_SIZE || info_size <= 8) {
        return SWITCH_FVARIANT_NO_SLOTS;
    }
    for (size_t i = 0; i < count; ++i) {
        if (__builtin_memcmp(caches[i].uuid, header + SWITCH_FVARIANT_HEADER_UUID, sizeof(caches[i].uuid)) == 0) {
            *cache = &caches[i];
            return SWITCH_FVARIANT_PINNED;
        }
    }
    return SWITCH_FVARIANT_UNPINNED;
}

/*
 * Stores the slots of `cache` that lie in the part of a page being paged in: `page` is its kernel address, `uaddr`
 * its address in the shared region, `slide` the region's slide. A slot is stored only while it holds the 0 the file
 * ships, and as a plain pointer: this kernel has no pointer authentication, and dyld's signing of it is the identity
 * on the A57 too. Returns how many it stored.
 */
static inline uint32_t switch_fvariant_fill(const struct switch_fvariant_cache *cache, uint32_t slide, uint8_t *page,
                                            uint64_t uaddr, uint64_t size)
{
    uint32_t filled = 0;
    if (cache == NULL || size < (uint64_t)sizeof(uint64_t)) {
        return 0;
    }
    for (uint32_t i = 0; i < cache->count; ++i) {
        const uint64_t at = cache->slots[i].slot + slide;
        if (at < uaddr || at - uaddr > size - (uint64_t)sizeof(uint64_t)) {
            continue;
        }
        uint64_t value = 0;
        __builtin_memcpy(&value, page + (at - uaddr), sizeof(value));
        if (value != 0) {
            continue;
        }
        value = cache->slots[i].target + slide;
        __builtin_memcpy(page + (at - uaddr), &value, sizeof(value));
        ++filled;
    }
    return filled;
}

#endif
