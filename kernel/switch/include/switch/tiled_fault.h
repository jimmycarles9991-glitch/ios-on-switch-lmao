// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_VM_TILED_BUCKETS 32768u

extern uint32_t switch_vm_tiled_live;
extern uint16_t switch_vm_tiled_counts[SWITCH_VM_TILED_BUCKETS];

static inline uint32_t switch_vm_tiled_bucket(uint32_t pn)
{
    return (pn ^ (pn >> 15)) & (SWITCH_VM_TILED_BUCKETS - 1u);
}

static inline bool switch_vm_tiled_page_maybe(uint32_t pn)
{
    if (__builtin_expect(__atomic_load_n(&switch_vm_tiled_live, __ATOMIC_RELAXED) == 0u, 1)) {
        return false;
    }
    return __atomic_load_n(&switch_vm_tiled_counts[switch_vm_tiled_bucket(pn)], __ATOMIC_ACQUIRE) != 0u;
}

int switch_vm_tiled_cpu_fault(void *map, uint64_t vaddr, uint64_t pa, uint32_t prot);
void switch_vm_tiled_mark_pages(const uint64_t *pas, uint32_t npages, int mark);
void switch_vm_disconnect_pages(const uint64_t *pas, uint32_t npages);
void switch_vm_protect_ro(void *task, uint64_t address, uint64_t length);
int switch_vm_mapping_bounds(void *task, uint64_t vaddr, uint64_t *start_out, uint64_t *end_out,
                             uint32_t *wimg_out, int *writable_out);
int switch_vm_task_active(void *task);
void switch_vm_task_hold(void *task, int hold);

#ifdef __cplusplus
}
#endif
