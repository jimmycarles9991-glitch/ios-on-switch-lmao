// SPDX-License-Identifier: BSD-2-Clause-Patent
// UsbSmmuMapper.cpp: the XUSB host's IOVA window through the MC SMMU, for UsbDmaMapper.cpp.
// A buffer whose physical pieces meet on page boundaries (contiguous, or scattered pages: what large I/O is) maps as
// one IOVA range over its own pages; anything else (pieces that break inside a page, as mbuf chains do) is the caller's
// to bounce. Thread context (IODMACommand prepare and complete).
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <stddef.h>
#include <stdint.h>

// Tables, the identity maps (firmware image, bounce pool) and the swgroup to translation. Once; false with the reason.
bool switch_usb_smmu_start(uint64_t firmwarePa, uint64_t firmwareBytes, uint64_t poolPa, uint64_t poolBytes, char *why,
                           size_t whySize);
// The prepared descriptor's [offset, offset + length) at one IOVA range (address: its first byte). False: not
// translatable here (not started, a piece breaking inside a page, misaligned, the window full).
bool switch_usb_smmu_map(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, bool writable,
                         uint64_t alignment, uint64_t *address);
bool switch_usb_smmu_owns(uint64_t address);
// Unmaps what switch_usb_smmu_map gave `memory` at `address`: the range it covered and whether the controller could write it.
bool switch_usb_smmu_unmap(IOMemoryDescriptor *memory, uint64_t address, uint64_t *offset, uint64_t *length,
                           bool *writable);

// XusbHost.cpp: the falcon firmware image the controller reads by physical address (ILOAD_BASE).
bool switch_xusb_firmware_range(uint64_t *pa, uint64_t *bytes);

// IODMACommand's copy and transfer paths ask the mapper for the CPU's address of a device address: a window IOVA
// through the tables, anything else (the identity-mapped pool and firmware) as it is.
uint64_t switch_usb_smmu_to_physical(uint64_t address);
