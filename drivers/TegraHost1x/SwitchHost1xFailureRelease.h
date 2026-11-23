// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// The CPU's increments of syncpoint `id` up to `threshold`, a distance its caller checked. The caller has stopped the
// sole hardware producer and stored its failure before releasing any hardware waiter, so every increment is a
// publishing write. -5: the read-back shows the increments did not take.
template <class Registers> int switch_host1x_release_failed(Registers &registers, uint32_t id, uint32_t threshold)
{
    const int32_t distance = static_cast<int32_t>(threshold - registers.read(0xf80 + 4 * id));
    if (distance <= 0)
        return 0;
    const uint32_t offset = 0xf60 + 4 * (id / 32), bit = 1u << (id % 32);
    for (int32_t i = 0; i < distance; ++i)
        registers.publish(offset, bit);
    registers.barrier();
    return static_cast<int32_t>(registers.read(0xf80 + 4 * id) - threshold) >= 0 ? 0 : -5;
}
