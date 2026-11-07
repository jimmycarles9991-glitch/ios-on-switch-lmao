// SPDX-License-Identifier: MIT
// The GM20B across system sleep (SC7), for drivers/SwitchSuspend. Both run on SwitchSuspend's sleep/wake thread with
// interrupts on and I2C up: suspend after the display's, resume after host1x's and before the display's (its GPU present
// needs the GPU). Neither returns before the GPU is fully off or fully back; a GPU that cannot get there panics with one
// line. Each step is one power-journal line ("tegra-gm20b: sleep ..." / "tegra-gm20b: wake ..."). Both are no-ops when
// the GPU is off (switch-no-gpu) or was never powered. Submissions and new channels made meanwhile wait for the resume.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void tegra_gm20b_suspend(void);
void tegra_gm20b_resume(void);

// The display's power, from SwitchBacklight's work loop: off once the panel is dark and the scanout stopped, on as the
// panel starts to wake. Never waits: the GPU's rail moves on its own thread call (TegraGM20BRail.h), down once the GPU
// is idle with the display off, up at once when the display comes on. Any thread context.
void tegra_gm20b_display_power(bool on);
// A wake source fired with the panel off: the GPU comes up once ahead of the panel's power-up, and goes back down once
// idle if the panel stays off. Never waits; any thread context.
void tegra_gm20b_display_expected(void);

#ifdef __cplusplus
}
#endif
