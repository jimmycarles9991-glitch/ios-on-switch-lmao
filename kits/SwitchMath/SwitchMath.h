// SPDX-License-Identifier: BSD-2-Clause-Patent
// Integer helpers every driver writes by hand: power-of-two alignment, rounding division, single-evaluation
// min/max/clamp, saturation, square root and bit masks. All inline, 64-bit (no truncating casts), usable from C and C++, kernel
// and userland.
#ifndef SWITCH_MATH_H
#define SWITCH_MATH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline bool switch_is_pow2(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;  // NOLINT(modernize-use-std-bit): a C header
}

// `pow2` must be a power of two.
static inline uint64_t switch_align_down(uint64_t value, uint64_t pow2)
{
    return value & ~(pow2 - 1);
}

static inline uint64_t switch_align_up(uint64_t value, uint64_t pow2)
{
    return (value + pow2 - 1) & ~(pow2 - 1);
}

static inline bool switch_is_aligned(uint64_t value, uint64_t pow2)
{
    return (value & (pow2 - 1)) == 0;
}

// Bytes from `address` up to the next `pow2` boundary (a whole `pow2` when it is on one).
static inline uint64_t switch_bytes_to_boundary(uint64_t address, uint64_t pow2)
{
    return pow2 - (address & (pow2 - 1));
}

static inline uint64_t switch_div_round_up(uint64_t value, uint64_t divisor)
{
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

// Rounds half away from zero, for either sign of either operand (Linux DIV_ROUND_CLOSEST on signed values).
static inline int64_t switch_div_round_closest(int64_t value, int64_t divisor)
{
    return ((value < 0) == (divisor < 0)) ? (value + divisor / 2) / divisor : (value - divisor / 2) / divisor;
}

static inline uint64_t switch_clamp_u64(uint64_t value, uint64_t low, uint64_t high)
{
    if (value < low)
        return low;
    return value > high ? high : value;
}

static inline int64_t switch_clamp_i64(int64_t value, int64_t low, int64_t high)
{
    if (value < low)
        return low;
    return value > high ? high : value;
}

static inline int8_t switch_sat_i8(int64_t value)
{
    return (int8_t)switch_clamp_i64(value, INT8_MIN, INT8_MAX);
}

static inline int16_t switch_sat_i16(int64_t value)
{
    return (int16_t)switch_clamp_i64(value, INT16_MIN, INT16_MAX);
}

static inline int32_t switch_sat_i32(int64_t value)
{
    return (int32_t)switch_clamp_i64(value, INT32_MIN, INT32_MAX);
}

// The largest r with r * r <= value, one result bit per round (no floating point: kernel code).
static inline uint32_t switch_isqrt_u64(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = 1ULL << 62;
    while (bit > value)
        bit >>= 2;
    while (bit) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

#ifdef __cplusplus
}
#endif

// Each argument evaluated once, in its own type (so no mixed signed/unsigned comparison is hidden by a cast).
#define SWITCH_MIN(a, b)                                               \
    __extension__({                                                    \
        const __typeof__(a) switch_min_a_ = (a);                       \
        const __typeof__(b) switch_min_b_ = (b);                       \
        switch_min_a_ < switch_min_b_ ? switch_min_a_ : switch_min_b_; \
    })
#define SWITCH_MAX(a, b)                                               \
    __extension__({                                                    \
        const __typeof__(a) switch_max_a_ = (a);                       \
        const __typeof__(b) switch_max_b_ = (b);                       \
        switch_max_a_ > switch_max_b_ ? switch_max_a_ : switch_max_b_; \
    })

// Bits hi..lo set, as a 64-bit constant (hi <= 63).
#define SWITCH_GENMASK(hi, lo) ((~0ull >> (63 - (hi))) & (~0ull << (lo)))

#endif
