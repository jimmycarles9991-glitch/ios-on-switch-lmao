// SPDX-License-Identifier: GPL-2.0-only
/*
 * The native full-RAM boot as the loader hands it over (tools/package/native_xnu_layout.py): boot_args' physBase and
 * memSize cover all of DRAM, the kernel's lowest segment sits at SWITCH_KERNEL_PHYS_BASE and its trust caches directly
 * below (tools/package/loader_abi.py KERNEL_PHYS_BASE). That segment's virtual address is the IPSW kind's
 * (ipsw/pins.bzl's derived kernel_virt_base), so boot_args' virtBase differs per kind. kernel/switch/objects.bzl gives
 * SWITCH_KERNEL_VIRT_BASE to the objects that include this.
 *
 * Plain integer constants: osfmk/arm64/start.s includes it too.
 */
#ifndef SWITCH_BOOT_LAYOUT_H
#define SWITCH_BOOT_LAYOUT_H

#ifndef SWITCH_KERNEL_VIRT_BASE
#error "SWITCH_KERNEL_VIRT_BASE is the IPSW kind's kernel_virt_base (kernel/switch/objects.bzl)"
#endif

#define SWITCH_BOOT_PHYS_BASE   0x80000000
#define SWITCH_BOOT_MEM_SIZE    0x100000000
#define SWITCH_KERNEL_PHYS_BASE 0xb207c000
#define SWITCH_BOOT_VIRT_BASE   (SWITCH_KERNEL_VIRT_BASE - SWITCH_KERNEL_PHYS_BASE + SWITCH_BOOT_PHYS_BASE)

#endif
