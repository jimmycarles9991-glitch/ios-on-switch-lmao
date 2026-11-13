// SPDX-License-Identifier: GPL-2.0-only
/*
 * vm_statistics64 revisions 4 to 6 from this XNU's counters (switch/vm_stats.h). Fields this XNU keeps no counter for
 * (shared region pages, the active/inactive split of internal and external pages, phantom cache ghosts, MTE tag
 * storage) read 0.
 */

#include <switch/vm_stats.h>

#include <kern/counter.h>
#include <mach/vm_param.h>
#include <string.h>
#include <vm/vm_page.h>

_Static_assert(SWITCH_HOST_VM_INFO64_COUNT == 104, "iOS 27's HOST_VM_INFO64_COUNT");
_Static_assert(SWITCH_HOST_VM_INFO64_COUNT <= HOST_INFO_MAX, "host_info64 reply size");

#if DEVELOPMENT || DEBUG
extern unsigned int vm_page_speculative_created;
#endif
extern unsigned int vm_swapfile_total_segs_used;
extern uint32_t c_seg_bufsize;

mach_msg_type_number_t switch_vm_stats_newer(void *info, mach_msg_type_number_t count)
{
    struct switch_vm_statistics64_newer s = {0};
    mach_msg_type_number_t filled = SWITCH_HOST_VM_INFO64_COUNT;

    if (count < SWITCH_HOST_VM_INFO64_REV4_COUNT) {
        return HOST_VM_INFO64_REV3_COUNT;
    }
#if DEVELOPMENT || DEBUG
    s.speculative_pages_created = vm_page_speculative_created;
    s.speculative_pages_activated = vm_page_speculative_used;
#endif
    s.swap_count = (uint64_t)vm_swapfile_total_segs_used * (c_seg_bufsize / PAGE_SIZE);
    s.executable_count = vm_page_xpmapped_external_count;
    s.boot_stolen_count = vm_page_stolen_count;
#if CONFIG_SECLUDED_MEMORY
    s.secluded_count = vm_page_secluded_count;
#endif
    const uint64_t purgeable = counter_load(&vm_page_purgeable_count);
    s.purgeable_wired_count = counter_load(&vm_page_purgeable_wired_count);
    s.purgeable_pageable_count = purgeable > s.purgeable_wired_count ? purgeable - s.purgeable_wired_count : 0;
    s.background_internal_count = vm_page_background_internal_count;
    s.background_external_count = vm_page_background_external_count;
    s.donated_count = vm_page_donate_count;
    s.realtime_count = vm_page_realtime_count;
    s.max_mem_count = atop_64(max_mem);
    if (count < SWITCH_HOST_VM_INFO64_REV5_COUNT) {
        filled = SWITCH_HOST_VM_INFO64_REV4_COUNT;
    } else if (count < SWITCH_HOST_VM_INFO64_COUNT) {
        filled = SWITCH_HOST_VM_INFO64_REV5_COUNT;
    }
    memcpy((integer_t *)info + HOST_VM_INFO64_REV3_COUNT, &s, (filled - HOST_VM_INFO64_REV3_COUNT) * sizeof(integer_t));
    return filled;
}
