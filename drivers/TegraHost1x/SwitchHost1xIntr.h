// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraHost1x.cpp's calls into the fence lists (SwitchHost1xFence.cpp), in place of mainline's host1x_intr_*.
#pragma once
#include <stdint.h>

class IOWorkLoop;

// The lists' lock, the hand-off source on host1x's work loop and the bound timer; false: one could not be made.
bool switch_host1x_intr_init(IOWorkLoop *loop, volatile uint8_t *sync, uint32_t intid);
// The sync unit is up (boot, or SC7 resume with the syncpoint values restored): fences may be added, and every pending
// fence's syncpoint is armed again. Before it and after stop, adds and waits fail with TEGRA_HOST1X_ENODEV.
void switch_host1x_intr_start();
void switch_host1x_intr_stop();
// The syncpoint interrupt's action, on the work loop.
void switch_host1x_intr_handle();
// The syncpoint interrupt's primary filter: the filter waiters whose thresholds passed. Reads syncpoint values only;
// the action acks and re-arms.
void switch_host1x_intr_filter();
