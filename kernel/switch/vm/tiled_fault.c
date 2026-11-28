// SPDX-License-Identifier: GPL-2.0-only
#include <switch/tiled_fault.h>

#include <kern/task.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <vm/pmap.h>
#include <vm/vm_map_internal.h>
#if XNU_12377_121_6
#include <vm/vm_map_lock_internal.h>
#include <vm/vm_object_internal.h>
#include <vm/vm_protos_internal.h>
#else
#include <vm/vm_object.h>
#include <vm/vm_protos.h>
#endif

extern int tegra_gm20b_tiled_cpu_fault(void *task, uint64_t vaddr, uint64_t pa, uint32_t prot) __attribute__((weak));

uint32_t switch_vm_tiled_live;
uint16_t switch_vm_tiled_counts[SWITCH_VM_TILED_BUCKETS];

void switch_vm_tiled_mark_pages(const uint64_t *pas, uint32_t npages, int mark)
{
    if (!pas || !npages) {
        return;
    }
    uint32_t last_pn = UINT32_MAX;
    for (uint32_t i = 0; i < npages; ++i) {
        const uint32_t pn = (uint32_t)(pas[i] >> PAGE_SHIFT);
        if (pn == last_pn) {
            continue;
        }
        last_pn = pn;
        const uint32_t b = switch_vm_tiled_bucket(pn);
        if (mark) {
            __atomic_add_fetch(&switch_vm_tiled_counts[b], 1u, __ATOMIC_RELEASE);
        } else {
            __atomic_sub_fetch(&switch_vm_tiled_counts[b], 1u, __ATOMIC_RELEASE);
        }
    }
    if (mark) {
        __atomic_add_fetch(&switch_vm_tiled_live, 1u, __ATOMIC_RELEASE);
    } else {
        __atomic_sub_fetch(&switch_vm_tiled_live, 1u, __ATOMIC_RELEASE);
    }
}

void switch_vm_disconnect_pages(const uint64_t *pas, uint32_t npages)
{
    if (!pas || !npages) {
        return;
    }
    uint32_t last_pn = UINT32_MAX;
    for (uint32_t i = 0; i < npages; ++i) {
        const uint32_t pn = (uint32_t)(pas[i] >> PAGE_SHIFT);
        if (pn == last_pn) {
            continue;
        }
        last_pn = pn;
        pmap_disconnect((ppnum_t)pn);
    }
}

void switch_vm_protect_ro(void *task_arg, uint64_t address, uint64_t length)
{
    task_t task = (task_t)task_arg;
    if (task == TASK_NULL || length == 0u) {
        return;
    }
    vm_map_t map = get_task_map_reference(task);
    if (map == VM_MAP_NULL) {
        return;
    }
    if (map != kernel_map && map->pmap != kernel_pmap) {
        pmap_protect(map->pmap, (vm_map_offset_t)address, (vm_map_offset_t)(address + length), VM_PROT_READ);
    }
    vm_map_deallocate(map);
}

static uint32_t wimg_to_cache_index(uint32_t wimg)
{
    switch (wimg & VM_WIMG_MASK) {
    case VM_WIMG_IO:
        return 1u;
    case VM_WIMG_WTHRU:
        return 2u;
    case VM_WIMG_COPYBACK:
        return 3u;
    case VM_WIMG_WCOMB:
        return 4u;
    case VM_WIMG_INNERWBACK:
        return 5u;
    default:
        return 0u;
    }
}

int switch_vm_mapping_bounds(void *task_arg, uint64_t vaddr, uint64_t *start_out, uint64_t *end_out,
                             uint32_t *wimg_out, int *writable_out)
{
    task_t task = (task_t)task_arg;
    if (task == TASK_NULL || !start_out || !end_out || !wimg_out || !writable_out) {
        return 0;
    }
    vm_map_t map = get_task_map_reference(task);
    if (map == VM_MAP_NULL) {
        return 0;
    }
    if (map == kernel_map || map->pmap == kernel_pmap || map->mapped_in_other_pmaps) {
        vm_map_deallocate(map);
        return 0;
    }
    const vm_map_offset_t addr = vm_map_trunc_page((vm_map_offset_t)vaddr, VM_MAP_PAGE_MASK(map));
    const vm_map_offset_t end = addr + (vm_map_size_t)VM_MAP_PAGE_SIZE(map);
    int found = 0;

#if XNU_12377_121_6
    if (!vm_map_is_sealed(map)) {
        VM_MAP_LOCK_CTX_DECLARE(ctx);
        vm_map_t wmap = map;
        kern_return_t kr = vm_map_range_sh_lock(ctx, &wmap, addr, end, VMRL_SH_STREAM_NO_HOLES);
        if (kr == KERN_SUCCESS) {
            vm_map_entry_t entry = vm_map_range_stream_next_with_error(ctx, &kr);
            if (entry != VM_MAP_ENTRY_NULL && !vm_map_lock_ctx_is_descended(ctx) && !VME_IS_SENTINEL(entry) &&
                !entry->is_sub_map && !entry->vme_permanent && !entry->superpage_size) {
                vm_object_t obj = VME_OBJECT(entry);
                *start_out = (uint64_t)entry->vme_start;
                *end_out = (uint64_t)entry->vme_end;
                *wimg_out = obj != VM_OBJECT_NULL ? wimg_to_cache_index(obj->wimg_bits) : 0u;
                *writable_out = (entry->protection & VM_PROT_WRITE) != 0 ? 1 : 0;
                found = 1;
            }
            vm_map_range_sh_unlock(ctx, &wmap);
        }
    }
#else
    vm_map_lock_read(map);
    vm_map_entry_t entry = VM_MAP_ENTRY_NULL;
    if (vm_map_lookup_entry(map, addr, &entry) && !entry->is_sub_map && !entry->vme_permanent &&
        !entry->superpage_size) {
        vm_object_t obj = VME_OBJECT(entry);
        *start_out = (uint64_t)entry->vme_start;
        *end_out = (uint64_t)entry->vme_end;
        *wimg_out = obj != VM_OBJECT_NULL ? wimg_to_cache_index(obj->wimg_bits) : 0u;
        *writable_out = (entry->protection & VM_PROT_WRITE) != 0 ? 1 : 0;
        found = 1;
    }
    vm_map_unlock_read(map);
#endif

    vm_map_deallocate(map);
    return found;
}

int switch_vm_task_active(void *task_arg)
{
    task_t task = (task_t)task_arg;
    return task != TASK_NULL && task_is_active(task) ? 1 : 0;
}

void switch_vm_task_hold(void *task_arg, int hold)
{
    task_t task = (task_t)task_arg;
    if (task == TASK_NULL) {
        return;
    }
    if (hold) {
        task_reference(task);
    } else {
        task_deallocate(task);
    }
}

int switch_vm_tiled_cpu_fault(void *map_arg, uint64_t vaddr, uint64_t pa, uint32_t prot)
{
    if (!tegra_gm20b_tiled_cpu_fault) {
        return 0;
    }
    task_t task = current_task();
    if (task == TASK_NULL || task == kernel_task || get_task_map(task) != (vm_map_t)map_arg) {
        return 0;
    }
    return tegra_gm20b_tiled_cpu_fault(task, vaddr, pa, prot);
}
