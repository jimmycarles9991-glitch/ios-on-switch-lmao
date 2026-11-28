// SPDX-License-Identifier: GPL-2.0-only
/*
 * The shared region side of <switch/fvariant.h>: which pinned cache a shared region maps, and the fill of a slid page.
 */

#include <switch/fvariant.h>
#include <switch/fvariant_hooks.h>

#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <libkern/OSAtomic.h>
#include <os/log.h>
#include <vm/vm_shared_region_xnu.h>

const struct switch_fvariant_cache *switch_fvariant_for_region(struct vm_shared_region *sr)
{
    static uint32_t unread_logged, unpinned_logged;
    uint8_t header[SWITCH_FVARIANT_HEADER_SIZE];
    const struct switch_fvariant_cache *cache = NULL;

    /*
     * vm_shared_region_map_file() calls this once, before it slides the mappings, with the region's config map
     * switched in: the main cache file's header is mapped there, where vm_shared_region_map_file_final() reads it too.
     */
    if (sr->sr_first_mapping == (mach_vm_offset_t)-1 ||
        copyin((user_addr_t)(sr->sr_base_address + sr->sr_first_mapping), header, sizeof(header)) != 0) {
        if (OSCompareAndSwap(0, 1, &unread_logged)) {
            os_log_error(OS_LOG_DEFAULT,
                         "SwitchFvariant: a shared region's cache header could not be read: its "
                         "function-variant slots stay unfilled, and dyld will remap its read-only data");
        }
        return NULL;
    }
    const uint8_t *uuid = header + SWITCH_FVARIANT_HEADER_UUID;
    switch (switch_fvariant_classify(header, SWITCH_FVARIANT_CACHES,
                                     sizeof(SWITCH_FVARIANT_CACHES) / sizeof(SWITCH_FVARIANT_CACHES[0]), &cache)) {
    case SWITCH_FVARIANT_PINNED:
#if DEVELOPMENT || DEBUG
        os_log(OS_LOG_DEFAULT, "SwitchFvariant: cache %02x%02x%02x%02x: the pager fills %u function-variant slots",
               uuid[0], uuid[1], uuid[2], uuid[3], cache->count);
#endif
        break;
    case SWITCH_FVARIANT_UNPINNED:
        if (OSCompareAndSwap(0, 1, &unpinned_logged)) {
            os_log_error(OS_LOG_DEFAULT,
                         "SwitchFvariant: cache %02x%02x%02x%02x has function-variant slots and " SWITCH_IPSW_BUILD
                         " pins none for it: dyld will remap its read-only data in every process",
                         uuid[0], uuid[1], uuid[2], uuid[3]);
        }
        break;
    case SWITCH_FVARIANT_NO_SLOTS:
        break;
    }
    return cache;
}

void switch_fvariant_fill_page(const struct vm_shared_region_slide_info *si, uint8_t *page, uint64_t uaddr,
                               uint64_t size)
{
#if __has_feature(ptrauth_calls)
    /* The slots are authenticated pointers, which a kernel with pointer authentication would have to sign. */
    (void)si;
    (void)page;
    (void)uaddr;
    (void)size;
#else
    const uint32_t filled = switch_fvariant_fill(si->si_switch_fvariant, si->si_slide, page, uaddr, size);
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_DIAG_FVARIANT) && SWITCH_DIAG_FVARIANT
    if (filled != 0) {
        os_log(OS_LOG_DEFAULT, "SwitchFvariant: filled %u function-variant slots in 0x%llx", filled, uaddr);
    }
#else
    (void)filled;
#endif
#endif
}
