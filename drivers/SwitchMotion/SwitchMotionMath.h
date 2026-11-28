// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fixed-point SwitchMotionFusion.h and SwitchMotionBias.h share: kexts build with -mgeneral-regs-only.
#pragma once
#include <stdint.h>

constexpr int kSwitchQ = 30;
constexpr int64_t kSwitchOne = int64_t{1} << kSwitchQ;

inline uint64_t switchIsqrt(uint64_t n)
{
    uint64_t root = 0;
    uint64_t bit = uint64_t{1} << 62;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}
