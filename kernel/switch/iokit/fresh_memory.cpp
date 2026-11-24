// SPDX-License-Identifier: GPL-2.0-only
/*
 * See switch/fresh_memory.h. The kmem flags are IOBufferMemoryDescriptor's for a wired buffer under a physical mask
 * (IOKernelAllocateWithPhysicalRestrict) less KMA_ZERO: data memory, which IOKit maps into tasks, from the shared data
 * range on xnu-12377.
 */
#include <IOKit/IOMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <switch/fresh_memory.h>
#if XNU_12377_121_6
#include <vm/vm_kern_xnu.h>
#else
#include <vm/vm_kern.h>
#endif

class SwitchFreshMemory : public IOGeneralMemoryDescriptor {
    OSDeclareDefaultStructors(SwitchFreshMemory);

public:
    vm_offset_t pages;
    vm_size_t bytes;

protected:
    void free() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SwitchFreshMemory, IOGeneralMemoryDescriptor);

void SwitchFreshMemory::free()
{
    const vm_offset_t va = pages;
    const vm_size_t size = bytes;
    // the descriptor unwires and unmaps in its own free; the pages go after it, as IOBufferMemoryDescriptor's do
    IOGeneralMemoryDescriptor::free();
    if (va) {
        kmem_free(kernel_map, va, size);
    }
}

IOMemoryDescriptor *switch_iokit_fresh_memory(uint64_t bytes, uint64_t run, void **cpu)
{
#if XNU_12377_121_6
    const kma_flags_t flags = KMA_DATA_SHARED;
#else
    const kma_flags_t flags = KMA_DATA;
#endif
    const vm_size_t size = round_page(bytes);
    vm_offset_t va = 0;
    kern_return_t kr;
    if (run) {
        const vm_offset_t mask = run - 1;
        kr = kmem_alloc_contig(kernel_map, &va, size, mask, 0, (ppnum_t)atop(mask), flags, VM_KERN_MEMORY_IOKIT);
    } else {
        kr = kernel_memory_allocate(kernel_map, &va, size, 0, flags, VM_KERN_MEMORY_IOKIT);
    }
    if (kr != KERN_SUCCESS) {
        return nullptr;
    }
    SwitchFreshMemory *memory = OSTypeAlloc(SwitchFreshMemory);
    if (!memory) {
        kmem_free(kernel_map, va, size);
        return nullptr;
    }
    memory->pages = va;
    memory->bytes = size;
    IOAddressRange range = {va, size};
    const IOOptionBits options = kIOMemoryTypeVirtual64 | kIODirectionInOut | kIOMemoryMapperNone;
    if (!memory->initWithOptions(&range, 1, 0, kernel_task, options, nullptr)) {
        memory->release();
        return nullptr;
    }
    *cpu = reinterpret_cast<void *>(va);
    return memory;
}
