// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fan curve profiles, skin estimate, TMP451 alert window math and limit caching for SwitchThermal.cpp.
#ifndef SWITCH_THERMAL_CURVE_H
#define SWITCH_THERMAL_CURVE_H
#include <stdint.h>

namespace switch_thermal {
constexpr unsigned kLevels = 10;

struct Profile {
    const char *name;
    int32_t soc[2], pcb[2];  // skin estimate scale, offset
    int32_t trip[kLevels];   // m C
    int32_t hysteresis[kLevels];
    uint16_t pwm[kLevels];  // of 256
};

// Level 1 (trip 36 C, pwm 0) carries 2 C hysteresis so skin readings around 36 C do not flap 0 <-> 1 and wake
// ALERT repeatedly when fan PWM is unchanged; level 2 drops below 36.1 C cleanly into level 1.
constexpr Profile kProfiles[] = {
    {"Console",
     {7831, 57590},
     {8051, -45213},
     {0, 36000, 40000, 43000, 49000, 53000, 70000, 80000, 90000, 100000},
     {0, 2000, 3900, 2900, 0, 0, 0, 0, 0, 0},
     {0, 0, 51, 51, 128, 255, 255, 255, 255, 255}},
    {"Handheld",
     {9029, 4274},
     {7176, -33954},
     {0, 36000, 40000, 43000, 44000, 45000, 46500, 80000, 90000, 100000},
     {0, 2000, 3900, 2900, 0, 0, 1000, 0, 0, 0},
     {0, 0, 51, 51, 76, 128, 255, 255, 255, 255}},
};

inline int skin(const int32_t coefficient[2], int milliC)
{
    return static_cast<int>(
        (static_cast<int64_t>(milliC) * coefficient[0] + static_cast<int64_t>(coefficient[1]) * 1000) / 10000 + 500);
}

inline int sensorFor(const int32_t coefficient[2], int estimate)
{
    return static_cast<int>(
        ((static_cast<int64_t>(estimate) - 500) * 10000 - static_cast<int64_t>(coefficient[1]) * 1000) /
        coefficient[0]);
}

inline unsigned stepFanLevel(const Profile &profile, unsigned level, int estimate)
{
    while (level + 1 < kLevels && estimate >= profile.trip[level + 1])
        ++level;
    while (level > 0 && estimate < profile.trip[level] - profile.hysteresis[level])
        --level;
    return level;
}

inline void window(int milliC, int up, int down, uint8_t *high, uint8_t *low)
{
    int h = (up + 999) / 1000;
    int l = down / 1000;
    if (h < milliC / 1000 + 1)
        h = milliC / 1000 + 1;
    if (l > (milliC + 999) / 1000 - 1)
        l = (milliC + 999) / 1000 - 1;
    *high = static_cast<uint8_t>(h > 127 ? 127 : h);
    *low = static_cast<uint8_t>(l < 0 ? 0 : l);
}

struct LimitCache {
    bool valid = false;
    uint8_t socHigh = 0, socLow = 0, pcbHigh = 0, pcbLow = 0;

    void invalidate()
    {
        valid = false;
    }

    bool matches(uint8_t sh, uint8_t sl, uint8_t ph, uint8_t pl) const
    {
        return valid && socHigh == sh && socLow == sl && pcbHigh == ph && pcbLow == pl;
    }

    void update(uint8_t sh, uint8_t sl, uint8_t ph, uint8_t pl)
    {
        socHigh = sh;
        socLow = sl;
        pcbHigh = ph;
        pcbLow = pl;
        valid = true;
    }
};
}  // namespace switch_thermal
#endif
