/* Code in the app's own image that an iOS 26+ deployment target compiles for an A12 (Apple clang's default there): an
 * LSE atomic add and half-precision arithmetic, neither of which the Cortex-A57 has. Built for iOS 17 the same source
 * is ARMv8.0 (an exclusive loop, conversions to single). a64x.self runs both and checks the results. */
#ifndef SWITCH_A64X_PROBE_H
#define SWITCH_A64X_PROBE_H

#include <stdint.h>

void switch_a64x_hammer(uint64_t *counter, uint64_t times);

/* a * b, rounded to half precision, plus c, rounded again: two FP16 operations with no fused form. */
float switch_a64x_fp16_mul_add(float a, float b, float c);

#endif
