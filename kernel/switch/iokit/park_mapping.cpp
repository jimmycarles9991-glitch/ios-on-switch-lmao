// SPDX-License-Identifier: GPL-2.0-only
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMultiMemoryDescriptor.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <switch/park_mapping.h>
#include <switch/park_mapping_internal.h>

namespace {
constexpr uint32_t kInlineSegs = 32u;

struct SubDescAccessor : IOSubMemoryDescriptor {
    static IOMemoryDescriptor *parent(const IOSubMemoryDescriptor *sub)
    {
        return static_cast<const SubDescAccessor *>(sub)->_parent;
    }

    static uint64_t start(const IOSubMemoryDescriptor *sub)
    {
        return static_cast<uint64_t>(static_cast<const SubDescAccessor *>(sub)->_start);
    }
};

struct MultiDescAccessor : IOMultiMemoryDescriptor {
    static IOMemoryDescriptor *const *descriptors(const IOMultiMemoryDescriptor *multi)
    {
        return static_cast<const MultiDescAccessor *>(multi)->_descriptors;
    }

    static uint32_t count(const IOMultiMemoryDescriptor *multi)
    {
        return static_cast<uint32_t>(static_cast<const MultiDescAccessor *>(multi)->_descriptorsCount);
    }
};

struct SegList {
    switch_vm_object_seg inline_segs[kInlineSegs];
    uint32_t inline_counts[kInlineSegs];
    switch_vm_object_seg *segs = inline_segs;
    uint32_t *counts = inline_counts;
    uint32_t count = 0;
    uint32_t capacity = kInlineSegs;

    ~SegList()
    {
        if (segs != inline_segs) {
            IODelete(segs, switch_vm_object_seg, capacity);
            IODelete(counts, uint32_t, capacity);
        }
    }

    bool reserve(uint32_t need)
    {
        if (need <= capacity) {
            return true;
        }
        auto *new_segs = IONew(switch_vm_object_seg, need);
        auto *new_counts = new_segs ? IONew(uint32_t, need) : nullptr;
        if (!new_segs || !new_counts) {
            if (new_segs) {
                IODelete(new_segs, switch_vm_object_seg, need);
            }
            if (new_counts) {
                IODelete(new_counts, uint32_t, need);
            }
            return false;
        }
        if (segs != inline_segs) {
            IODelete(segs, switch_vm_object_seg, capacity);
            IODelete(counts, uint32_t, capacity);
        }
        segs = new_segs;
        counts = new_counts;
        capacity = need;
        return true;
    }

    bool push(const switch_vm_object_seg &seg)
    {
        if (count >= capacity) {
            const uint32_t next_cap = capacity * 2u;
            auto *new_segs = IONew(switch_vm_object_seg, next_cap);
            auto *new_counts = new_segs ? IONew(uint32_t, next_cap) : nullptr;
            if (!new_segs || !new_counts) {
                if (new_segs) {
                    IODelete(new_segs, switch_vm_object_seg, next_cap);
                }
                if (new_counts) {
                    IODelete(new_counts, uint32_t, next_cap);
                }
                return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                new_segs[i] = segs[i];
            }
            if (segs != inline_segs) {
                IODelete(segs, switch_vm_object_seg, capacity);
                IODelete(counts, uint32_t, capacity);
            }
            segs = new_segs;
            counts = new_counts;
            capacity = next_cap;
        }
        segs[count++] = seg;
        return true;
    }
};

bool append_gmd_segments(const IOGeneralMemoryDescriptor *gmd, uint64_t gmd_offset, uint64_t gmd_len,
                         uint64_t va_offset, bool mapped, bool has_kernel_map, bool for_park, SegList *out)
{
    switch_iokit_mem_entry entries_inline[4];
    switch_iokit_mem_entry *entries = entries_inline;
    uint32_t entry_count = 0;
    bool has_map_ref = false;
    bool ok = switch_iokit_gmd_entries(gmd, entries_inline, 4u, &entry_count, &has_map_ref);
    uint32_t alloc_cap = 0;
    if (!ok) {
        if (for_park || entry_count == 0u) {
            return false;
        }
        entries = IONew(switch_iokit_mem_entry, entry_count);
        if (!entries) {
            return false;
        }
        alloc_cap = entry_count;
        ok = switch_iokit_gmd_entries(gmd, entries, alloc_cap, &entry_count, &has_map_ref);
        if (!ok) {
            IODelete(entries, switch_iokit_mem_entry, alloc_cap);
            return false;
        }
    }

    bool result = true;
    if (for_park) {
        if (has_map_ref || entry_count != 1u) {
            result = false;
        }
    } else if (entry_count == 0u) {
        if (alloc_cap) {
            IODelete(entries, switch_iokit_mem_entry, alloc_cap);
        }
        return true;
    }

    const uint32_t expected_krefs = 2u + (has_kernel_map ? 1u : 0u);
    uint64_t covered = 0;
    for (uint32_t i = 0; result && i < entry_count; ++i) {
        void *vm_obj = nullptr;
        uint64_t ne_off = 0;
        bool is_internal = false;
        if (!switch_vm_resolve_mem_entry(entries[i].port, &vm_obj, &ne_off, &is_internal)) {
            result = false;
            break;
        }
        if (for_park && !is_internal) {
            result = false;
            break;
        }
        if (!mapped) {
            switch_vm_object_seg seg = {vm_obj, 0u, ne_off, entries[i].size, expected_krefs, false};
            if (!out->push(seg)) {
                result = false;
            }
            continue;
        }
        const int64_t raw_start = entries[i].offset;
        const uint64_t entry_start = raw_start < 0 ? 0u : static_cast<uint64_t>(raw_start);
        const uint64_t trim_lead = raw_start < 0 ? static_cast<uint64_t>(-raw_start) : 0u;
        if (trim_lead >= entries[i].size) {
            continue;
        }
        const uint64_t entry_end = entry_start + (entries[i].size - trim_lead);
        const uint64_t req_start = gmd_offset;
        const uint64_t req_end = gmd_offset + gmd_len;
        if (entry_end <= req_start || entry_start >= req_end) {
            continue;
        }
        const uint64_t seg_start = entry_start > req_start ? entry_start : req_start;
        const uint64_t seg_end = entry_end < req_end ? entry_end : req_end;
        const uint64_t seg_bytes = seg_end - seg_start;
        const uint64_t seg_obj_off = ne_off + trim_lead + (seg_start - entry_start);
        const uint64_t seg_va_off = va_offset + (seg_start - req_start);
        switch_vm_object_seg seg = {vm_obj, seg_va_off, seg_obj_off, seg_bytes, expected_krefs, true};
        if (!out->push(seg)) {
            result = false;
            break;
        }
        covered += seg_bytes;
    }

    if (alloc_cap) {
        IODelete(entries, switch_iokit_mem_entry, alloc_cap);
    }
    if (result && for_park && mapped && covered != gmd_len) {
        return false;
    }
    return result;
}

bool collect_descriptor_segments(const IOMemoryDescriptor *desc, uint64_t offset, uint64_t bytes, bool has_kernel_map,
                                 bool for_park, SegList *out)
{
    if (!desc || bytes == 0u) {
        return false;
    }
    while (const auto *sub = OSDynamicCast(IOSubMemoryDescriptor, desc)) {
        const uint64_t sub_start = SubDescAccessor::start(sub);
        if (offset > UINT64_MAX - sub_start) {
            return false;
        }
        offset += sub_start;
        desc = SubDescAccessor::parent(sub);
        if (!desc) {
            return false;
        }
    }
    const uint64_t total_len = desc->getLength();
    if (offset > total_len || bytes > total_len - offset) {
        return false;
    }

    if (const auto *gmd = OSDynamicCast(IOGeneralMemoryDescriptor, desc)) {
        return append_gmd_segments(gmd, offset, bytes, 0u, true, has_kernel_map, for_park, out);
    }
    const auto *multi = OSDynamicCast(IOMultiMemoryDescriptor, desc);
    if (!multi) {
        return false;
    }
    const uint32_t sub_count = MultiDescAccessor::count(multi);
    IOMemoryDescriptor *const *subs = MultiDescAccessor::descriptors(multi);
    if (!subs || sub_count == 0u || !out->reserve(sub_count)) {
        return false;
    }
    uint64_t cursor = 0;
    uint64_t mapped_covered = 0;
    const uint64_t req_end = offset + bytes;
    for (uint32_t i = 0; i < sub_count; ++i) {
        const IOMemoryDescriptor *child = subs[i];
        const auto *child_gmd = OSDynamicCast(IOGeneralMemoryDescriptor, child);
        if (!child_gmd) {
            return false;
        }
        if (for_park && child_gmd->getRetainCount() != 1) {
            return false;
        }
        const uint64_t child_len = child_gmd->getLength();
        const uint64_t child_end = cursor + child_len;
        if (child_end > offset && cursor < req_end) {
            const uint64_t clip_start = cursor > offset ? cursor : offset;
            const uint64_t clip_end = child_end < req_end ? child_end : req_end;
            const uint64_t chunk = clip_end - clip_start;
            if (!append_gmd_segments(child_gmd, clip_start - cursor, chunk, clip_start - offset, true, has_kernel_map,
                                     for_park, out)) {
                return false;
            }
            mapped_covered += chunk;
        } else if (for_park) {
            if (!append_gmd_segments(child_gmd, 0u, child_len, 0u, false, has_kernel_map, true, out)) {
                return false;
            }
        }
        cursor = child_end;
    }
    if (for_park && mapped_covered != bytes) {
        return false;
    }
    return true;
}

bool desc_is_physical(const IOMemoryDescriptor *desc)
{
    while (const auto *sub = OSDynamicCast(IOSubMemoryDescriptor, desc)) {
        desc = SubDescAccessor::parent(sub);
    }
    if (switch_iokit_desc_is_physical(desc)) {
        return true;
    }
    if (const auto *multi = OSDynamicCast(IOMultiMemoryDescriptor, desc)) {
        const uint32_t sub_count = MultiDescAccessor::count(multi);
        IOMemoryDescriptor *const *subs = MultiDescAccessor::descriptors(multi);
        for (uint32_t i = 0; subs && i < sub_count; ++i) {
            if (desc_is_physical(subs[i])) {
                return true;
            }
        }
    }
    return false;
}
}  // namespace

kern_return_t switch_iokit_park_replace(task_t task, IOMemoryMap *old_map, IOMemoryDescriptor *buffer_mem,
                                        IOMemoryDescriptor *old_view, bool has_kernel_map, uint64_t address,
                                        uint64_t bytes, bool guard_none, bool *stale_dropped)
{
    if (stale_dropped) {
        *stale_dropped = false;
    }
    if (!old_map || !buffer_mem || !old_view || task == TASK_NULL || bytes == 0u) {
        return KERN_INVALID_ARGUMENT;
    }
    // Check map, view and backing exclusivity before mutating any VM entry or map tracking.
    if (!switch_iokit_map_check_or_neuter(old_map, buffer_mem, task, address, bytes, false)) {
        return KERN_RESOURCE_SHORTAGE;
    }
    // buffer_mem retains: bo->buffer + bo->backing + old_view + old_map (+ buffer.map when kernel-mapped).
    const int expected_live_retains = 4 + (has_kernel_map ? 1 : 0);
    if (buffer_mem->getRetainCount() != expected_live_retains) {
        return KERN_RESOURCE_SHORTAGE;
    }
    if (old_view != buffer_mem && old_view->getRetainCount() != 1) {
        return KERN_RESOURCE_SHORTAGE;
    }

    SegList segs;
    if (!collect_descriptor_segments(buffer_mem, 0u, bytes, has_kernel_map, true, &segs) || segs.count == 0u) {
        return KERN_RESOURCE_SHORTAGE;
    }

    const kern_return_t kr = switch_vm_park_range(task, address, bytes, segs.segs, segs.count, segs.counts, guard_none);
    if (kr != KERN_SUCCESS) {
        // Neuter the stale IOMemoryMap only if none of this descriptor's vm_objects remain in [address, address + bytes).
        if (kr == KERN_INVALID_ADDRESS && !switch_vm_range_has_objects(task, address, bytes, segs.segs, segs.count)) {
            if (switch_iokit_map_check_or_neuter(old_map, buffer_mem, task, address, bytes, true)) {
                old_map->release();
                if (stale_dropped) {
                    *stale_dropped = true;
                }
            }
        }
        return kr;
    }

    switch_iokit_map_neuter_after_park(old_map);
    old_map->release();
    return KERN_SUCCESS;
}

IOReturn switch_iokit_unpark_range(IOMemoryDescriptor *desc, task_t task, uint64_t address, uint64_t bytes,
                                   IOOptionBits options, bool expected_guard_none, IOMemoryMap **out_map)
{
    if (out_map) {
        *out_map = nullptr;
    }
    if (!desc || !out_map || task == TASK_NULL || bytes == 0u || bytes > desc->getLength()) {
        return kIOReturnBadArgument;
    }
    const kern_return_t kr = switch_vm_verify_parked_range(task, address, bytes, expected_guard_none);
    if (kr != KERN_SUCCESS) {
        return kr == KERN_INVALID_TASK ? kIOReturnNotOpen : kIOReturnVMError;
    }
    const bool is_physical = desc_is_physical(desc);
    const IOOptionBits map_opts = (options & (kIOMapCacheMask | kIOMapReadOnly | kIOMapPrefault)) | kIOMapOverwrite |
                                  (is_physical ? 0u : kIOMapUnique);
    IOMemoryMap *map = desc->createMappingInTask(task, address, map_opts, 0, bytes);
    if (!map) {
        SegList segs;
        if (collect_descriptor_segments(desc, 0u, bytes, false, false, &segs) && segs.count != 0u) {
            switch_vm_repark_matching(task, address, bytes, segs.segs, segs.count, segs.counts, expected_guard_none);
        }
        return kIOReturnNoMemory;
    }
    if (is_physical && !switch_iokit_map_is_unique(map)) {
        map->release();
        return kIOReturnBusy;
    }
    if (map->getAddress() != address || map->getSize() != bytes) {
        SegList segs;
        const uint64_t map_addr = map->getAddress();
        const uint64_t map_size = map->getSize();
        const bool has_range = map_addr != 0u && map_size != 0u;
        if (has_range && collect_descriptor_segments(desc, 0u, map_size, false, false, &segs) && segs.count != 0u) {
            if (map_addr == address && map_size <= bytes) {
                switch_vm_repark_matching(task, map_addr, map_size, segs.segs, segs.count, segs.counts,
                                          expected_guard_none);
            } else {
                switch_vm_unmap_matching(task, map_addr, map_size, segs.segs, segs.count);
            }
        }
        map->unmap();
        map->release();
        return kIOReturnVMError;
    }
    *out_map = map;
    return kIOReturnSuccess;
}

IOReturn switch_iokit_map_fixed(IOMemoryDescriptor *desc, task_t task, uint64_t address, IOOptionBits options,
                                IOMemoryMap **out_map)
{
    if (out_map) {
        *out_map = nullptr;
    }
    if (!desc || !out_map || task == TASK_NULL) {
        return kIOReturnBadArgument;
    }
    const uint64_t bytes = desc->getLength();
    if (bytes == 0u) {
        return kIOReturnBadArgument;
    }
    const kern_return_t kr = switch_vm_verify_fixed_target(task, address, bytes);
    if (kr != KERN_SUCCESS) {
        return kr == KERN_INVALID_TASK ? kIOReturnNotOpen : kIOReturnVMError;
    }
    // Physical descriptors mapped with kIOMapUnique detach IOMemoryMap onto a temporary descriptor without kPoolCacheMode.
    const bool is_physical = desc_is_physical(desc);
    const IOOptionBits map_opts = (options & (kIOMapCacheMask | kIOMapReadOnly | kIOMapPrefault)) | kIOMapOverwrite |
                                  (is_physical ? 0u : kIOMapUnique);
    IOMemoryMap *map = desc->createMappingInTask(task, address, map_opts, 0, bytes);
    if (!map) {
        SegList segs;
        if (collect_descriptor_segments(desc, 0u, bytes, false, false, &segs) && segs.count != 0u) {
            switch_vm_unmap_matching(task, address, bytes, segs.segs, segs.count);
        }
        return kIOReturnNoMemory;
    }
    if (is_physical && !switch_iokit_map_is_unique(map)) {
        map->release();
        return kIOReturnBusy;
    }
    if (map->getAddress() != address || map->getSize() != bytes) {
        switch_iokit_unmap_overwrite(task, map, map->getAddress(), map->getSize());
        return kIOReturnVMError;
    }
    *out_map = map;
    return kIOReturnSuccess;
}

void switch_iokit_unmap_overwrite(task_t task, IOMemoryMap *map, uint64_t address, uint64_t bytes)
{
    if (task != TASK_NULL && map != nullptr && address != 0u && bytes != 0u) {
        const IOMemoryDescriptor *desc = map->getMemoryDescriptor();
        SegList segs;
        if (desc && collect_descriptor_segments(desc, map->fOffset, bytes, false, false, &segs) && segs.count != 0u) {
            switch_vm_unmap_matching(task, address, bytes, segs.segs, segs.count);
        }
    }
    if (map) {
        map->unmap();
        map->release();
    }
}

void switch_iokit_drop_parked_range(task_t task, uint64_t address, uint64_t bytes, bool guard_none)
{
    if (task != TASK_NULL && address != 0u && bytes != 0u) {
        switch_vm_drop_parked_range(task, address, bytes, guard_none);
    }
}
