// SPDX-License-Identifier: BSD-2-Clause-Patent
// Kernel-side register helpers: mapping a register page and the one bounded wait the project allows.
#ifndef SWITCH_MMIO_KERNEL_H
#define SWITCH_MMIO_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kern/clock.h>
#include "SwitchMMIO.h"

#ifdef __cplusplus
extern "C" {
#endif

// NOLINTNEXTLINE(readability-redundant-declaration): not every XNU header set this includes declares it
uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t size);

// Uncached (Device-nGnRE) and never unmapped: register pages live as long as the kernel.
static inline volatile uint8_t *switch_mmio_map(uint64_t pa, size_t size)
{
    return (volatile uint8_t *)ml_io_map((uintptr_t)pa, size);
}

// For hardware handshakes that raise no interrupt and finish in microseconds (PLL lock, partition power, reset done,
// FIFO flush); anything slower or with an interrupt waits on the interrupt. The deadline is taken once; the register
// is read once more after it passes, so a thread preempted past the deadline does not report a timeout the hardware
// never had.
static inline bool switch_mmio_wait32(const volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value,
                                      uint32_t timeout_us)
{
    if ((switch_mmio_read32(base, offset) & mask) == value)
        return true;
    uint64_t deadline = 0;
    clock_interval_to_deadline(timeout_us, NSEC_PER_USEC, &deadline);
    while (mach_absolute_time() < deadline) {
        if ((switch_mmio_read32(base, offset) & mask) == value)
            return true;
    }
    return (switch_mmio_read32(base, offset) & mask) == value;
}

#ifdef __cplusplus
}
#endif

#endif
