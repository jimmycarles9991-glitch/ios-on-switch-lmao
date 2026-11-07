// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XUSB pad controller's UPHY PLL (UPHY_PLL_P0, fed by PLLE), which PCIe's lane 0 and SuperSpeed USB's lane 5 both
// run on. PCIe (drivers/SwitchWLAN TegraPcie::uphy) calibrates it, at boot and after every SC7, and announces each
// calibration: not ready before it resets the UPHY, ready once the PLL has locked. The lane user (drivers/SwitchUSB's
// SuperSpeed lane) listens. On "not ready" it clamps its lane before the call returns, so the PLL is never
// recalibrated under a live lane; on "ready" it may take its lane up again. SC7 takes the PLL with VDD_CORE: the lane
// user marks it lost at suspend (no listener call) and forgets its lane, and the owner's next calibration after the
// wake announces both steps again.
#ifndef SWITCH_UPHY_H
#define SWITCH_UPHY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*switch_uphy_listener)(void *context, bool ready);

// The one listener, set once before its lane can go up. Any thread.
void switch_uphy_listen(switch_uphy_listener listener, void *context);
// The PLL's owner, thread context. The listener runs on the caller's thread before this returns.
void switch_uphy_announce(bool ready);
// The last announcement. Lock-free, so a listener may read it under its own lane lock.
bool switch_uphy_ready(void);
// SC7 is about to take the PLL: not ready, without a listener call. Suspend path.
void switch_uphy_lost(void);

#ifdef __cplusplus
}
#endif

#endif
