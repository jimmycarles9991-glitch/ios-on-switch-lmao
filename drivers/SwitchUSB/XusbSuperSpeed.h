// SPDX-License-Identifier: GPL-2.0-only
// XusbSuperSpeed.cpp: SS port 0's lane, from the role thread, PCIe's PLL calibration and the falcon mailbox.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The pad controller's mapping and the lane lock; false when either cannot be had. Once, before the others.
bool switch_usb_ss_init(void);
// Role thread: the lane up (defaults, mux out of IDDQ, port map, equalisation, clamps released) only while the UPHY
// PLL is announced ready and locked; down (clamped, VCORE down). Returns whether the lane is up now.
bool switch_usb_ss_lane(bool up);
// Whether the lane is up (any thread).
bool switch_usb_ss_up(void);
// PCIe is about to reset the UPHY and recalibrate its PLL (SwitchUPHY.h), on its thread: the lane clamped first.
void switch_usb_ss_clamp(void);
// SC7 took the pad controller: the lane is down without a write.
void switch_usb_ss_lost(void);
// LFPS detection on the lane, as the falcon asks (DISABLE/ENABLE_SS_LFPS_DETECTION). Mailbox work loop.
void switch_usb_ss_lfps(bool enable);
// One clause for the role line: lane state, pad mux, port map, clamps, PLL lock and the UPHY announcement.
void switch_usb_ss_describe(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif
