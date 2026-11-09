// SPDX-License-Identifier: BSD-2-Clause-Patent
// Data cache maintenance by VA for DMA masters that do not snoop the A57's caches (every Tegra engine on this SoC).
//
// clean: the device reads what the CPU wrote. clean_invalidate: before the device writes, so no dirty line is later
// evicted over its data. invalidate: after the device wrote, so the CPU sees its data rather than a line prefetched
// meanwhile; a partial first or last line is cleaned and invalidated instead (Linux dcache_inval_poc), so bytes that
// share a line with the range survive.
//
// The plain forms end with one DSB SY, which is what makes the maintenance visible to a device outside the inner
// shareable domain; issue it before the doorbell. The *_lines forms leave it out so a batch pays for one
// switch_dcache_sync(). No barrier is needed before the first DC: DC by VA is ordered after earlier stores to the
// same line.
//
// clean and clean_invalidate also run at EL0 (SCTLR_EL1.UCI); invalidate and the physical forms are kernel-only.
#ifndef SWITCH_DCACHE_H
#define SWITCH_DCACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CTR_EL0.DminLine on the Cortex-A57, a constant because EL0 cannot read CTR_EL0 (XNU leaves SCTLR_EL1.UCT clear).
#define SWITCH_DCACHE_LINE 64u

#ifndef SWITCH_DCACHE_OP
#define SWITCH_DCACHE_OP(op, address) __asm__ volatile("dc " op ", %0" ::"r"(address) : "memory")
#endif
#ifndef SWITCH_DCACHE_BARRIER
#define SWITCH_DCACHE_BARRIER() __asm__ volatile("dsb sy" ::: "memory")
#endif

typedef enum switch_dcache_op {
    SWITCH_DCACHE_CLEAN,
    SWITCH_DCACHE_CLEAN_INVALIDATE,
    SWITCH_DCACHE_INVALIDATE,
} switch_dcache_op;

typedef struct switch_dma_span {
    uint64_t va;
    uint64_t length;
} switch_dma_span;

static inline void switch_dcache_sync(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
    SWITCH_DCACHE_BARRIER();
}

static inline uintptr_t switch_dcache_line_of(uintptr_t address)
{
    return address & ~(uintptr_t)(SWITCH_DCACHE_LINE - 1);
}

// One DC per line over [from, to), both line-aligned, eight to a turn. DC takes no offset, so a line still costs a DC
// and an add; the unrolling takes the loop's compare and branch down from one per line to one per eight.
#define SWITCH_DCACHE_RUN(op, from, to)                                         \
    do {                                                                        \
        const uintptr_t run_line_ = SWITCH_DCACHE_LINE;                         \
        uintptr_t run_at_ = (from);                                             \
        const uintptr_t run_end_ = (to);                                        \
        for (; run_end_ - run_at_ >= 8 * run_line_; run_at_ += 8 * run_line_) { \
            SWITCH_DCACHE_OP(op, run_at_);                                      \
            SWITCH_DCACHE_OP(op, run_at_ + run_line_);                          \
            SWITCH_DCACHE_OP(op, run_at_ + 2 * run_line_);                      \
            SWITCH_DCACHE_OP(op, run_at_ + 3 * run_line_);                      \
            SWITCH_DCACHE_OP(op, run_at_ + 4 * run_line_);                      \
            SWITCH_DCACHE_OP(op, run_at_ + 5 * run_line_);                      \
            SWITCH_DCACHE_OP(op, run_at_ + 6 * run_line_);                      \
            SWITCH_DCACHE_OP(op, run_at_ + 7 * run_line_);                      \
        }                                                                       \
        for (; run_at_ < run_end_; run_at_ += run_line_)                        \
            SWITCH_DCACHE_OP(op, run_at_);                                      \
    } while (0)

static inline void switch_dcache_clean_lines(const volatile void *va, size_t length)
{
    if (!length)
        return;
    const uintptr_t end = (uintptr_t)va + length;
    SWITCH_DCACHE_RUN("cvac", switch_dcache_line_of((uintptr_t)va),
                      switch_dcache_line_of(end - 1) + SWITCH_DCACHE_LINE);
}

static inline void switch_dcache_clean_invalidate_lines(const volatile void *va, size_t length)
{
    if (!length)
        return;
    const uintptr_t end = (uintptr_t)va + length;
    SWITCH_DCACHE_RUN("civac", switch_dcache_line_of((uintptr_t)va),
                      switch_dcache_line_of(end - 1) + SWITCH_DCACHE_LINE);
}

static inline void switch_dcache_invalidate_lines(const volatile void *va, size_t length)
{
    if (!length)
        return;
    uintptr_t at = (uintptr_t)va;
    uintptr_t end = at + length;
    if (at != switch_dcache_line_of(at)) {
        at = switch_dcache_line_of(at);
        SWITCH_DCACHE_OP("civac", at);
        at += SWITCH_DCACHE_LINE;
    }
    if (end != switch_dcache_line_of(end) && switch_dcache_line_of(end) >= at) {
        end = switch_dcache_line_of(end);
        SWITCH_DCACHE_OP("civac", end);
    }
    if (at < end)
        SWITCH_DCACHE_RUN("ivac", at, end);
}

static inline void switch_dcache_lines(switch_dcache_op op, const volatile void *va, size_t length)
{
    switch (op) {
    case SWITCH_DCACHE_CLEAN:
        switch_dcache_clean_lines(va, length);
        break;
    case SWITCH_DCACHE_CLEAN_INVALIDATE:
        switch_dcache_clean_invalidate_lines(va, length);
        break;
    case SWITCH_DCACHE_INVALIDATE:
        switch_dcache_invalidate_lines(va, length);
        break;
    }
}

static inline void switch_dcache_clean(const volatile void *va, size_t length)
{
    switch_dcache_clean_lines(va, length);
    switch_dcache_sync();
}

static inline void switch_dcache_clean_invalidate(const volatile void *va, size_t length)
{
    switch_dcache_clean_invalidate_lines(va, length);
    switch_dcache_sync();
}

static inline void switch_dcache_invalidate(const volatile void *va, size_t length)
{
    switch_dcache_invalidate_lines(va, length);
    switch_dcache_sync();
}

static inline void switch_dcache_spans(switch_dcache_op op, const switch_dma_span *spans, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        switch_dcache_lines(op, (const volatile void *)(uintptr_t)spans[i].va, (size_t)spans[i].length);
    switch_dcache_sync();
}

#if defined(KERNEL) || defined(SWITCH_DCACHE_PHYS_MODEL)
#include "SwitchHwPage.h"
#ifndef KERNEL
bool switch_dma_pa_managed(uint64_t pa);
#endif

// XNU's physical aperture (osfmk/arm64/arm_vm_init.c): linear only within the chunk phystokv_range() reports, and
// only over managed RAM (it panics on anything else).
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
uint64_t phystokv_range(uint64_t pa, size_t *max_length);

static inline void switch_dcache_phys_lines(switch_dcache_op op, uint64_t pa, uint64_t length)
{
    while (length) {
        size_t chunk = (size_t)length;
        const uint64_t va = phystokv_range(pa, &chunk);
        switch_dcache_lines(op, (const volatile void *)(uintptr_t)va, chunk);
        pa += chunk;
        length -= chunk;
    }
}

// An engine's page list: each hardware page of managed RAM through the aperture; a page outside it (device memory a
// task mapped) has no cacheable alias to maintain. A page's offset bits are ignored.
static inline void switch_dcache_hw_pages(switch_dcache_op op, const uint64_t *pages, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (switch_dma_pa_managed(pages[i]))
            switch_dcache_phys_lines(op, pages[i] & ~SWITCH_HW_PAGE_MASK, SWITCH_HW_PAGE_SIZE);
    }
}
#endif

#ifdef __cplusplus
}
#endif

#endif
