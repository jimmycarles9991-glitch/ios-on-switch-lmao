// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_WLAN_A3_H
#define SWITCH_WLAN_A3_H

#if DEVELOPMENT || DEBUG
#include <stddef.h>
#include <stdint.h>

uint8_t *switch_wlan_a3_build(const uint8_t *image, size_t length, size_t *outputLength, bool candidate);
uint32_t switch_wlan_a3_state_address(bool candidate);
#endif

#endif
