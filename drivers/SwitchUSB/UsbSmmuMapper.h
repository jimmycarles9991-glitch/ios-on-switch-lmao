// SPDX-License-Identifier: BSD-2-Clause-Patent
// UsbSmmuMapper.cpp: the XUSB host's address space through the MC SMMU, for UsbDmaMapper.cpp.
// A buffer in one physical piece below 4 GiB is at its own address (switch_usb_smmu_fixed). A buffer whose physical
// pieces meet on page boundaries (scattered pages: what large I/O is, or one piece above 4 GiB) maps as one IOVA range
// over its own pages in the window; anything else (pieces that break inside a page, as mbuf chains do) is the caller's
// to bounce. Thread context (IODMACommand prepare and complete).
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <stddef.h>
#include <stdint.h>

// Tables, RAM below 4 GiB at its own address (the firmware image read only; it and the bounce pool must be there), the
// window, and the swgroup to translation. Once; false with the reason.
bool switch_usb_smmu_start(uint64_t firmwarePa, uint64_t firmwareBytes, uint64_t poolPa, uint64_t poolBytes, char *why,
                           size_t whySize);
// The prepared descriptor's [offset, offset + length) at one IOVA range (address: its first byte). False: not
// translatable here (not started, a piece breaking inside a page, misaligned, the window full).
bool switch_usb_smmu_map(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, bool writable,
                         uint64_t alignment, uint64_t *address);
// [pa, pa + length)'s own address once started, when it lies in RAM below 4 GiB; UINT64_MAX otherwise. Nothing to
// undo.
uint64_t switch_usb_smmu_fixed(uint64_t pa, uint64_t length);
bool switch_usb_smmu_owns(uint64_t address);
#if SWITCH_STATS_IO
// A window address's page table entry (0: none); for the development probe.
uint32_t switch_usb_smmu_entry(uint64_t address);
#endif
// Unmaps what switch_usb_smmu_map gave `memory` at `address`: the range it covered and whether the controller could write it.
bool switch_usb_smmu_unmap(const IOMemoryDescriptor *memory, uint64_t address, uint64_t *offset, uint64_t *length,
                           bool *writable);

// XusbHost.cpp: the falcon firmware image the controller reads by physical address (ILOAD_BASE).
bool switch_xusb_firmware_range(uint64_t *pa, uint64_t *bytes);

// IODMACommand's copy and transfer paths ask the mapper for the CPU's address of a device address: a window IOVA
// through the tables, anything else (RAM at its own address, the pool) as it is.
uint64_t switch_usb_smmu_to_physical(uint64_t address);
