// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchUPHY.h"

namespace {
switch_uphy_listener gListener;
void *gContext;
bool gReady;
}  // namespace

extern "C" void switch_uphy_listen(switch_uphy_listener listener, void *context)
{
    __atomic_store_n(&gContext, context, __ATOMIC_RELAXED);
    __atomic_store_n(&gListener, listener, __ATOMIC_RELEASE);
}

// The flag goes first: a lane user that takes its lane lock after the listener let go sees "not ready".
extern "C" void switch_uphy_announce(bool ready)
{
    __atomic_store_n(&gReady, ready, __ATOMIC_SEQ_CST);
    const switch_uphy_listener listener = __atomic_load_n(&gListener, __ATOMIC_ACQUIRE);
    if (listener)
        listener(__atomic_load_n(&gContext, __ATOMIC_RELAXED), ready);
}

extern "C" bool switch_uphy_ready(void)
{
    return __atomic_load_n(&gReady, __ATOMIC_SEQ_CST);
}

extern "C" void switch_uphy_lost(void)
{
    __atomic_store_n(&gReady, false, __ATOMIC_SEQ_CST);
}
