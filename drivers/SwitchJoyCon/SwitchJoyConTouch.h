// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Records the latest touchscreen contact time (ms, switch_now_ms()) so iPadOS 27 rail
// gamepads can disconnect once the user switches back to touch.
void switch_joycon_note_touch_ms(uint64_t now_ms);

#ifdef __cplusplus
}
#endif
