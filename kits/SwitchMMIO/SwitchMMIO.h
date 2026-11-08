// SPDX-License-Identifier: BSD-2-Clause-Patent
// 32-bit device register access for Device-nGnRE mappings (ml_io_map, IOMemoryMap with kIOMapInhibitCache).
//
// Reads are load-acquire. Cortex-A57 erratum 832075 (r0p0-r1p2; Tegra X1's A57 is r1p1): a Device load mixed with
// exclusives to Write-Back memory can deadlock the core, and with no LSE on ARMv8.0 every XNU lock and atomic is an
// LDXR/STXR pair. Linux's workaround, on in switchroot's kernel, is to make every device load an LDAR. Against an APB
// read of hundreds of cycles it costs a few.
//
// Writes are posted and ordered only against other accesses to the same device. switch_mmio_write32_publish first
// orders every earlier store to Normal memory (a descriptor or ring entry in non-cacheable or coherent memory) before
// the register write, for doorbells. Cacheable buffers a device does not snoop need switch_dcache_* (kits/SwitchDMA)
// instead; its DSB orders the doorbell as well.
#ifndef SWITCH_MMIO_H
#define SWITCH_MMIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SWITCH_MMIO_HOST_MODEL
uint32_t switch_mmio_model_read32(uintptr_t address);
void switch_mmio_model_write32(uintptr_t address, uint32_t value);
uint16_t switch_mmio_model_read16(uintptr_t address);
void switch_mmio_model_write16(uintptr_t address, uint16_t value);
uint8_t switch_mmio_model_read8(uintptr_t address);
void switch_mmio_model_write8(uintptr_t address, uint8_t value);
#endif

static inline uint32_t switch_mmio_read32(const volatile uint8_t *base, uint32_t offset)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    return switch_mmio_model_read32((uintptr_t)(base + offset));
#else
    // NOLINTNEXTLINE(misc-const-correctness): written by the asm below
    uint32_t value = 0;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
    return value;
#endif
}

static inline void switch_mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    switch_mmio_model_write32((uintptr_t)(base + offset), value);
#else
    *(volatile uint32_t *)(base + offset) = value;
#endif
}

// Narrow accesses for blocks that take them (PCIe BARs carry byte enables); the same ordering as the 32-bit ones.
static inline uint16_t switch_mmio_read16(const volatile uint8_t *base, uint32_t offset)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    return switch_mmio_model_read16((uintptr_t)(base + offset));
#else
    // NOLINTNEXTLINE(misc-const-correctness): written by the asm below
    uint32_t value = 0;
    __asm__ volatile("ldarh %w0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
    return (uint16_t)value;
#endif
}

static inline void switch_mmio_write16(volatile uint8_t *base, uint32_t offset, uint16_t value)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    switch_mmio_model_write16((uintptr_t)(base + offset), value);
#else
    *(volatile uint16_t *)(base + offset) = value;
#endif
}

static inline uint8_t switch_mmio_read8(const volatile uint8_t *base, uint32_t offset)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    return switch_mmio_model_read8((uintptr_t)(base + offset));
#else
    // NOLINTNEXTLINE(misc-const-correctness): written by the asm below
    uint32_t value = 0;
    __asm__ volatile("ldarb %w0, [%1]" : "=r"(value) : "r"(base + offset) : "memory");
    return (uint8_t)value;
#endif
}

static inline void switch_mmio_write8(volatile uint8_t *base, uint32_t offset, uint8_t value)
{
#ifdef SWITCH_MMIO_HOST_MODEL
    switch_mmio_model_write8((uintptr_t)(base + offset), value);
#else
    *(base + offset) = value;
#endif
}

static inline void switch_mmio_write32_publish(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
#ifndef SWITCH_MMIO_HOST_MODEL
    __asm__ volatile("dmb oshst" ::: "memory");
#endif
    switch_mmio_write32(base, offset, value);
}

// A published write that has reached its device on return (L4T's fence_udelay): the read-back of the same register
// cannot complete before it, so a delay that follows starts after the write took effect.
static inline void switch_mmio_write32_flush(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    switch_mmio_write32_publish(base, offset, value);
    (void)switch_mmio_read32(base, offset);
}

static inline void switch_mmio_rmw32(volatile uint8_t *base, uint32_t offset, uint32_t clear, uint32_t set)
{
    switch_mmio_write32(base, offset, (switch_mmio_read32(base, offset) & ~clear) | set);
}

#define SWITCH_BIT(n)                  (1u << (n))
#define SWITCH_FIELD_SHIFT(mask)       __builtin_ctzll((unsigned long long)(mask))
#define SWITCH_FIELD_GET(mask, value)  (((value) & (mask)) >> SWITCH_FIELD_SHIFT(mask))
#define SWITCH_FIELD_PREP(mask, value) (((value) << SWITCH_FIELD_SHIFT(mask)) & (mask))

#ifdef __cplusplus
}
#endif

#endif
