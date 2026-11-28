// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fan curve level 0<->1 hysteresis, TMP451 limit cache, and lock/atomic source guards for SwitchThermal/SwitchSensors.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "../SwitchThermalCurve.h"
#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchThermalFan, Level1HysteresisPrevents36CFlappingOnBothProfiles)
{
    using namespace switch_thermal;
    for (const Profile &p : kProfiles) {
        EXPECT_EQ(p.trip[1], 36000) << p.name;
        EXPECT_EQ(p.hysteresis[1], 2000) << p.name;
        EXPECT_EQ(p.pwm[0], 0u) << p.name;
        EXPECT_EQ(p.pwm[1], 0u) << p.name;

        unsigned level = 0;
        level = stepFanLevel(p, level, 35999);
        EXPECT_EQ(level, 0u) << p.name;
        level = stepFanLevel(p, level, 36000);
        EXPECT_EQ(level, 1u) << p.name;

        // Oscillating right below 36 C must hold level 1 instead of flapping 1 -> 0 -> 1.
        for (const int t : {35999, 35500, 34500, 34000}) {
            level = stepFanLevel(p, level, t);
            EXPECT_EQ(level, 1u) << p.name << " at " << t;
        }
        level = stepFanLevel(p, level, 33999);
        EXPECT_EQ(level, 0u) << p.name;

        // Cooling from level 2 (drops below 36.1 C) lands cleanly in level 1 without falling straight to 0.
        level = stepFanLevel(p, 0, 40000);
        EXPECT_EQ(level, 2u) << p.name;
        level = stepFanLevel(p, level, 36099);
        EXPECT_EQ(level, 1u) << p.name;
    }
}

TEST(SwitchThermalFan, LimitCacheSkipsUnchangedWritesAndInvalidates)
{
    switch_thermal::LimitCache cache;
    EXPECT_FALSE(cache.matches(50, 30, 45, 25));
    cache.update(50, 30, 45, 25);
    EXPECT_TRUE(cache.matches(50, 30, 45, 25));
    EXPECT_FALSE(cache.matches(51, 30, 45, 25));
    EXPECT_FALSE(cache.matches(50, 29, 45, 25));
    EXPECT_FALSE(cache.matches(50, 30, 46, 25));
    EXPECT_FALSE(cache.matches(50, 30, 45, 24));
    cache.invalidate();
    EXPECT_FALSE(cache.matches(50, 30, 45, 25));
}

TEST(SwitchThermalSource, SkipsUnchangedLimitsAndGatesFanLogs)
{
    const std::string thermal = SourceText("../SwitchThermal.cpp");
    const std::string arm = Body(thermal, "bool SwitchThermal::armAlert(");
    EXPECT_THAT(arm, HasSubstr("if (limits.matches(socHigh, socLow, pcbHigh, pcbLow))"));
    EXPECT_THAT(arm, HasSubstr("limits.update(socHigh, socLow, pcbHigh, pcbLow);"));

    const std::string eval = Body(thermal, "bool SwitchThermal::evaluate()");
    EXPECT_THAT(eval, HasSubstr("if (announced && target != drive && !reportedCritical)"));
    EXPECT_THAT(eval, HasSubstr("#if DEVELOPMENT || DEBUG"));

    const std::string sensors = SourceText("../SwitchSensors.cpp");
    const std::string note = Body(sensors, "void noteSource(");
    EXPECT_THAT(note, HasSubstr("__atomic_load_n(&gSourceDown, __ATOMIC_RELAXED)"));
    const std::string ans = Body(sensors, "OSObject *answer(");
    EXPECT_THAT(ans, HasSubstr("__atomic_load_n(&gServed, __ATOMIC_RELAXED)"));
}
