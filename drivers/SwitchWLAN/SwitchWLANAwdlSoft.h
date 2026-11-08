// SPDX-License-Identifier: BSD-2-Clause-Patent
// Between SwitchWLAN.cpp (the radio) and SwitchWLANAwdlSoft.cpp (the soft AWDL backend's radio services).
#ifndef SWITCH_WLAN_AWDL_SOFT_H
#define SWITCH_WLAN_AWDL_SOFT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct swlan_awdl_caps;

// SwitchWLAN.cpp -> the soft radio. Work loop, gate held: a monitor-mode 802.11 frame; an event while AWDL is open
// (1: the soft radio took it: received action frames, action frame completions).
void switch_wlan_soft_frame(const uint8_t *frame, size_t length);
int switch_wlan_soft_event(uint32_t type, uint32_t status, const uint8_t addr[6], const uint8_t *data, size_t length);
// Thread context, once the radio is open: the queue and its worker (no-op after the first call).
void switch_wlan_soft_init(void);
// What the soft radio adds to the caps (action frames out; the debug trial's level, 0 in release builds).
void switch_wlan_soft_caps(struct swlan_awdl_caps *caps);
// A suffix for the AWDL stats line: what went out and came in (empty before the first frame).
int switch_wlan_soft_stat(char *buf, size_t size);

// The soft radio -> SwitchWLAN.cpp. Thread context only: one SET ioctl on the station's interface, waiting for the
// answer; 0, SWLAN_FWERR(code) when the firmware refused it, or a negative errno.
int switch_wlan_ioctl_set(uint32_t cmd, const void *buf, size_t length);
int switch_wlan_d11n(void);  // the firmware's chanspecs are d11n (WLC_GET_VERSION iotype 1)

#ifdef __cplusplus
}
#endif

#endif
