// SPDX-License-Identifier: BSD-2-Clause-Patent
// Mach absolute time in the units drivers think in, through XNU's own conversions (kernel only).
#ifndef SWITCH_TIME_H
#define SWITCH_TIME_H

#include <stdint.h>
#include <kern/clock.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t switch_abs_from_ns(uint64_t ns)
{
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(ns, &abs);
    return abs;
}

static inline uint64_t switch_abs_from_us(uint64_t us)
{
    return switch_abs_from_ns(us * NSEC_PER_USEC);
}

static inline uint64_t switch_abs_from_ms(uint64_t ms)
{
    return switch_abs_from_ns(ms * NSEC_PER_MSEC);
}

static inline uint64_t switch_abs_to_ns(uint64_t abs)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(abs, &ns);
    return ns;
}

static inline uint64_t switch_abs_to_us(uint64_t abs)
{
    return switch_abs_to_ns(abs) / NSEC_PER_USEC;
}

static inline uint64_t switch_abs_to_ms(uint64_t abs)
{
    return switch_abs_to_ns(abs) / NSEC_PER_MSEC;
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
    uint64_t deadline = 0;
    clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
    return deadline;
}

static inline uint64_t switch_deadline_ms(uint32_t ms)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    return deadline;
}

#ifdef __cplusplus
}
#endif

#endif
