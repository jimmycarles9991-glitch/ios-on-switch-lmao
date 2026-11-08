// SPDX-License-Identifier: BSD-2-Clause-Patent
// The 4 KiB granule the hardware works in (SMMU and GMMU pages, engine page lists, the kernel's translation granule),
// named apart from the VM page: PAGE_SIZE is the VM's, 16 KiB once the kernel runs 16 KiB VM pages on this 4 KiB
// granule. Code meaning the hardware's page says SWITCH_HW_PAGE_*, never a bare 4096 or PAGE_SIZE.
#ifndef SWITCH_HW_PAGE_H
#define SWITCH_HW_PAGE_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_HW_PAGE_SHIFT 12
#define SWITCH_HW_PAGE_SIZE  (1u << SWITCH_HW_PAGE_SHIFT)
#define SWITCH_HW_PAGE_MASK  ((uint64_t)SWITCH_HW_PAGE_SIZE - 1)

#ifdef KERNEL
#include <mach/boolean.h>
#include <mach/vm_param.h>
#include <mach/vm_types.h>

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
boolean_t pmap_valid_page(ppnum_t pn);

// RAM the VM manages (what phystokv reaches). pmap_valid_page takes a VM page number: atop_64, never
// pa >> SWITCH_HW_PAGE_SHIFT, which names a page four times too far once VM pages are 16 KiB.
static inline bool switch_dma_pa_managed(uint64_t pa)
{
    return pmap_valid_page((ppnum_t)atop_64(pa)) != 0;
}

#ifdef __cplusplus
}
#endif
#endif

#endif
