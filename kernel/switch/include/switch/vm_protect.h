// SPDX-License-Identifier: GPL-2.0-only
/*
 * vm_map_protect hooks. Included by osfmk/vm/vm_map.c after its own headers (vm_prot_t, VM_PROT_*).
 */
#ifndef SWITCH_VM_PROTECT_H
#define SWITCH_VM_PROTECT_H

#include <stdbool.h>

/*
 * A nested submap entry whose effective protection (and max protection when set_max is set) already equals the
 * requested protection needs no clip, unnest, or pmap update. Rejecting W+X prevents preflight from skipping an entry
 * before a later entry in the range strips VM_PROT_ALLEXEC from new_prot.
 */
static inline bool switch_vm_protect_submap_noop(bool is_sub_map, bool use_pmap, vm_prot_t cur_prot, vm_prot_t max_prot,
                                                 vm_prot_t new_prot, bool set_max)
{
    if (!is_sub_map || !use_pmap) {
        return false;
    }
    if ((new_prot & VM_PROT_WRITE) && (new_prot & VM_PROT_ALLEXEC)) {
        return false;
    }
    if (set_max) {
        return max_prot == new_prot && (new_prot & cur_prot) == cur_prot;
    }
    return cur_prot == new_prot;
}

#endif
