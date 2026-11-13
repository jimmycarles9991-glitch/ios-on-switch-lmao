// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// The caller has stopped the sole hardware producer and published its failure before releasing any hardware waiter.
template <class Registers>
int switch_host1x_release_failed(Registers &registers, uint32_t id, uint32_t threshold, uint32_t maxIncrements,
                                 uint32_t allocated)
{
    if (id < 160 || id >= 192 || id >= allocated || !maxIncrements || maxIncrements > 2)
        return -22;
    const uint32_t current = registers.read(0xf80 + 4 * id);
    const int32_t distance = static_cast<int32_t>(threshold - current);
    if (distance <= 0)
        return 0;
    if (static_cast<uint32_t>(distance) > maxIncrements)
        return -22;
    const uint32_t offset = 0xf60 + 4 * (id / 32), bit = 1u << (id % 32);
    for (int32_t i = 0; i < distance; ++i)
        registers.write(offset, bit);
    registers.barrier();
    return static_cast<int32_t>(registers.read(0xf80 + 4 * id) - threshold) >= 0 ? 0 : -5;
}
