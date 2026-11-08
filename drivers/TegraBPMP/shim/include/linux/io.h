/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: MMIO accessors. The BPMP channel areas are device memory, so copies use naturally aligned
 * 32-bit accesses where they can and single bytes otherwise (no unaligned or paired accesses to Device memory). */
#ifndef BPXNU_LINUX_IO_H
#define BPXNU_LINUX_IO_H
/* The GM20B shim (same kernel) defines Linux names too: every function here is bpmp_linux_-prefixed. */
#define memcpy_toio(a0, a1, a2) bpmp_linux_memcpy_toio(a0, a1, a2)
#define memcpy_fromio(a0, a1, a2) bpmp_linux_memcpy_fromio(a0, a1, a2)
#include <linux/kernel.h>
#include "../../../../../kits/SwitchMMIO/SwitchMMIO.h"
#define __raw_readl(a) switch_mmio_read32((const volatile u8 *)(a), 0)
#define __raw_writel(v, a) switch_mmio_write32((volatile u8 *)(a), 0, (v))
#define readl(a) __raw_readl(a)
#define writel(v, a) switch_mmio_write32_publish((volatile u8 *)(a), 0, (v))
#define writel_relaxed(v, a) __raw_writel(v, a)
void memcpy_toio(volatile void *dst, const void *src, size_t n);
void memcpy_fromio(void *dst, const volatile void *src, size_t n);
static inline void *ioremap(phys_addr_t pa, size_t size) { return (void *)switch_linux_ioremap(pa, size, 0); }
#endif
