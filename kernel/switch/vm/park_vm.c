// SPDX-License-Identifier: GPL-2.0-only
#include <switch/park_mapping_internal.h>

#include <kern/task.h>
#include <mach/vm_inherit.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <mach/vm_statistics.h>
#include <vm/pmap.h>
#include <vm/vm_map_internal.h>
#if XNU_12377_121_6
#include <vm/vm_map_lock_internal.h>
#include <vm/vm_memory_entry_xnu.h>
#include <vm/vm_object_internal.h>
#include <vm/vm_protos_internal.h>
#else
#include <vm/vm_object.h>
#include <vm/vm_protos.h>
#endif

static bool range_valid_for_map(vm_map_t map, uint64_t address, uint64_t bytes)
{
    if (map == VM_MAP_NULL || map == kernel_map || map->pmap == kernel_pmap || map->mapped_in_other_pmaps) {
        return false;
    }
    if (bytes == 0u || address > UINT64_MAX - bytes || ((address | bytes) & (uint64_t)VM_MAP_PAGE_MASK(map)) != 0u) {
        return false;
    }
    const vm_map_offset_t start = (vm_map_offset_t)address;
    const vm_map_offset_t end = start + (vm_map_size_t)bytes;
    return start >= vm_map_min(map) && end <= vm_map_max(map);
}

static bool entry_is_sentinel(vm_map_entry_t entry)
{
#if XNU_12377_121_6
    return VME_IS_SENTINEL(entry);
#else
    (void)entry;
    return false;
#endif
}

static bool entry_modifiable_base(vm_map_entry_t entry)
{
    return !entry_is_sentinel(entry) && !entry->is_sub_map && !entry->vme_permanent && entry->wired_count == 0u &&
           entry->user_wired_count == 0u && !entry->superpage_size && !entry->used_for_jit && !entry->csm_associated;
}

static int find_segment_for_entry(vm_map_entry_t entry, vm_map_offset_t range_start, vm_map_offset_t range_end,
                                  const struct switch_vm_object_seg *segs, uint32_t seg_count,
                                  bool require_internal_iokit, bool require_no_straddle)
{
    if (!entry_modifiable_base(entry)) {
        return -1;
    }
    if (require_no_straddle && (entry->vme_start < range_start || entry->vme_end > range_end || entry->vme_atomic)) {
        return -1;
    }
    if (entry->vme_atomic && (entry->vme_start < range_start || entry->vme_end > range_end)) {
        return -1;
    }
    vm_object_t obj = VME_OBJECT(entry);
    if (obj == VM_OBJECT_NULL || is_kernel_object(obj)) {
        return -1;
    }
    if (require_internal_iokit) {
        if (!entry->iokit_acct || !obj->internal) {
            return -1;
        }
    } else if (obj->internal ? !entry->iokit_acct : !obj->private) {
        return -1;
    }

    const vm_map_offset_t clip_start = entry->vme_start > range_start ? entry->vme_start : range_start;
    const vm_map_offset_t clip_end = entry->vme_end < range_end ? entry->vme_end : range_end;
    if (clip_end <= clip_start) {
        return -1;
    }
    const uint64_t entry_obj_off = (uint64_t)VME_OFFSET(entry) + (uint64_t)(clip_start - entry->vme_start);

    for (uint32_t i = 0; i < seg_count; ++i) {
        if (!segs[i].mapped || (vm_object_t)segs[i].vm_object != obj) {
            continue;
        }
        const vm_map_offset_t seg_start = range_start + (vm_map_offset_t)segs[i].va_offset;
        const vm_map_offset_t seg_end = seg_start + (vm_map_size_t)segs[i].bytes;
        if (clip_start < seg_start || clip_end > seg_end) {
            continue;
        }
        if (entry_obj_off == segs[i].obj_offset + (uint64_t)(clip_start - seg_start)) {
            return (int)i;
        }
    }
    return -1;
}

static bool entry_is_parked(vm_map_entry_t entry, bool guard_none)
{
    if (!entry_modifiable_base(entry) || entry->iokit_acct || entry->is_shared || VME_ALIAS(entry) != VM_MEMORY_IOKIT) {
        return false;
    }
    vm_object_t obj = VME_OBJECT(entry);
    if (guard_none) {
        return obj == VM_OBJECT_NULL && entry->protection == VM_PROT_NONE && entry->max_protection == VM_PROT_NONE;
    }
    return obj == VM_OBJECT_NULL || (obj->internal && !is_kernel_object(obj));
}

static bool entry_is_fixed_target(vm_map_entry_t entry)
{
    if (!entry_modifiable_base(entry) || entry->iokit_acct || entry->is_shared) {
        return false;
    }
    vm_object_t obj = VME_OBJECT(entry);
    return obj == VM_OBJECT_NULL || (obj->internal && !is_kernel_object(obj));
}

static bool verify_objects_exclusive(const struct switch_vm_object_seg *segs, const uint32_t *user_counts,
                                     uint32_t seg_count)
{
    for (uint32_t i = 0; i < seg_count; ++i) {
        vm_object_t obj = (vm_object_t)segs[i].vm_object;
        if (obj == VM_OBJECT_NULL) {
            return false;
        }
        bool seen = false;
        for (uint32_t j = 0; j < i; ++j) {
            if ((vm_object_t)segs[j].vm_object == obj) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        uint32_t total_user = 0;
        for (uint32_t k = i; k < seg_count; ++k) {
            if ((vm_object_t)segs[k].vm_object == obj) {
                if (segs[k].expected_krefs != segs[i].expected_krefs) {
                    return false;
                }
                total_user += user_counts[k];
            }
        }
        vm_object_lock(obj);
#if XNU_12377_121_6
        const uint32_t refs = os_ref_get_count_raw(&obj->ref_count);
#else
        const uint32_t refs = (uint32_t)obj->ref_count;
#endif
        const bool ok = obj->internal && !is_kernel_object(obj) && obj->alive && !obj->terminating && !obj->shadowed &&
                        obj->vo_copy == VM_OBJECT_NULL && obj->shadow == VM_OBJECT_NULL &&
                        refs == segs[i].expected_krefs + total_user;
        vm_object_unlock(obj);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void convert_entry_to_parked_locked(vm_map_t map, vm_map_entry_t e, bool guard_none)
{
    pmap_remove(map->pmap, e->vme_start, e->vme_end);
    vm_map_iokit_unmapped_region(map, e->vme_end - e->vme_start);
    e->iokit_acct = FALSE;
    e->use_pmap = TRUE;
    e->is_shared = FALSE;
    e->needs_copy = FALSE;
    e->no_cache = FALSE;
    e->zero_wired_pages = FALSE;
    e->behavior = VM_BEHAVIOR_DEFAULT;
    e->inheritance = VM_INHERIT_NONE;
    e->protection = guard_none ? VM_PROT_NONE : VM_PROT_DEFAULT;
    e->max_protection = guard_none ? VM_PROT_NONE : VM_PROT_ALL;
    e->vme_resilient_codesign = FALSE;
    e->vme_resilient_media = FALSE;
    e->vme_no_copy_on_read = FALSE;
    e->translated_allow_execute = FALSE;
#ifdef __arm64e__
    e->used_for_tpro = FALSE;
#endif
#if XNU_12377_121_6 && HAS_MTE
    e->vme_is_tagged = FALSE;
#endif
    VME_OBJECT_SET(e, VM_OBJECT_NULL, false, 0);
    VME_OFFSET_SET(e, 0);
    VME_ALIAS_SET(e, VM_MEMORY_IOKIT);
}

static void deallocate_removed_objects(const struct switch_vm_object_seg *segs, const uint32_t *counts,
                                       uint32_t seg_count)
{
    for (uint32_t i = 0; i < seg_count; ++i) {
        vm_object_t obj = (vm_object_t)segs[i].vm_object;
        for (uint32_t n = 0; n < counts[i]; ++n) {
            vm_object_deallocate(obj);
        }
    }
}

#if !XNU_12377_121_6
static void simplify_parked_range_locked(vm_map_t map, vm_map_offset_t start, vm_map_offset_t end)
{
    vm_map_offset_t cursor = start;
    while (cursor < end) {
        vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
        if (!vm_map_lookup_entry_or_next(map, cursor, &entry) || entry == vm_map_to_entry(map) ||
            entry->vme_start >= end) {
            break;
        }
        const vm_map_offset_t next_va = entry->vme_end;
        vm_map_simplify_entry(map, entry);
        if (next_va <= cursor) {
            break;
        }
        cursor = next_va;
    }
}
#endif

bool switch_vm_resolve_mem_entry(ipc_port_t port, void **vm_object_out, uint64_t *obj_offset_out, bool *is_internal_out)
{
    if (vm_object_out) {
        *vm_object_out = NULL;
    }
    if (obj_offset_out) {
        *obj_offset_out = 0;
    }
    if (is_internal_out) {
        *is_internal_out = false;
    }
    vm_named_entry_t ne = port != IP_NULL ? mach_memory_entry_from_port(port) : NULL;
    if (ne == NULL) {
        return false;
    }
    named_entry_lock(ne);
    bool ok = false;
    vm_object_t obj = VM_OBJECT_NULL;
    uint64_t base_offset = 0;
    bool internal = false;
    if (ne->is_object && !ne->is_copy && !ne->is_sub_map) {
        obj = vm_named_entry_to_vm_object(ne);
        base_offset = (uint64_t)(ne->offset + ne->data_offset);
        if (obj != VM_OBJECT_NULL && !is_kernel_object(obj)) {
            internal = obj->internal;
            ok = true;
        }
    }
    named_entry_unlock(ne);
    if (!ok) {
        return false;
    }
    if (vm_object_out) {
        *vm_object_out = obj;
    }
    if (obj_offset_out) {
        *obj_offset_out = base_offset;
    }
    if (is_internal_out) {
        *is_internal_out = internal;
    }
    return true;
}

kern_return_t switch_vm_park_range(task_t task, uint64_t address, uint64_t bytes,
                                   const struct switch_vm_object_seg *segs, uint32_t seg_count,
                                   uint32_t *scratch_counts, bool guard_none)
{
    if (task == TASK_NULL || !segs || seg_count == 0u || !scratch_counts) {
        return KERN_INVALID_ARGUMENT;
    }
    vm_map_t map = get_task_map_reference(task);
    if (map == VM_MAP_NULL) {
        return KERN_INVALID_TASK;
    }
    if (!range_valid_for_map(map, address, bytes)) {
        vm_map_deallocate(map);
        return KERN_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < seg_count; ++i) {
        scratch_counts[i] = 0u;
    }
    const vm_map_offset_t start = (vm_map_offset_t)address;
    const vm_map_offset_t end = start + (vm_map_size_t)bytes;
    kern_return_t kr = KERN_SUCCESS;

#if XNU_12377_121_6
    if (vm_map_is_sealed(map)) {
        vm_map_deallocate(map);
        return KERN_INVALID_ADDRESS;
    }
    VM_MAP_LOCK_CTX_DECLARE(ctx);
    vm_map_t wmap = map;
    vm_map_lock_ctx_set_preflight(ctx, ^kern_return_t(vm_map_lock_ctx_t vctx, vm_map_entry_t vme) {
        if (vm_map_lock_ctx_is_descended(vctx) ||
            find_segment_for_entry(vme, start, end, segs, seg_count, true, true) < 0) {
            return KERN_INVALID_ADDRESS;
        }
        return KERN_SUCCESS;
    });
    kr = vm_map_range_ex_lock(ctx, &wmap, start, end, VMRL_EX_ATOMIC | VMRL_EX_NO_PMAP_UNNEST | VMRL_EX_SIMPLIFY);
    if (kr != KERN_SUCCESS) {
        vm_map_deallocate(map);
        return kr == KERN_ABORTED ? KERN_RESOURCE_SHORTAGE : KERN_INVALID_ADDRESS;
    }
    vm_map_offset_t cursor = start;
    vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
    while ((entry = vm_map_range_atomic_next(ctx)) != VM_MAP_ENTRY_NULL) {
        const int idx = (!vm_map_lock_ctx_is_descended(ctx) && entry->vme_start == cursor)
                            ? find_segment_for_entry(entry, start, end, segs, seg_count, true, true)
                            : -1;
        if (idx < 0) {
            kr = KERN_INVALID_ADDRESS;
            break;
        }
        scratch_counts[idx]++;
        cursor = entry->vme_end;
    }
    if (kr == KERN_SUCCESS && cursor != end) {
        kr = KERN_INVALID_ADDRESS;
    }
    if (kr == KERN_SUCCESS && !verify_objects_exclusive(segs, scratch_counts, seg_count)) {
        kr = KERN_RESOURCE_SHORTAGE;
    }
    if (kr == KERN_SUCCESS) {
        vm_map_range_atomic_reset(ctx);
        while ((entry = vm_map_range_atomic_next(ctx)) != VM_MAP_ENTRY_NULL) {
            convert_entry_to_parked_locked(ctx->vmlc_map, entry, guard_none);
        }
    }
    vm_map_range_ex_unlock(ctx, &wmap);
#else
    vm_map_lock(map);
    vm_map_entry_t first = VM_MAP_ENTRY_NULL;
    if (!vm_map_lookup_entry_or_next(map, start, &first)) {
        kr = KERN_INVALID_ADDRESS;
    } else {
        vm_map_offset_t cursor = start;
        vm_map_entry_t entry = first;
        while (cursor < end) {
            if (entry == vm_map_to_entry(map) || entry->vme_start != cursor) {
                kr = KERN_INVALID_ADDRESS;
                break;
            }
            if (entry->in_transition) {
                kr = KERN_RESOURCE_SHORTAGE;
                break;
            }
            const int idx = find_segment_for_entry(entry, start, end, segs, seg_count, true, true);
            if (idx < 0) {
                kr = KERN_INVALID_ADDRESS;
                break;
            }
            scratch_counts[idx]++;
            cursor = entry->vme_end;
            entry = entry->vme_next;
        }
        if (kr == KERN_SUCCESS && cursor != end) {
            kr = KERN_INVALID_ADDRESS;
        }
        if (kr == KERN_SUCCESS && !verify_objects_exclusive(segs, scratch_counts, seg_count)) {
            kr = KERN_RESOURCE_SHORTAGE;
        }
        if (kr == KERN_SUCCESS) {
            for (entry = first; entry != vm_map_to_entry(map) && entry->vme_start < end; entry = entry->vme_next) {
                convert_entry_to_parked_locked(map, entry, guard_none);
            }
            simplify_parked_range_locked(map, start, end);
        }
    }
    vm_map_unlock(map);
#endif

    if (kr == KERN_SUCCESS) {
        deallocate_removed_objects(segs, scratch_counts, seg_count);
    }
    vm_map_deallocate(map);
    return kr;
}

void switch_vm_repark_matching(task_t task, uint64_t address, uint64_t bytes, const struct switch_vm_object_seg *segs,
                               uint32_t seg_count, uint32_t *scratch_counts, bool guard_none)
{
    if (task == TASK_NULL || !segs || seg_count == 0u || !scratch_counts) {
        return;
    }
    vm_map_t map = get_task_map_reference(task);
    if (map == VM_MAP_NULL || !range_valid_for_map(map, address, bytes)) {
        if (map != VM_MAP_NULL) {
            vm_map_deallocate(map);
        }
        return;
    }
    for (uint32_t i = 0; i < seg_count; ++i) {
        scratch_counts[i] = 0u;
    }
    const vm_map_offset_t start = (vm_map_offset_t)address;
    const vm_map_offset_t end = start + (vm_map_size_t)bytes;

#if XNU_12377_121_6
    if (!vm_map_is_sealed(map)) {
        VM_MAP_LOCK_CTX_DECLARE(ctx);
        vm_map_t wmap = map;
        vm_map_lock_ctx_set_preflight(ctx, ^kern_return_t(vm_map_lock_ctx_t vctx, vm_map_entry_t vme) {
            if (vm_map_lock_ctx_is_descended(vctx) ||
                find_segment_for_entry(vme, start, end, segs, seg_count, true, true) < 0) {
                return VMRL_ERR_SKIP_PREPARE;
            }
            return KERN_SUCCESS;
        });
        const vmrl_ex_flags_t flags = VMRL_EX_STREAM | VMRL_EX_NO_PMAP_UNNEST | VMRL_EX_SIMPLIFY;
        kern_return_t kr = vm_map_range_ex_lock(ctx, &wmap, start, end, flags);
        if (kr == KERN_SUCCESS) {
            vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
            while ((entry = vm_map_range_stream_next_with_error(ctx, &kr)) != VM_MAP_ENTRY_NULL) {
                if (!vm_map_lock_ctx_is_descended(ctx)) {
                    const int idx = find_segment_for_entry(entry, start, end, segs, seg_count, true, true);
                    if (idx >= 0) {
                        convert_entry_to_parked_locked(ctx->vmlc_map, entry, guard_none);
                        scratch_counts[idx]++;
                    }
                }
            }
            vm_map_range_ex_unlock(ctx, &wmap);
        }
    }
#else
    vm_map_lock(map);
    vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
    (void)vm_map_lookup_entry_or_next(map, start, &entry);
    bool converted = false;
    while (entry != vm_map_to_entry(map) && entry->vme_start < end) {
        if (!entry->in_transition) {
            const int idx = find_segment_for_entry(entry, start, end, segs, seg_count, true, true);
            if (idx >= 0) {
                convert_entry_to_parked_locked(map, entry, guard_none);
                scratch_counts[idx]++;
                converted = true;
            }
        }
        entry = entry->vme_next;
    }
    if (converted) {
        simplify_parked_range_locked(map, start, end);
    }
    vm_map_unlock(map);
#endif

    deallocate_removed_objects(segs, scratch_counts, seg_count);
    vm_map_deallocate(map);
}

enum scan_kind {
    SCAN_PARKED,
    SCAN_FIXED_TARGET,
    SCAN_HAS_OBJECTS,
};

static bool entry_has_any_object(vm_map_entry_t entry, const struct switch_vm_object_seg *segs, uint32_t seg_count)
{
    if (entry_is_sentinel(entry) || entry->is_sub_map) {
        return false;
    }
    vm_object_t obj = VME_OBJECT(entry);
    if (obj == VM_OBJECT_NULL) {
        return false;
    }
    for (uint32_t i = 0; i < seg_count; ++i) {
        if ((vm_object_t)segs[i].vm_object == obj) {
            return true;
        }
    }
    return false;
}

static kern_return_t scan_range_shared(task_t task, uint64_t address, uint64_t bytes, enum scan_kind kind,
                                       bool guard_none, const struct switch_vm_object_seg *segs, uint32_t seg_count,
                                       bool *result_out)
{
    *result_out = (kind == SCAN_HAS_OBJECTS);
    if (task == TASK_NULL || (kind == SCAN_HAS_OBJECTS && (!segs || seg_count == 0u))) {
        return KERN_INVALID_ARGUMENT;
    }
    vm_map_t map = get_task_map_reference(task);
    if (map == VM_MAP_NULL) {
        *result_out = false;
        return KERN_INVALID_TASK;
    }
    if (!range_valid_for_map(map, address, bytes)) {
        vm_map_deallocate(map);
        return KERN_INVALID_ARGUMENT;
    }
    const vm_map_offset_t start = (vm_map_offset_t)address;
    const vm_map_offset_t end = start + (vm_map_size_t)bytes;
    bool res = (kind != SCAN_HAS_OBJECTS);

#if XNU_12377_121_6
    if (vm_map_is_sealed(map)) {
        res = (kind == SCAN_HAS_OBJECTS);
    } else {
        VM_MAP_LOCK_CTX_DECLARE(ctx);
        vm_map_t wmap = map;
        const vmrl_sh_flags_t flags = (kind == SCAN_PARKED) ? VMRL_SH_STREAM_NO_HOLES : VMRL_SH_STREAM;
        kern_return_t kr = vm_map_range_sh_lock(ctx, &wmap, start, end, flags);
        if (kr == KERN_INVALID_ADDRESS) {
            res = (kind == SCAN_FIXED_TARGET);
        } else if (kr != KERN_SUCCESS) {
            res = (kind == SCAN_HAS_OBJECTS);
        } else {
            vm_map_offset_t cursor = start;
            vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
            res = (kind != SCAN_HAS_OBJECTS);
            while ((entry = vm_map_range_stream_next_with_error(ctx, &kr)) != VM_MAP_ENTRY_NULL) {
                if (kind == SCAN_HAS_OBJECTS) {
                    if (!vm_map_lock_ctx_is_descended(ctx) && entry_has_any_object(entry, segs, seg_count)) {
                        res = true;
                        break;
                    }
                    continue;
                }
                if (vm_map_lock_ctx_is_descended(ctx)) {
                    res = false;
                    break;
                }
                if (kind == SCAN_PARKED) {
                    vm_map_address_t e_start = 0;
                    vm_map_address_t e_end = 0;
                    vm_map_lock_ctx_bounds(ctx, &e_start, &e_end, NULL);
                    if (e_start > cursor || !entry_is_parked(entry, guard_none)) {
                        res = false;
                        break;
                    }
                    if (e_end > cursor) {
                        cursor = e_end;
                    }
                } else if (!entry_is_fixed_target(entry)) {
                    res = false;
                    break;
                }
            }
            if (kr != KERN_SUCCESS) {
                res = (kind == SCAN_HAS_OBJECTS);
            } else if (kind == SCAN_PARKED && cursor != end) {
                res = false;
            }
            vm_map_range_sh_unlock(ctx, &wmap);
        }
    }
#else
    vm_map_lock_read(map);
    vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
    const bool found_start = vm_map_lookup_entry_or_next(map, start, &entry);
    if (kind == SCAN_PARKED) {
        vm_map_offset_t cursor = start;
        res = found_start;
        while (res && cursor < end) {
            if (entry == vm_map_to_entry(map) || entry->vme_start > cursor || !entry_is_parked(entry, guard_none)) {
                res = false;
                break;
            }
            cursor = entry->vme_end < end ? entry->vme_end : end;
            entry = entry->vme_next;
        }
    } else {
        res = (kind == SCAN_FIXED_TARGET);
        while (entry != vm_map_to_entry(map) && entry->vme_start < end) {
            if (kind == SCAN_HAS_OBJECTS) {
                if (entry_has_any_object(entry, segs, seg_count)) {
                    res = true;
                    break;
                }
            } else if (!entry_is_fixed_target(entry)) {
                res = false;
                break;
            }
            entry = entry->vme_next;
        }
    }
    vm_map_unlock_read(map);
#endif

    vm_map_deallocate(map);
    *result_out = res;
    return KERN_SUCCESS;
}

kern_return_t switch_vm_verify_parked_range(task_t task, uint64_t address, uint64_t bytes, bool guard_none)
{
    bool ok = false;
    const kern_return_t kr = scan_range_shared(task, address, bytes, SCAN_PARKED, guard_none, NULL, 0u, &ok);
    return kr != KERN_SUCCESS ? kr : (ok ? KERN_SUCCESS : KERN_INVALID_ADDRESS);
}

kern_return_t switch_vm_verify_fixed_target(task_t task, uint64_t address, uint64_t bytes)
{
    bool ok = false;
    const kern_return_t kr = scan_range_shared(task, address, bytes, SCAN_FIXED_TARGET, false, NULL, 0u, &ok);
    return kr != KERN_SUCCESS ? kr : (ok ? KERN_SUCCESS : KERN_INVALID_ADDRESS);
}

bool switch_vm_range_has_objects(task_t task, uint64_t address, uint64_t bytes, const struct switch_vm_object_seg *segs,
                                 uint32_t seg_count)
{
    bool found = true;
    (void)scan_range_shared(task, address, bytes, SCAN_HAS_OBJECTS, false, segs, seg_count, &found);
    return found;
}

static bool match_for_unmap(vm_map_entry_t entry, vm_map_offset_t start, vm_map_offset_t end,
                            const struct switch_vm_object_seg *segs, uint32_t seg_count, bool drop_parked,
                            bool guard_none)
{
    if (drop_parked) {
        if (entry->vme_atomic && (entry->vme_start < start || entry->vme_end > end)) {
            return false;
        }
        return entry_is_parked(entry, guard_none);
    }
    return find_segment_for_entry(entry, start, end, segs, seg_count, false, false) >= 0;
}

static void remove_matching_atomic(task_t task, uint64_t address, uint64_t bytes,
                                   const struct switch_vm_object_seg *segs, uint32_t seg_count, bool drop_parked,
                                   bool guard_none)
{
    if (task == TASK_NULL || (!drop_parked && (!segs || seg_count == 0u))) {
        return;
    }
    vm_map_t map = get_task_map_reference(task);
    if (map == VM_MAP_NULL || !range_valid_for_map(map, address, bytes)) {
        if (map != VM_MAP_NULL) {
            vm_map_deallocate(map);
        }
        return;
    }
    const vm_map_offset_t start = (vm_map_offset_t)address;
    const vm_map_offset_t end = start + (vm_map_size_t)bytes;

#if XNU_12377_121_6
    if (!vm_map_is_sealed(map) && !(map->corpse_source && !map->terminated)) {
        VM_MAP_LOCK_CTX_DECLARE(ctx);
        vm_map_t wmap = map;
        vm_map_lock_ctx_set_preflight(ctx, ^kern_return_t(vm_map_lock_ctx_t vctx, vm_map_entry_t vme) {
            if (vm_map_lock_ctx_is_descended(vctx) ||
                !match_for_unmap(vme, start, end, segs, seg_count, drop_parked, guard_none)) {
                return VMRL_ERR_SKIP_PREPARE;
            }
            return KERN_SUCCESS;
        });
        kern_return_t kr = vm_map_range_ex_lock(ctx, &wmap, start, end, VMRL_EX_STREAM | VMRL_EX_NO_PMAP_UNNEST);
        vm_map_entry_t zapped = VM_MAP_ENTRY_NULL;
        if (kr == KERN_SUCCESS) {
            vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
            while ((entry = vm_map_range_stream_next_with_error(ctx, &kr)) != VM_MAP_ENTRY_NULL) {
                if (vm_map_lock_ctx_is_descended(ctx) || entry->vme_start < start || entry->vme_end > end ||
                    !match_for_unmap(entry, start, end, segs, seg_count, drop_parked, guard_none)) {
                    continue;
                }
                vm_map_found_entry_ex_pop_curr(ctx);
                vm_map_remove_entry(ctx->vmlc_map, entry, VM_MAP_REMOVE_NO_FLAGS);
                entry->vme_next = zapped;
                zapped = entry;
            }
            vm_map_range_ex_unlock(ctx, &wmap);
            if (zapped != VM_MAP_ENTRY_NULL && map->wait_for_space) {
                thread_wakeup((event_t)map);
            }
            while (zapped != VM_MAP_ENTRY_NULL) {
                vm_map_entry_t next = zapped->vme_next;
                zapped->vme_next = VM_MAP_ENTRY_NULL;
                zapped->vme_prev = VM_MAP_ENTRY_NULL;
                vm_map_entry_free_locked(NULL, zapped);
                zapped = next;
            }
        }
    }
#else
    vm_map_offset_t cursor = start;
    while (cursor < end) {
        vm_map_lock(map);
        vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
        (void)vm_map_lookup_entry_or_next(map, cursor, &entry);
        vm_map_offset_t sub_start = 0;
        vm_map_offset_t sub_end = 0;
        bool wait_transition = false;
        while (entry != vm_map_to_entry(map) && entry->vme_start < end) {
            if (entry->in_transition) {
                entry->needs_wakeup = TRUE;
                wait_transition = true;
                break;
            }
            if (match_for_unmap(entry, start, end, segs, seg_count, drop_parked, guard_none)) {
                sub_start = entry->vme_start > cursor ? entry->vme_start : cursor;
                sub_end = entry->vme_end < end ? entry->vme_end : end;
                break;
            }
            entry = entry->vme_next;
        }
        if (wait_transition) {
            vm_map_entry_wait(map, THREAD_UNINT);
            continue;
        }
        if (sub_end <= sub_start) {
            vm_map_unlock(map);
            break;
        }
        const kmem_return_t kr = vm_map_remove_and_unlock(
            map, sub_start, sub_end, VM_MAP_REMOVE_GAPS_FAIL | VM_MAP_REMOVE_NO_YIELD, KMEM_GUARD_NONE);
        (void)kr;
        cursor = sub_end;
    }
#endif

    vm_map_deallocate(map);
}

void switch_vm_unmap_matching(task_t task, uint64_t address, uint64_t bytes, const struct switch_vm_object_seg *segs,
                              uint32_t seg_count)
{
    remove_matching_atomic(task, address, bytes, segs, seg_count, false, false);
}

void switch_vm_drop_parked_range(task_t task, uint64_t address, uint64_t bytes, bool guard_none)
{
    remove_matching_atomic(task, address, bytes, NULL, 0u, true, guard_none);
}
