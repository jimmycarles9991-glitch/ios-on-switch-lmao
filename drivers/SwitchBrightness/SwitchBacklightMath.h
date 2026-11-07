// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchBacklight's panel model, header-only so the host check (test/als_math_test) runs this exact code.
//
// The panel's DCS 0x51 level (0..2047) is taken as linear in luminance, its maximum as kNitsMax: the OLED's luminance has
// not been measured, and 400 nits is CoreBrightness's own figure for an uncalibrated display (DisplayCreateWithID's
// fallback). The dimmest level ever sent is kFloorRaw (~3%), so the panel's minimum is kFloorRaw / 2047 of that,
// 12.5 nits, and that is what backlight-calibration-parameters tells CoreBrightness (lMinPanel), so its range and the
// panel's are the same.
//
// CoreBrightness writes the backlight two ways (21H461):
//   * "brightness", 0..65536 (IODisplayParameters.brightness max), from ___DisplaySetPhysicalBrightnessLegacyOnThread:
//     ceil(fraction * (max - min)) + min. Mapped onto kFloorRaw..2047 (toRaw).
//   * "brightness-nits", nits * 65536 in a CFNumber SInt32, from _SetBLDriverNitsValue, which
//     ___DisplaySetLogicalBrightnessInternal calls for every brightness change once the display has a luminance range
//     (backlight-calibration-parameters) and is of the new backlight architecture (new-backlight-architecture, which
//     SwitchBacklight publishes with the backlight-marketing-table that architecture requires): the user's slider and
//     auto-brightness alike, stepped by CoreBrightness's own fade timer. Mapped linearly onto the panel (nitsToRaw): 2047
//     at kNitsMax, never below kFloorRaw.
#ifndef SWITCH_BACKLIGHT_MATH_H
#define SWITCH_BACKLIGHT_MATH_H
#include <stdint.h>

namespace switch_backlight {
constexpr uint32_t kMax = 65536;      // IODisplayParameters.brightness max, as Apple's
constexpr unsigned kPanelMax = 2047;  // TEGRA_PANEL_BRIGHTNESS_MAX
constexpr unsigned kFloorRaw = 64;    // the dimmest panel value ever sent (~3% of full)
constexpr unsigned kSpanRaw = kPanelMax - kFloorRaw;

// Luminance, nits * 65536 (16.16 fixed point, as CoreBrightness's backlight-calibration-parameters and brightness-nits).
constexpr uint32_t kNitsMax = 400u << 16, kNitsMid = 140u << 16;
constexpr uint32_t kNitsMin = (uint32_t)(((uint64_t)kNitsMax * kFloorRaw + kPanelMax / 2) / kPanelMax);  // 12.5 nits

// backlight-marketing-table: the slider's luminance at evenly spaced slider positions, int32 nits * 65536 (21H461
// DisplayCreateWithID 0x1b62ae0b0..0x1b62ae158: CFDataGetLength / 4 entries, each scvtf / 65536 into display+0x48).
// __DisplaySliderToLogicalBrightnessInternal (0x1b62b29a0) interpolates slider * (count - 1) between neighbouring
// entries, then maps that from the product range onto the panel range (backlight-calibration-parameters, the same range
// here), so slider 0, 0.5 and 1 are 12.5, 140 and 400 nits, linear between. These are CoreBrightness's own three points
// for a display without a table (5, 140 and 400 nits at 0x1b632d644, which the legacy architecture takes) with the bottom
// raised to the panel's floor, lMinProduct, so the slider's bottom asks for no less than the panel shows.
constexpr int32_t kMarketingTable[] = {(int32_t)kNitsMin, (int32_t)kNitsMid, (int32_t)kNitsMax};

inline unsigned toRaw(uint32_t brightness)
{
    if (brightness > kMax)
        brightness = kMax;
    return kFloorRaw + (unsigned)(((uint64_t)brightness * kSpanRaw + kMax / 2) / kMax);
}

inline uint32_t toBrightness(unsigned raw)
{
    if (raw >= kPanelMax)
        return kMax;
    return raw <= kFloorRaw ? 0 : (uint32_t)((uint64_t)(raw - kFloorRaw) * kMax / kSpanRaw);
}

// A brightness-nits value (nits * 65536, signed as CoreBrightness sends it) as a panel level.
inline unsigned nitsToRaw(int32_t nits)
{
    if (nits <= 0)
        return kFloorRaw;
    const uint64_t raw = ((uint64_t)nits * kPanelMax + kNitsMax / 2) / kNitsMax;
    return raw < kFloorRaw ? kFloorRaw : raw > kPanelMax ? kPanelMax : (unsigned)raw;
}

// The same as a brightness (0..kMax) that toRaw() turns back into that level, for SwitchBacklight's ramp and
// IODisplayParameters.
inline uint32_t nitsToBrightness(int32_t nits)
{
    const unsigned raw = nitsToRaw(nits);
    if (raw <= kFloorRaw)
        return 0;
    if (raw >= kPanelMax)
        return kMax;
    // Round up so that toRaw() lands on raw exactly: toRaw rounds to nearest.
    return (uint32_t)(((uint64_t)(raw - kFloorRaw) * kMax + kSpanRaw - 1) / kSpanRaw);
}

// A panel level as nits * 65536 (the brightness-nits readback).
inline uint32_t rawToNits(unsigned raw)
{
    if (raw > kPanelMax)
        raw = kPanelMax;
    return (uint32_t)(((uint64_t)raw * kNitsMax + kPanelMax / 2) / kPanelMax);
}

// IODisplayParameters.BrightnessMilliNits {min, max, value}, as AppleARMBacklight publishes it (J120: max 600000) and
// thermalmonitord reads value and max as SInt32: the panel's luminance in thousandths of a nit.
constexpr uint32_t kMilliNitsMax = (uint32_t)(((uint64_t)kNitsMax * 1000 + 32768) >> 16);

inline uint32_t rawToMilliNits(unsigned raw)
{
    return (uint32_t)(((uint64_t)rawToNits(raw) * 1000 + 32768) >> 16);
}
}  // namespace switch_backlight
#endif
