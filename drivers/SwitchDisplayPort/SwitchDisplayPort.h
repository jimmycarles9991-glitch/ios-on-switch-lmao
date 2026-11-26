// SPDX-License-Identifier: GPL-2.0-only
// SwitchDisplayPort's entry from the USB-C side: the BM92T36 alternate-mode engine (SwitchPDAltMode.cpp) reports the
// DisplayPort lanes its Configure gave the partner, or 0 when DisplayPort ended (detach, exit, failure).
#ifndef SWITCH_DISPLAY_PORT_H
#define SWITCH_DISPLAY_PORT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The PD work loop publishes a new attachment without blocking the controller. `lanes` is 2 (pin assignment D) or 4 (C); `flipped` is the plug's orientation (CC2), for the log.
int switch_displayport_start(void);
void switch_displayport_alt_mode(unsigned lanes, bool flipped, bool dock);
void switch_displayport_suspend(void);
void switch_displayport_resume(void);
bool switch_displayport_audio_prepare_presence(void);
bool switch_displayport_audio_format(uint32_t rate, uint32_t channels, uint32_t sample_bits);
void switch_displayport_audio_enable(bool enable);
void switch_displayport_audio_presence(const uint8_t *eld, uint32_t bytes, bool present);
bool switch_displayport_gpu_complete(void *display, int status);

#ifdef __cplusplus
}
#endif
#endif
