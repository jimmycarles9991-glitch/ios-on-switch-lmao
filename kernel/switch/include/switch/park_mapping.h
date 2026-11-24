// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#include <IOKit/IOTypes.h>

class IOMemoryDescriptor;
class IOMemoryMap;

kern_return_t switch_iokit_park_replace(task_t task, IOMemoryMap *old_map, IOMemoryDescriptor *buffer_mem,
                                        IOMemoryDescriptor *old_view, bool has_kernel_map, uint64_t address,
                                        uint64_t bytes, bool guard_none, bool *stale_dropped);
IOReturn switch_iokit_unpark_range(IOMemoryDescriptor *desc, task_t task, uint64_t address, uint64_t bytes,
                                   IOOptionBits options, bool expected_guard_none, IOMemoryMap **out_map);
IOReturn switch_iokit_map_fixed(IOMemoryDescriptor *desc, task_t task, uint64_t address, IOOptionBits options,
                                IOMemoryMap **out_map);
void switch_iokit_unmap_overwrite(task_t task, IOMemoryMap *map, uint64_t address, uint64_t bytes);
void switch_iokit_drop_parked_range(task_t task, uint64_t address, uint64_t bytes, bool guard_none);
#endif
