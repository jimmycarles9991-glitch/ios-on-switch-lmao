// SPDX-License-Identifier: GPL-2.0-only
/*
 * host_statistics64(HOST_VM_INFO64) for iOS 27's user space, whose vm_statistics64 (xnu-13432, the iOS 27 SDK) grew
 * revisions 4 to 6 after this XNU's rev3: osfmk/kern/host.c's vm_stats takes counts up to SWITCH_HOST_VM_INFO64_COUNT
 * and fills the newer fields with switch_vm_stats_newer (kernel/switch/vm/stats.c). The count still negotiates the
 * revision, as each earlier one does.
 */
#ifndef SWITCH_VM_STATS_H
#define SWITCH_VM_STATS_H

#include <mach/host_info.h>
#include <stddef.h>
#include <stdint.h>

/* The fields after rev3's compressed_tag_storage_bytes, in the SDK's order. */
struct switch_vm_statistics64_newer {
    uint64_t speculative_pages_created; /* rev4 */
    uint64_t speculative_pages_activated;
    uint64_t swap_count; /* rev5 */
    uint64_t empty_tag_storing_tag_storage_pages;
    uint64_t executable_count; /* rev6 */
    uint64_t shared_region_count;
    uint64_t boot_stolen_count;
    uint64_t secluded_count;
    uint64_t active_internal_count;
    uint64_t inactive_internal_count;
    uint64_t active_external_count;
    uint64_t inactive_external_count;
    uint64_t purgeable_pageable_count;
    uint64_t purgeable_wired_count;
    uint64_t background_internal_count;
    uint64_t background_external_count;
    uint64_t donated_count;
    uint64_t realtime_count;
    uint64_t max_mem_count;
    uint64_t phantom_ghosts_found;
    uint64_t phantom_ghosts_added;
};

#define SWITCH_HOST_VM_INFO64_AT(field)                   \
    ((mach_msg_type_number_t)(HOST_VM_INFO64_REV3_COUNT + \
                              offsetof(struct switch_vm_statistics64_newer, field) / sizeof(integer_t)))
#define SWITCH_HOST_VM_INFO64_REV4_COUNT SWITCH_HOST_VM_INFO64_AT(swap_count)
#define SWITCH_HOST_VM_INFO64_REV5_COUNT SWITCH_HOST_VM_INFO64_AT(executable_count)
#define SWITCH_HOST_VM_INFO64_COUNT                       \
    ((mach_msg_type_number_t)(HOST_VM_INFO64_REV3_COUNT + \
                              sizeof(struct switch_vm_statistics64_newer) / sizeof(integer_t)))

/* Fills the newer fields the caller's count has room for after rev3's; returns the count filled (rev3's at least). */
mach_msg_type_number_t switch_vm_stats_newer(void *info, mach_msg_type_number_t count);

#endif
