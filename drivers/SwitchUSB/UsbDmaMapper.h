// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMAMapper: DMA for Apple's USB host stack on the Tegra X1, whose XUSB host does not snoop the A57 caches.
//
// AppleUSBHostController asks its subclass for a mapper (copyMapper, vtable slot 253) and keeps it as the mapper of its
// buffers and DMA commands, so every DMA address AppleUSBXHCI hands the controller comes from here:
//   * a buffer whose physical pieces meet on page boundaries (contiguous memory, or the scattered pages of large I/O)
//     gets one IOVA range over its own pages through the MC SMMU (UsbSmmuMapper.cpp); a cacheable one is cleaned before
//     the controller reads it and cleaned and invalidated after it may have written (IOMemoryDescriptor
//     performOperation, kIOMemoryIncoherentIOStore/Flush). Controller structures (rings, contexts, scratchpads) are
//     allocated non-cacheable: SwitchXHCI gives AppleUSBXHCI's ring buffer options the write-combine cache mode;
//   * anything else (pieces that break inside a page, as mbuf chains do, or a full window) goes through a non-cacheable
//     bounce pool below 4 GiB, identity-mapped, copied in at map time and back at unmap time when the controller may
//     have written;
//   * if the SMMU could not be attached (one line says why), contiguous buffers in reach go by physical address and
//     the rest is bounced.
// Map and unmap run in thread context (IODMACommand prepare/complete), never at interrupt level.
#pragma once
#include <IOKit/IOMapper.h>

// The bounce pool (8 MiB, physically contiguous, below 4 GiB) for the boot, taken at start-up while such a run is easy
// to find (taken at the first device attach it was not: vm_page_find_contiguous fails once memory is in use). USB
// thread; 0, or -1 (the caller logs it).
int switch_usb_dma_reserve(void);

// Creates (once) and starts the mapper attached to IOResources, on the reserved pool; nullptr on failure (logged).
// `reach` is the first physical address the controller cannot address (HCCPARAMS1.AC64: 64-bit or 32-bit DMA).
IOMapper *switch_usb_dma_mapper(uint64_t reach);
