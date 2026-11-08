// SPDX-License-Identifier: BSD-2-Clause-Patent
// BH1730FVC lux and gain steps for SwitchALS.cpp, header-only so the host check (test/) runs this exact code
// against a port of Linux's.
//
// Calibration: switchroot L4T tegra210-nx-sensor.dtsi bh1730fvc@29 (rohm,integration-cycle 38, rohm,lux-multiplier 3600)
// with the OLED's rohm,opt-win-coeff and rohm,gain-coeff from tegra210b01-fric.dts (unchanged in switchroot's newest OLED
// tree, linux-5.0.0-dev and lineage-20 tegra210b01-frig.dts), except row 0's ci: 30300 as hekate bdk/input/als.c's Aula
// table, not the DT's 7502 (the Icosa table's row-0 ci, copied). Every boundary of both tables is continuous (each row
// meets the next at its rc: als_math_test), and only 30300 makes row 0 meet row 1 at 231 (2697 vs 2691; 7502 jumps to
// 7964, 3x). It matters only in low-infrared light (DATA1 < 23% of DATA0, e.g. LED lighting), where 7502 reads high.
// Scale: at the IR ratios measured on the console (about 0.35, row 1) this gives 16.8 times the datasheet's
// bare-sensor lux, the optical window's loss; Nintendo's Icosa table (hekate's default) gives 16.7 there.
// Maths: CTCaer/switch-l4t-kernel-4.9
// drivers/iio/light/bh1730.c (GPL-2.0, Copyright (c) 2021 CTCaer) bh1730_get_lux and bh1730_adjust_gain, re-expressed:
//   * lux = (cv * DATA0 - ci * DATA1) * 100 ms * multiplier / (gain * integration time us * 1000), one division instead
//     of Linux's three (its first truncation cost up to 15% below 30 lux); (cv, ci) from the first optical-window row
//     whose rc exceeds 1000 * DATA1 / DATA0, none (0 lux) past the last. Never below Linux's value, at most 4 above.
//   * Gain: down when the larger channel passes the gain's fl, up when it falls under its cl, except that a step up is
//     taken only if the reading stays under the next gain's fl (the DT's 2x cl 1600 is 51200 at 64x, over its fl 50000,
//     which would flip between the two every sample).
// hekate's Aula multiplier (1410) is not used: its code never selects it, and the DT's 3600 is what L4T and Android use
// on this console. Nothing here corrects for the panel's own light reaching the sensor (measured: the readings did not
// follow the panel level, 324 to 807 raw, at the same ambient).
#ifndef SWITCH_ALS_MATH_H
#define SWITCH_ALS_MATH_H
#include <stdint.h>

namespace switch_als {
constexpr unsigned kCycles = 38;                     // rohm,integration-cycle; TIMING = 256 - cycles (datasheet 0xda)
constexpr unsigned kIntegrationUS = 2700 * kCycles;  // bh1730.c BH1730_ITIME_TO_US: 102.6 ms (+ 2 ms ADC)
constexpr unsigned kConversionUS = kIntegrationUS + 2000;

// SwitchALS takes a result every kFramesPerSample-th frame end of the 60 Hz panel, the first kFramesToFirstSample frame
// ends after its set-up (kSetUpUS, three register writes): each one a whole conversion, and never as far apart as
// CoreBrightness's first-sample timeout after display-on (AABC, 250 ms).
constexpr unsigned kFramesPerSample = 7, kFramesToFirstSample = kFramesPerSample + 1;
constexpr unsigned kFrameUS = 1000000 / 60, kSetUpUS = 1000, kFirstSampleTimeoutUS = 250000;
constexpr unsigned kSampleUS = kFramesPerSample * kFrameUS;
constexpr unsigned kFirstSampleUS = kFramesToFirstSample * kFrameUS;
constexpr unsigned kLuxMultiplier = 3600, kLuxMax = 100000;
constexpr unsigned kGains = 4;
constexpr unsigned kGainFactor[kGains] = {1, 2, 64, 128};  // GAIN register 0..3

struct GainLimits {
    uint32_t cl, fl;
};

constexpr GainLimits kGainLimits[kGains] = {{2500, 0}, {1600, 7800}, {10, 50000}, {0, 1000}};  // tegra210b01-fric.dts

struct Window {
    uint32_t rc, cv, ci;
};

constexpr Window kWindow[] = {{231, 9697, 30300}, {993, 3333, 2778}, {1478, 1621, 1053}, {7500, 81, 10}};

// A channel at the ADC's top (65535): the light is beyond this gain's range, and with DATA0 clipped the IR ratio is
// too high, so the window row and the lux under-read. At 1x there is no lower gain: lux() reports kLuxMax. At any
// other gain SwitchALS drops the sample while nextGain() steps down (a lamp read ch0 65535 ch1 12940 at 1x).
constexpr uint32_t kChannelMax = 0xffff;

inline bool saturated(uint32_t visible, uint32_t ir)
{
    return visible >= kChannelMax || ir >= kChannelMax;
}

inline uint32_t lux(uint32_t visible, uint32_t ir, unsigned gain)
{
    if (gain >= kGains)
        return 0;
    if (saturated(visible, ir))
        return gain == 0 ? kLuxMax : 0;
    if (!visible)
        return 0;
    int64_t weighted = 0;
    for (const Window &w : kWindow) {
        if (1000ULL * ir / visible < w.rc) {
            weighted = static_cast<int64_t>(w.cv) * visible - static_cast<int64_t>(w.ci) * ir;
            break;
        }
    }
    if (weighted <= 0)
        return 0;  // cannot happen inside a row's range; bh1730.c would wrap in u64
    const uint64_t value = static_cast<uint64_t>(weighted) * 100 * kLuxMultiplier /
                           (static_cast<uint64_t>(kGainFactor[gain]) * kIntegrationUS * 1000);
    return value > kLuxMax ? kLuxMax : static_cast<uint32_t>(value);
}

// The gain for the next conversion after one at `gain` read these channels.
inline unsigned nextGain(unsigned gain, uint32_t visible, uint32_t ir)
{
    const uint32_t highest = visible > ir ? visible : ir;
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): SwitchALS passes only a gain this returned, under kGains
    if (gain > 0 && highest > kGainLimits[gain].fl)
        return gain - 1;
    if (gain + 1 < kGains && highest < kGainLimits[gain].cl &&
        static_cast<uint64_t>(highest) * kGainFactor[gain + 1] / kGainFactor[gain] < kGainLimits[gain + 1].fl) {
        return gain + 1;
    }
    return gain;
}
}  // namespace switch_als
#endif
