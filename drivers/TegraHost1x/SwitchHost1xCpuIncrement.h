// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// One CPU increment of syncpoint `id`, one this driver handed out that no engine increments (a HW Test's gate on debug
// kernels: TegraVIC's SwitchVICGate.h). -5: the read-back shows no step; it is not written again.
template <class Registers> int switch_host1x_cpu_increment(Registers &registers, uint32_t id, uint32_t allocated)
{
    if (id < 160 || id >= 192 || id >= allocated)
        return -22;
    const uint32_t before = registers.read(0xf80 + 4 * id);
    registers.write(0xf60 + 4 * (id / 32), 1u << (id % 32));
    registers.barrier();
    return registers.read(0xf80 + 4 * id) - before == 1 ? 0 : -5;
}
