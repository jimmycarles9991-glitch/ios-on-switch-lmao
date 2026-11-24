// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_A3_PROBE_H
#define SWITCH_WLAN_A3_PROBE_H
#if DEVELOPMENT || DEBUG
#include <stddef.h>
#include <stdint.h>
void switch_wlan_a3_probe(const uint8_t mac[6], int d11n);
void switch_wlan_a3_probe_rx(const uint8_t *data, size_t length);
#endif
#endif
