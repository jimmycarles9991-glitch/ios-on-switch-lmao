// SPDX-License-Identifier: GPL-2.0-only
#include <sys/errno.h>
#include <switch/app_code.h>
#include <switch/dyld_policy_24B119.h>
#include <mach-o/loader.h>
#include <mach/vm_attributes.h>
#include <vm/vm_kern_xnu.h>
#include <vm/vm_map_internal.h>

int switch_app_code_dyld(struct _vm_map *map, const void *commands, size_t size, int64_t slide)
{
    if (!map || !switch_app_code_map_enabled(map) || size < sizeof(struct mach_header_64) ||
        ((const struct mach_header_64 *)commands)->filetype != MH_DYLINKER) return 0;
    uint64_t address = 0;
    if (slide < 0 || !switch_dyld_policy_24B119_site(commands, size, (uint64_t)slide, &address)) return EBADMACHO;
    uint32_t words[3];
    if (copyinmap(map, address, words, sizeof(words)) != KERN_SUCCESS ||
        !switch_dyld_policy_24B119_patch(words)) return EBADMACHO;
    const vm_map_offset_t start = vm_map_trunc_page(address, vm_map_page_mask(map));
    const vm_map_offset_t end = start + vm_map_page_size(map);
    // This private, unscheduled map needs dyld's missing-signature gate to reach ordinary library admission.
    kern_return_t kr = vm_map_protect(map, start, end, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) return EACCES;
    kr = copyoutmap(map, words + 2, address + 8, sizeof(words[2]));
    if (kr != KERN_SUCCESS) return EFAULT;
    vm_machine_attribute_val_t attribute = MATTR_VAL_CACHE_FLUSH;
    kr = vm_map_machine_attribute(map, start, end, MATTR_CACHE, &attribute);
    if (kr != KERN_SUCCESS) return EIO;
    kr = vm_map_protect(map, start, end, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) return EACCES;
    kr = vm_map_protect(map, start, end, TRUE, VM_PROT_READ | VM_PROT_EXECUTE);
    return kr == KERN_SUCCESS ? 0 : EACCES;
}
