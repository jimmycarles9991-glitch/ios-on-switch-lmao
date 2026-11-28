// SPDX-License-Identifier: BSD-2-Clause-Patent
// The NVJPG engine's state that TegraNVJPG.cpp (bring-up, power, the idle gate, sleep) and TegraNVJPGFlight.cpp (a
// decode's start, end and cancellation) share. Each global is written by the side its comment in TegraNVJPG.cpp names,
// under the lock named there.
#pragma once
#include <IOKit/IOLocks.h>
#include <stdint.h>
#include "TegraNVJPG.h"
#include "../TegraHost1x/TegraHost1x.h"
#include "../../kits/SwitchEngineGate/SwitchEngineGateKernel.h"
#include "../../kits/SwitchEngineGate/SwitchEnginePower.h"
#include "../../kits/SwitchFalcon/SwitchFalconInterrupt.h"

namespace tegra_nvjpg_engine {
constexpr uint32_t kClass = TEGRA_HOST1X_CLASS_NVJPG;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data)
constexpr unsigned kClock = 195;        // TEGRA210_CLK_NVJPG: CAR bank Y bit 3, also its reset

extern const SwitchEnginePower kPower;
extern volatile uint8_t *gJpg;
extern char gLine[256];
extern SwitchFalconInterrupt gErrors;
extern IOLock *gJobLock, *gFlightLock;
extern SwitchEngineGate<SwitchEngineGateKernel> gGate;
extern uint32_t gSyncpt, gNext, gPicture, gPowerOns;
extern uint64_t gPoweredAtUs;
extern bool gUp, gWedged;
extern TegraNVJPGBuffer gSetup, gStatus;
#if TEGRA_NVJPG_STATS
extern uint64_t gWaitsUnlocked;
#endif
#if TEGRA_NVJPG_PROBES
extern uint64_t gCancellations;
#endif

bool powerUp(bool *wasOn);
bool bootFalcon();
int answer(uint32_t id, uint32_t from, const char **stage);
void engineOff();
// The falcon's error line (SwitchFalconInterrupt), on its thread call (TegraNVJPGFlight.cpp)
void falconFailed(const switch_falcon_error &);
}  // namespace tegra_nvjpg_engine
