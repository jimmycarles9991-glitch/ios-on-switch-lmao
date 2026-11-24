/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SWITCH_WLAN_ROM_STATE_H
#define SWITCH_WLAN_ROM_STATE_H

#include <stdbool.h>
#include <stddef.h>

struct switch_wlan_rom_state {
    void *switch_pending;
    bool switch_mounted, switch_claimed;
};

// Sequential consistency prevents capture and mount from both missing the other's publication.
static inline int switch_wlan_rom_publish(struct switch_wlan_rom_state *state, void *pending)
{
    void *expected = NULL;
    return __atomic_compare_exchange_n(&state->switch_pending, &expected, pending, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

static inline void *switch_wlan_rom_take(struct switch_wlan_rom_state *state)
{
    if (!__atomic_load_n(&state->switch_mounted, __ATOMIC_SEQ_CST))
        return NULL;
    void *pending = __atomic_load_n(&state->switch_pending, __ATOMIC_SEQ_CST);
    bool expected = false;
    return pending && __atomic_compare_exchange_n(&state->switch_claimed, &expected, true, false, __ATOMIC_ACQ_REL,
                                                  __ATOMIC_ACQUIRE)
               ? pending
               : NULL;
}

#endif
