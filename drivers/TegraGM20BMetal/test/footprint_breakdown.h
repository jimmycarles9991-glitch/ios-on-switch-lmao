// SPDX-License-Identifier: MIT
// A process's dirty memory, region by region (mach_vm_region_recurse, into the shared cache's submap), named by the image
// an address lies in (dladdr) or, outside every image, by its VM tag (VM_MEMORY_MALLOC_*, stacks, IOKit...): the pages
// phys_footprint charges, attributed for a host test's report.
#pragma once
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace footprint {

inline int64_t physFootprint()
{
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count);
    return int64_t(vm.phys_footprint);
}

inline std::string tagName(unsigned tag)
{
    switch (tag) {
    case VM_MEMORY_MALLOC:
    case VM_MEMORY_MALLOC_SMALL:
    case VM_MEMORY_MALLOC_LARGE:
    case VM_MEMORY_MALLOC_LARGE_REUSED:
    case VM_MEMORY_MALLOC_HUGE:
    case VM_MEMORY_MALLOC_TINY:
    case VM_MEMORY_MALLOC_NANO:
    case VM_MEMORY_MALLOC_MEDIUM:
        return "malloc";
    case VM_MEMORY_STACK:
        return "stacks";
    case VM_MEMORY_IOKIT:
        return "IOKit";
    case VM_MEMORY_OS_ALLOC_ONCE:
        return "os_alloc_once";
    case VM_MEMORY_DYLD:
    case VM_MEMORY_DYLD_MALLOC:
        return "dyld";
    case VM_MEMORY_OBJC_DISPATCHERS:
        return "objc";
    default:
        return "tag " + std::to_string(tag);
    }
}

// Dirty bytes by owner: "<image basename>" or "[<tag>]"
inline std::map<std::string, int64_t> dirtyByOwner()
{
    std::map<std::string, int64_t> owners;
    const vm_size_t page = vm_page_size;
    mach_vm_address_t at = 0;
    natural_t depth = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info = {};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &at, &size, &depth,
                                   reinterpret_cast<vm_region_recurse_info_t>(&info), &count) != KERN_SUCCESS)
            break;
        if (info.is_submap) {
            depth++;
            continue;
        }
        const int64_t dirty =
            int64_t(info.pages_dirtied) * int64_t(page) + int64_t(info.pages_swapped_out) * int64_t(page);
        if (dirty && info.share_mode != SM_SHARED && info.share_mode != SM_TRUESHARED) {
            Dl_info image = {};
            std::string owner;
            if (dladdr(reinterpret_cast<const void *>(uintptr_t(at)), &image) && image.dli_fname &&
                uintptr_t(image.dli_fbase) <= uintptr_t(at)) {
                owner = image.dli_fname;
                owner = owner.substr(owner.rfind('/') + 1);
            } else {
                owner = "[" + tagName(info.user_tag) + "]";
            }
            owners[owner] += dirty;
        }
        at += size;
    }
    return owners;
}

// The `top` largest owners of what grew from `before` to `after`, then the rest as one row
inline std::string growth(const std::map<std::string, int64_t> &before, const std::map<std::string, int64_t> &after,
                          size_t top = 12)
{
    std::vector<std::pair<int64_t, std::string>> rows;
    for (const auto &[owner, bytes] : after) {
        const auto was = before.find(owner);
        const int64_t grew = bytes - (was == before.end() ? 0 : was->second);
        if (grew)
            rows.push_back({grew, owner});
    }
    std::sort(rows.rbegin(), rows.rend());
    std::string text;
    int64_t rest = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        if (i < top)
            text += " " + rows[i].second + " " + std::to_string(rows[i].first / 1024) + "K";
        else
            rest += rows[i].first;
    }
    if (rest)
        text += " other " + std::to_string(rest / 1024) + "K";
    return text;
}

}  // namespace footprint
