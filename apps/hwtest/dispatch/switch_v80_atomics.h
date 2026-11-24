/* The probe's own bookkeeping as ARMv8.0 exclusives, written out. Built for iOS 27 (an A12 target) the compiler emits
 * LSE atomics for C11 atomics, which the a64x loader rewrites into stubs; the code that judges libdispatch must not run
 * the code under suspicion. */
#ifndef SWITCH_V80_ATOMICS_H
#define SWITCH_V80_ATOMICS_H

#include <stdbool.h>
#include <stdint.h>

static inline uint64_t v80_load(const uint64_t *p)
{
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline void v80_store(uint64_t *p, uint64_t v)
{
    __asm__ volatile("stlr %0, [%1]" : : "r"(v), "r"(p) : "memory");
}

/* The new value. */
static inline uint64_t v80_add(uint64_t *p, uint64_t v)
{
    uint64_t sum;
    uint32_t failed;
    __asm__ volatile("1:\n\t"
                     "ldaxr %0, [%2]\n\t"
                     "add %0, %0, %3\n\t"
                     "stlxr %w1, %0, [%2]\n\t"
                     "cbnz %w1, 1b"
                     : "=&r"(sum), "=&r"(failed)
                     : "r"(p), "r"(v)
                     : "memory");
    return sum;
}

/* The old value. */
static inline uint64_t v80_swap(uint64_t *p, uint64_t v)
{
    uint64_t old;
    uint32_t failed;
    __asm__ volatile("1:\n\t"
                     "ldaxr %0, [%2]\n\t"
                     "stlxr %w1, %3, [%2]\n\t"
                     "cbnz %w1, 1b"
                     : "=&r"(old), "=&r"(failed)
                     : "r"(p), "r"(v)
                     : "memory");
    return old;
}

/* The old value. */
static inline uint64_t v80_or(uint64_t *p, uint64_t v)
{
    uint64_t old, next;
    uint32_t failed;
    __asm__ volatile("1:\n\t"
                     "ldaxr %0, [%3]\n\t"
                     "orr %1, %0, %4\n\t"
                     "stlxr %w2, %1, [%3]\n\t"
                     "cbnz %w2, 1b"
                     : "=&r"(old), "=&r"(next), "=&r"(failed)
                     : "r"(p), "r"(v)
                     : "memory");
    return old;
}

static inline bool v80_cas(uint64_t *p, uint64_t expected, uint64_t desired)
{
    uint64_t old;
    uint32_t failed;
    __asm__ volatile("1:\n\t"
                     "ldaxr %0, [%2]\n\t"
                     "cmp %0, %3\n\t"
                     "b.ne 2f\n\t"
                     "stlxr %w1, %4, [%2]\n\t"
                     "cbnz %w1, 1b\n\t"
                     "b 3f\n"
                     "2:\n\t"
                     "clrex\n"
                     "3:"
                     : "=&r"(old), "=&r"(failed)
                     : "r"(p), "r"(expected), "r"(desired)
                     : "memory", "cc");
    return old == expected;
}

#endif
