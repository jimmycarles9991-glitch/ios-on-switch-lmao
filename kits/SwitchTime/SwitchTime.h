// SPDX-License-Identifier: BSD-2-Clause-Patent
// Mach absolute time in the units drivers think in, through XNU's own conversions (kernel only).
#ifndef SWITCH_TIME_H
#define SWITCH_TIME_H

#include <stdint.h>
#include <kern/clock.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct switch_time_ratio {
    uint64_t mult;
    uint32_t shift;
} switch_time_ratio;

typedef struct switch_time_scale {
    switch_time_ratio ticks_to_ns;
    switch_time_ratio ticks_to_us;
    switch_time_ratio ticks_to_ms;
    switch_time_ratio ns_to_ticks;
    switch_time_ratio us_to_ticks;
    switch_time_ratio ms_to_ticks;
} switch_time_scale;

static inline uint64_t switch_time_gcd(uint64_t a, uint64_t b)
{
    while (b) {
        const uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

static inline switch_time_ratio switch_time_ratio_make(uint64_t num, uint64_t den)
{
    const uint64_t g = switch_time_gcd(num, den);
    num /= g;
    den /= g;
    if (den == 1) {
        const switch_time_ratio exact = {num, 0};
        return exact;
    }
    uint32_t best_shift = 0;
    uint64_t best_mult = 0;
    for (uint32_t cand = 0; cand < 128; ++cand) {
        if (cand > 64 && (num >> (128 - cand)) != 0)
            break;
        const __uint128_t scaled = ((__uint128_t)num) << cand;
        if (scaled > (~(__uint128_t)0) - (den - 1))
            break;
        const __uint128_t m = (scaled + (den - 1)) / den;
        if (m > UINT64_MAX)
            break;
        best_shift = cand;
        best_mult = (uint64_t)m;
    }
    const switch_time_ratio ratio = {best_mult, best_shift};
    return ratio;
}

static inline uint64_t switch_time_apply(uint64_t x, switch_time_ratio r)
{
    return (uint64_t)((((__uint128_t)x) * r.mult) >> r.shift);
}

static inline switch_time_scale switch_time_scale_init(uint64_t freq)
{
    const switch_time_scale s = {
        switch_time_ratio_make(UINT64_C(1000000000), freq),
        switch_time_ratio_make(UINT64_C(1000000), freq),
        switch_time_ratio_make(UINT64_C(1000), freq),
        switch_time_ratio_make(freq, UINT64_C(1000000000)),
        switch_time_ratio_make(freq, UINT64_C(1000000)),
        switch_time_ratio_make(freq, UINT64_C(1000)),
    };
    return s;
}

static inline uint64_t switch_time_ticks_to_ns(const switch_time_scale *s, uint64_t ticks)
{
    return switch_time_apply(ticks, s->ticks_to_ns);
}

static inline uint64_t switch_time_ticks_to_us(const switch_time_scale *s, uint64_t ticks)
{
    return switch_time_apply(ticks, s->ticks_to_us);
}

static inline uint64_t switch_time_ticks_to_ms(const switch_time_scale *s, uint64_t ticks)
{
    return switch_time_apply(ticks, s->ticks_to_ms);
}

static inline uint64_t switch_time_ns_to_ticks(const switch_time_scale *s, uint64_t ns)
{
    return switch_time_apply(ns, s->ns_to_ticks);
}

static inline uint64_t switch_time_us_to_ticks(const switch_time_scale *s, uint64_t us)
{
    return switch_time_apply(us, s->us_to_ticks);
}

static inline uint64_t switch_time_ms_to_ticks(const switch_time_scale *s, uint64_t ms)
{
    return switch_time_apply(ms, s->ms_to_ticks);
}

#if defined(__aarch64__) && defined(KERNEL)
static inline const switch_time_scale *switch_time_kernel_scale(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
    static switch_time_scale cached;
    static uint32_t state;
    if (__builtin_expect(__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2u, 1))
        return &cached;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&state, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        uint64_t freq = 0;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        cached = switch_time_scale_init(freq ? freq : UINT64_C(19200000));
        __atomic_store_n(&state, 2u, __ATOMIC_RELEASE);
        return &cached;
    }
    while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2u)
        __asm__ volatile("yield");
    return &cached;
}
#endif

static inline uint64_t switch_abs_from_ns(uint64_t ns)
{
#if defined(__aarch64__) && defined(KERNEL)
    return switch_time_ns_to_ticks(switch_time_kernel_scale(), ns);
#else
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(ns, &abs);
    return abs;
#endif
}

static inline uint64_t switch_abs_from_us(uint64_t us)
{
#if defined(__aarch64__) && defined(KERNEL)
    return switch_time_us_to_ticks(switch_time_kernel_scale(), us);
#else
    return switch_abs_from_ns(us * NSEC_PER_USEC);
#endif
}

static inline uint64_t switch_abs_from_ms(uint64_t ms)
{
#if defined(__aarch64__) && defined(KERNEL)
    return switch_time_ms_to_ticks(switch_time_kernel_scale(), ms);
#else
    return switch_abs_from_ns(ms * NSEC_PER_MSEC);
#endif
}

static inline uint64_t switch_abs_to_ns(uint64_t abs)
{
#if defined(__aarch64__) && defined(KERNEL)
    return switch_time_ticks_to_ns(switch_time_kernel_scale(), abs);
#else
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(abs, &ns);
    return ns;
#endif
}

static inline uint64_t switch_abs_to_us(uint64_t abs)
{
#if defined(__aarch64__) && defined(KERNEL)
    return switch_time_ticks_to_us(switch_time_kernel_scale(), abs);
#else
    return switch_abs_to_ns(abs) / NSEC_PER_USEC;
#endif
}

static inline uint64_t switch_abs_to_ms(uint64_t abs)
{
#if defined(__aarch64__) && defined(KERNEL)
    return switch_time_ticks_to_ms(switch_time_kernel_scale(), abs);
#else
    return switch_abs_to_ns(abs) / NSEC_PER_MSEC;
#endif
}

static inline uint64_t switch_now_ns(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
    return switch_abs_to_ns(mach_absolute_time());
}

static inline uint64_t switch_now_us(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
    return switch_abs_to_us(mach_absolute_time());
}

static inline uint64_t switch_now_ms(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
    return switch_abs_to_ms(mach_absolute_time());
}

static inline uint64_t switch_ns_since(uint64_t start_abs)
{
    return switch_abs_to_ns(mach_absolute_time() - start_abs);
}

static inline uint64_t switch_us_since(uint64_t start_abs)
{
    return switch_abs_to_us(mach_absolute_time() - start_abs);
}

static inline uint64_t switch_ms_since(uint64_t start_abs)
{
    return switch_abs_to_ms(mach_absolute_time() - start_abs);
}

static inline uint64_t switch_deadline_us(uint32_t us)
{
#if defined(__aarch64__) && defined(KERNEL)
    return mach_absolute_time() + switch_abs_from_us(us);
#else
    uint64_t deadline = 0;
    clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
    return deadline;
#endif
}

static inline uint64_t switch_deadline_ms(uint32_t ms)
{
#if defined(__aarch64__) && defined(KERNEL)
    return mach_absolute_time() + switch_abs_from_ms(ms);
#else
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    return deadline;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
