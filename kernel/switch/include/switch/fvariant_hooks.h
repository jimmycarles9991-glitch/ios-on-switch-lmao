// SPDX-License-Identifier: GPL-2.0-only
/* The shared region's calls into kernel/switch/vm/fvariant.c (switch/fvariant.h). */
#ifndef SWITCH_FVARIANT_HOOKS_H
#define SWITCH_FVARIANT_HOOKS_H

#include <stdint.h>

struct switch_fvariant_cache;
struct vm_shared_region;
struct vm_shared_region_slide_info;

/* The pinned slots of the cache `sr` maps, read from its header; NULL for a cache with none pinned. */
const struct switch_fvariant_cache *switch_fvariant_for_region(struct vm_shared_region *sr);

/* Fills the slots of si's cache in [uaddr, uaddr + size), a just-slid part of a page at kernel address `page`. */
void switch_fvariant_fill_page(const struct vm_shared_region_slide_info *si, uint8_t *page, uint64_t uaddr,
                               uint64_t size);

#endif
