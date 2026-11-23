/*
 * The sign-extension helpers of Mesa's src/util/u_math.h (Mesa, MIT) that the Fermi MME decoder and simulator use.
 * r8-rawca: see util/macros.h.
 */
#ifndef SWITCH_METAL_MESA_UTIL_U_MATH_H
#define SWITCH_METAL_MESA_UTIL_U_MATH_H
#include <assert.h>
#include <stdint.h>
#include "macros.h"
static inline int64_t util_mask_sign_extend(uint64_t val, unsigned width)
{
    assert(width > 0 && width <= 64);
    unsigned shift = 64 - width;
    return (int64_t)(val << shift) >> shift;
}
static inline int64_t util_sign_extend(uint64_t val, unsigned width)
{
    assert(width == 64 || val < (UINT64_C(1) << width));
    return util_mask_sign_extend(val, width);
}
#endif
