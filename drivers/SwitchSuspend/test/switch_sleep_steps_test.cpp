// The sleep steps' codes (drivers/SwitchSuspend/SwitchSleepSteps.h) as the durable mark and the next boot's log read
// them: each code its own name, in the power trail's phases, and no name for a code that is not a step.
#include <gtest/gtest.h>

#include <set>
#include <string>

#include "drivers/SwitchSuspend/SwitchSleepSteps.h"

namespace {

// The phases are separate enums; their codes compare as plain numbers.
constexpr unsigned code(unsigned step)
{
    return step;
}

TEST(SwitchSleepSteps, EachStepHasItsOwnName)
{
    const struct {
        unsigned step;
        const char *name;
    } steps[] = {
        {SWITCH_STEP_SLEEP_WATCHDOG, "sleep watchdog armed"},
        {SWITCH_STEP_DISPLAY_QUIESCED, "display quiesced"},
        {SWITCH_STEP_DISPLAY_CLOCKS_OFF, "display clocks off"},
        {SWITCH_STEP_GPU_GATE, "gpu gate"},
        {SWITCH_STEP_GPU_DOWN, "gpu down"},
        {SWITCH_STEP_SLEEP, "sleep"},
        {SWITCH_STEP_SLEEP_RTC, "sleep rtc"},
        {SWITCH_STEP_SLEEP_BATTERY, "sleep battery"},
        {SWITCH_STEP_SLEEP_BPMP, "sleep bpmp"},
        {SWITCH_STEP_SLEEP_JOURNAL, "sleep journal"},
        {SWITCH_STEP_SLEEP_SD, "sleep sd"},
        {SWITCH_STEP_SLEEP_I2C, "sleep i2c"},
        {SWITCH_STEP_HALT_CPU1, "halt cpu1"},
        {SWITCH_STEP_HALT_BOOT_CPU, "halt boot cpu"},
        {SWITCH_STEP_SYSTEM_SUSPEND, "SYSTEM_SUSPEND"},
        {SWITCH_STEP_RESUME, "resume"},
        {SWITCH_STEP_RESUME_ACTIVE, "resume active"},
        {SWITCH_STEP_WAKE_I2C, "wake i2c"},
        {SWITCH_STEP_WAKE_SD, "wake sd"},
        {SWITCH_STEP_WAKE_BATTERY, "wake battery"},
        {SWITCH_STEP_WAKE_ALS, "wake als"},
        {SWITCH_STEP_AWAKE, "awake"},
    };

    for (const auto &s : steps) {
        ASSERT_NE(switch_sleep_step_name(s.step), nullptr) << s.name;
        EXPECT_STREQ(switch_sleep_step_name(s.step), s.name);
    }
}

TEST(SwitchSleepSteps, PhasesMatchThePowerTrail)
{
    EXPECT_EQ(code(SWITCH_STEP_SLEEP_RTC), 0x2fu);
    EXPECT_EQ(code(SWITCH_STEP_SLEEP_BATTERY), 0x30u);
    EXPECT_LE(code(SWITCH_STEP_SYSTEM_SUSPEND), 0x3fu);
    EXPECT_EQ(code(SWITCH_STEP_RESUME), 0x40u);
    EXPECT_LE(code(SWITCH_STEP_WAKE_SD), 0x4fu);
    EXPECT_EQ(code(SWITCH_STEP_WAKE_BATTERY), 0x50u);
    EXPECT_LT(code(SWITCH_STEP_WAKE_ALS), code(SWITCH_STEP_AWAKE));
    EXPECT_EQ(code(SWITCH_STEP_AWAKE), 0x5fu);
}

TEST(SwitchSleepSteps, NamesAreUniqueAndOnlyForSteps)
{
    std::set<std::string> seen;
    unsigned named = 0;
    for (unsigned value = 0; value < 0x100; ++value) {
        const char *name = switch_sleep_step_name(value);
        if (!name)
            continue;
        ++named;
        EXPECT_TRUE(seen.insert(name).second) << name;
        EXPECT_TRUE((value >= 0x01u && value <= 0x0fu) || (value >= 0x20u && value <= 0x5fu)) << value;
    }
    const unsigned steps = (code(SWITCH_STEP_GPU_DOWN) - code(SWITCH_STEP_SLEEP_WATCHDOG) + 1) +
                           (code(SWITCH_STEP_SLEEP_RTC) - code(SWITCH_STEP_SLEEP) + 1) +
                           (code(SWITCH_STEP_SYSTEM_SUSPEND) - code(SWITCH_STEP_SLEEP_BATTERY) + 1) +
                           (code(SWITCH_STEP_WAKE_SD) - code(SWITCH_STEP_RESUME) + 1) +
                           (code(SWITCH_STEP_WAKE_ALS) - code(SWITCH_STEP_WAKE_BATTERY) + 1) + 1;
    EXPECT_EQ(named, steps);
    EXPECT_EQ(switch_sleep_step_name(0x00), nullptr);
    EXPECT_EQ(switch_sleep_step_name(0x0c), nullptr);
    EXPECT_EQ(switch_sleep_step_name(0x3d), nullptr);
    EXPECT_EQ(switch_sleep_step_name(0x4a), nullptr);
    EXPECT_EQ(switch_sleep_step_name(0x5e), nullptr);
}

}  // namespace
