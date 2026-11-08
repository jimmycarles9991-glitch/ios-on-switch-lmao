/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: zeroed kernel heap (kits/SwitchLinux). */
#ifndef BPXNU_LINUX_SLAB_H
#define BPXNU_LINUX_SLAB_H
#include <linux/kernel.h>
#define GFP_KERNEL 0u
static inline void *kzalloc(size_t size, gfp_t flags) { return switch_linux_malloc(size, 1); }
static inline void *kcalloc(size_t n, size_t size, gfp_t flags)
{
	return n && size > (size_t)-1 / n ? NULL : switch_linux_malloc(n * size, 1);
}
static inline void kfree(const void *p) { switch_linux_free(p); }
#endif
