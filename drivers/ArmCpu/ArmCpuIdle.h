// SPDX-License-Identifier: BSD-2-Clause-Patent
// What ArmCpu.cpp shares with ArmCpuIdle.cpp, C7's platform half (kits/SwitchIdle/switch_idle.h).
#pragma once
#include <stdint.h>

namespace armcpu {
bool nativeGicV2Smp(void);
bool psciSmcConduit(void);
int64_t psciSmc(uint64_t function, uint64_t a, uint64_t b, uint64_t c);
uintptr_t gicDistributor(void);
void fillBootRecord(void);
// On the core back from C7, interrupts masked.
void restoreBankedGic(void);
}  // namespace armcpu

// The all-cores observer, once every secondary runs: C7 on after its checks, or one line saying which failed. Thread
// context.
void armcpu_idle_start(void);
