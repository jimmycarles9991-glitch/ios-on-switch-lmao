// SPDX-License-Identifier: BSD-2-Clause-Patent
// Brightness and power of the Switch OLED's Samsung AMS699VC01 panel (hekate panel ID 0x2050) through MIPI DCS commands
// on DSI A, sent while the display controller scans out.
//
// The panel has no PWM backlight: brightness is DCS SET_DISPLAY_BRIGHTNESS (0x51) with a 2-byte big-endian value,
// 0 (off) .. 2047 (hekate di.h PANEL_SM_BL_CANDELA_MAX; di.c _display_dsi_backlight_brightness converts its 0..255 duty as
// duty * 2047 / 255). The panel is in video mode once hekate's display_init() has run, so a command packet rides the video
// stream: hekate display_dsi_vblank_write() (enable command packets in video on line 4, wait for a frame end, queue the
// packet, wait for the next frame end, reset the DSI syncpoint block, return to plain video). Every DSI write of that
// sequence is made at a fixed offset after a DC A frame end, in primary interrupt context (the frame-end filter or a
// timer callout), as hekate makes them; a command takes four frames (67 ms). TegraPanel.cpp has the steps and the timing.
//
// Frame ends come only from the display controller's interrupt (DC A, GIC SPI 73 = INTID 105), which the display driver
// owns (NativeLegacyDisplay's vblank source); its interrupt filter calls tegra_panel_frame_end_filter() for every
// FRAME_END; each command holds FRAME_END unmasked (native_legacy_frame_end_hold) until it is done. There is no polled
// mode: a command issued before the display reports frame ends, or whose hold leaves FRAME_END masked, or a frame end
// that does not come within 50 ms, panics with the DC interrupt registers and the GIC state.
//
// The functions below are for thread context only, never an interrupt filter or with a spinlock held. Commands are
// serialized internally. They return 0 on success and -1 if the display is not running (clocks gated, DSI off, not in
// video mode, or the DC stopped); everything else panics.
#ifndef TEGRA_PANEL_H
#define TEGRA_PANEL_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TEGRA_PANEL_BRIGHTNESS_MAX 2047

// Sets the panel brightness, 0..TEGRA_PANEL_BRIGHTNESS_MAX (clamped). The first call after start or wake also sends DCS
// 0x53 (brightness control + dimming, 0x28), as hekate's and L4T's panel init do. Level 0 turns the emission off but
// keeps the panel on.
int tegra_panel_set_brightness(unsigned level);

// Panel off, the switchroot L4T suspend sequence for this panel (panel-s-720p-7-0.dtsi nvidia,dsi-suspend-cmd), after
// hekate's dimming wait: brightness 0, 100 ms, DCS SET_DISPLAY_OFF (0x28), 40 ms, ENTER_SLEEP_MODE (0x10), 120 ms. The
// DC must still be scanning out (the commands ride its stream); stop it only afterwards. About 0.5 s.
int tegra_panel_power_off(void);

// Panel on after tegra_panel_power_off(), once the DC scans out again: one frame end (proves the DC restarted),
// EXIT_SLEEP_MODE (0x11), 180 ms (L4T nvidia,dsi-init-cmd), brightness control 0x53, SET_DISPLAY_ON (0x29), 20 ms, then
// the brightness: `level` if >= 0, else the last level set (or the boot chain's). The panel keeps its other registers in
// sleep mode (it is not reset), so L4T's level-2 register writes are not repeated. About 0.5 s.
int tegra_panel_power_on(int level);

// True: TegraPanel takes native_legacy_frame_end_hold for each command and nothing else of SwitchBrightness needs DC A
// frame ends, so the display may mask FRAME_END while it has no holder. Any context.
bool tegra_panel_holds_frame_end(void);

// The level the boot chain left, from DC_COM_SCRATCH_REGISTER_B where hekate keeps its 0..255 duty (di.h
// DC_DCS_BACKLIGHT_LEVEL), scaled to 0..TEGRA_PANEL_BRIGHTNESS_MAX; -1 if the display is not running or the register
// holds no plausible value.
int tegra_panel_boot_brightness(void);

// The panel level last sent, 0..TEGRA_PANEL_BRIGHTNESS_MAX, or -1 before the first; any context (SwitchALS's stats).
int tegra_panel_brightness(void);

// For the owner of the DC A interrupt: call from its primary interrupt filter at every FRAME_END (DC_CMD_INT_STATUS
// bit 1, which that owner enables, unmasks and clears), after clearing it. Returns at once unless a command is out, and
// never waits: the last step's settle delays run on a timer callout.
void tegra_panel_frame_end_filter(void);

#ifdef __cplusplus
}
#endif
#endif
