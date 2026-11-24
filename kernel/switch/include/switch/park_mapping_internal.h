// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
class IOGeneralMemoryDescriptor;
class IOMemoryDescriptor;
class IOMemoryMap;

extern "C" {
#endif

struct switch_iokit_mem_entry {
    ipc_port_t port;
    int64_t offset;
    uint64_t size;
};

struct switch_vm_object_seg {
    void *vm_object;
    uint64_t va_offset;
    uint64_t obj_offset;
    uint64_t bytes;
    uint32_t expected_krefs;
    bool mapped;
};

#ifdef __cplusplus
bool switch_iokit_gmd_entries(const IOGeneralMemoryDescriptor *gmd, struct switch_iokit_mem_entry *entries,
                              uint32_t capacity, uint32_t *count_out, bool *has_map_ref_out);
bool switch_iokit_desc_is_physical(const IOMemoryDescriptor *desc);
bool switch_iokit_map_is_unique(const IOMemoryMap *map);
bool switch_iokit_map_check_or_neuter(IOMemoryMap *map, const IOMemoryDescriptor *expected_mem, task_t task,
                                      uint64_t address, uint64_t bytes, bool neuter);
void switch_iokit_map_neuter_after_park(IOMemoryMap *map);
#endif

bool switch_vm_resolve_mem_entry(ipc_port_t port, void **vm_object_out, uint64_t *obj_offset_out,
                                 bool *is_internal_out);
kern_return_t switch_vm_park_range(task_t task, uint64_t address, uint64_t bytes,
                                   const struct switch_vm_object_seg *segs, uint32_t seg_count,
                                   uint32_t *scratch_counts, bool guard_none);
void switch_vm_repark_matching(task_t task, uint64_t address, uint64_t bytes, const struct switch_vm_object_seg *segs,
                               uint32_t seg_count, uint32_t *scratch_counts, bool guard_none);
kern_return_t switch_vm_verify_parked_range(task_t task, uint64_t address, uint64_t bytes, bool guard_none);
kern_return_t switch_vm_verify_fixed_target(task_t task, uint64_t address, uint64_t bytes);
bool switch_vm_range_has_objects(task_t task, uint64_t address, uint64_t bytes, const struct switch_vm_object_seg *segs,
                                 uint32_t seg_count);
void switch_vm_unmap_matching(task_t task, uint64_t address, uint64_t bytes, const struct switch_vm_object_seg *segs,
                              uint32_t seg_count);
void switch_vm_drop_parked_range(task_t task, uint64_t address, uint64_t bytes, bool guard_none);

#ifdef __cplusplus
}
#endif
