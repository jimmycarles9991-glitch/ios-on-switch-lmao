/*
 * The bit packers of Mesa's src/util/bitpack_helpers.h (Mesa, MIT) that mme_bitpack_helpers.h uses, without the debug range
 * checks. r8-rawca: see util/macros.h.
 */
#ifndef SWITCH_METAL_MESA_UTIL_BITPACK_HELPERS_H
#define SWITCH_METAL_MESA_UTIL_BITPACK_HELPERS_H
#include <stdint.h>
#include "macros.h"
static inline uint64_t util_bitpack_ones(uint32_t start, uint32_t end)
{
    return (UINT64_MAX >> (64 - (end - start + 1))) << start;
}
static inline uint64_t util_bitpack_uint(uint64_t v, uint32_t start, UNUSED uint32_t end)
{
    return v << start;
}
static inline uint64_t util_bitpack_sint(int64_t v, uint32_t start, uint32_t end)
{
    const int bits = end - start + 1;
    return ((uint64_t)v & BITFIELD64_MASK(bits)) << start;
}
#endif
