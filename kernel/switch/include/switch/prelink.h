// SPDX-License-Identifier: GPL-2.0-only
/*
 * The prelinked kexts' segments in arm_vm_prot_init (osfmk/arm64/arm_vm_init.c).
 */
#ifndef SWITCH_PRELINK_H
#define SWITCH_PRELINK_H

/*
 * Where the gap below XNU's __TEXT starts, which arm_vm_prot_init maps RWNX: past both read-only kext segments. kcgen
 * lays out __PLK_TEXT_EXEC below __PLK_DATA_CONST (iOS 17's kernelcache), but a fileset prelink keeps the fileset's
 * order, __PLK_DATA_CONST first (iOS 27, translate/arm64e/xnu/prelink26), and its kext code must stay executable.
 */
static inline vm_offset_t switch_kext_readonly_end(vm_offset_t data_const_end, vm_offset_t text_exec_end)
{
    return data_const_end > text_exec_end ? data_const_end : text_exec_end;
}

#endif
