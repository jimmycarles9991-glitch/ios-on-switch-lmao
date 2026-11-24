// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug-build readings of the stock firmware's heap and A-MSDU receive state (SwitchWLANChipProbe.cpp). Release builds
// get empty hooks.
#ifndef SWITCH_WLAN_PROBE_HOOKS_H
#define SWITCH_WLAN_PROBE_HOOKS_H

#include <stdint.h>

#if DEVELOPMENT || DEBUG
// Words of the chip's RAM, only while it is reachable and every address is inside it; takes the driver's gate.
typedef bool (*switch_wlan_probe_read)(const uint32_t *addresses, uint32_t *values, unsigned count);

void switch_wlan_probe_attached(switch_wlan_probe_read read);
// Thread context, never under the AWDL lock: the work loop takes that lock inside the gate.
void switch_wlan_probe_heap(const char *moment);
// Under the AWDL lock, as each activation commits.
void switch_wlan_probe_activated();
#else
static inline void switch_wlan_probe_heap(const char *)
{}

static inline void switch_wlan_probe_activated()
{}
#endif

#endif
