// kernel/switch/diag/heartbeat.h: the freeze heartbeat's timing and its TMR6/WDT4 words against the Tegra X1 TRM
// (8.6, 8.7, 8.9, 8.10), which timer block it takes, which class a stall is; the power trail holds it, its panics are
// watchdog panics, release has none of it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "bazel/testing/source_text.h"
#include "switch_heartbeat.h"

using testing::HasSubstr;

namespace {

constexpr uint64_t kSecond = 1000000000ull;
constexpr uint64_t kTick = SWITCH_HEARTBEAT_TICK_US * 1000ull;

TEST(SwitchHeartbeat, ItsInterruptComesAQuarterSecondEarlyAtWorstAndItsResetFourPeriodsIn)
{
    EXPECT_EQ(SWITCH_HEARTBEAT_STALL_MS, 12000u);
    EXPECT_EQ(SWITCH_HEARTBEAT_LATE_MS, 11750u);
    EXPECT_EQ(SWITCH_HEARTBEAT_RESET_MS, 48000u);
    EXPECT_EQ(SWITCH_HEARTBEAT_LATE_NS, SWITCH_HEARTBEAT_STALL_MS * 1000000ull - kTick);
    EXPECT_LE((SWITCH_HEARTBEAT_BEAT_S + SWITCH_HEARTBEAT_LEEWAY_S) * 1000u + 5000u, SWITCH_HEARTBEAT_LATE_MS);
}

TEST(SwitchHeartbeat, TheWordsAreTheTrmsForTmr6AndWdt4)
{
    EXPECT_EQ(SWITCH_HEARTBEAT_TMR6_PTV, 0x68u);
    EXPECT_EQ(SWITCH_HEARTBEAT_WDT4_CFG, 0x100u + 0x20u * 4);
    EXPECT_EQ(SWITCH_HEARTBEAT_WDT4_STS, 0x104u + 0x20u * 4);
    EXPECT_EQ(SWITCH_HEARTBEAT_WDT4_CMD, 0x108u + 0x20u * 4);
    EXPECT_EQ(SWITCH_HEARTBEAT_WDT4_UNLOCK, 0x10cu + 0x20u * 4);
    EXPECT_EQ(SWITCH_HEARTBEAT_SHARED_STATUS, 0x1a0u);
    EXPECT_EQ(switch_heartbeat_ptv_offset(1), 0x00u);
    EXPECT_EQ(switch_heartbeat_ptv_offset(5), 0x60u);
    EXPECT_EQ(switch_heartbeat_ptv_offset(6), SWITCH_HEARTBEAT_TMR6_PTV);
    EXPECT_EQ(switch_heartbeat_ptv_offset(9), 0x80u);
    EXPECT_EQ(switch_heartbeat_ptv_offset(0), 0x88u);
    EXPECT_EQ(switch_heartbeat_ptv_offset(10), UINT32_MAX);
    const uint32_t config = switch_heartbeat_wdt_config();
    EXPECT_EQ(config & 0xfu, 6u);
    EXPECT_EQ((config >> 4) & 0xffu, SWITCH_HEARTBEAT_PERIOD);
    EXPECT_NE(config & (1u << 12), 0u) << "InterruptEnable";
    EXPECT_EQ(config & (1u << 13), 0u) << "FIQEnable";
    EXPECT_EQ(config & (1u << 14), 0u) << "SystemResetEnable";
    EXPECT_NE(config & (1u << 15), 0u) << "Pmc2CarResetEn";
    EXPECT_EQ((config >> 16) & 0x1fu, 0u) << "CoreResetBitmapEn";
    const uint32_t ptv = switch_heartbeat_ptv();
    EXPECT_NE(ptv & (1u << 31), 0u);
    EXPECT_NE(ptv & (1u << 30), 0u);
    EXPECT_EQ((ptv & 0x1fffffffu) + 1u, SWITCH_HEARTBEAT_TICK_US);
}

TEST(SwitchHeartbeat, ItTakesOnlyAnIdleTimerBlockOrHekatesStoppedWatchdog)
{
    EXPECT_EQ(switch_heartbeat_hardware(0, 0, 0, 0), SWITCH_HEARTBEAT_HW_IDLE);
    EXPECT_EQ(switch_heartbeat_hardware(0x80000010u, 0, 0, 0), SWITCH_HEARTBEAT_HW_BUSY) << "TMR6 in use";
    EXPECT_EQ(switch_heartbeat_hardware(0x00000010u, 0, 0, 0), SWITCH_HEARTBEAT_HW_BUSY) << "TMR6 period left";
    EXPECT_EQ(switch_heartbeat_hardware(0, 0, 0, 1u << 2), SWITCH_HEARTBEAT_HW_BUSY) << "WDT2's interrupt pending";
    EXPECT_EQ(switch_heartbeat_hardware(0, SWITCH_HEARTBEAT_STS_ENABLED | SWITCH_HEARTBEAT_STS_INT, 0, 0),
              SWITCH_HEARTBEAT_HW_BUSY);
    // hekate's watchdog_end: TMR9 stopped, then START (which clears the unlock) and DISABLE, which therefore stays.
    EXPECT_EQ(switch_heartbeat_hardware(0, SWITCH_HEARTBEAT_STS_ENABLED, 0, 0), SWITCH_HEARTBEAT_HW_DORMANT);
    EXPECT_EQ(switch_heartbeat_hardware(0, SWITCH_HEARTBEAT_STS_ENABLED, 0xc0000063u, 0), SWITCH_HEARTBEAT_HW_BUSY);
}

TEST(SwitchHeartbeat, ARunningCoreThatTakesNoInterruptForTheLateTimeIsMasked)
{
    switch_heartbeat_core core = {};
    switch_heartbeat_sample(&core, true, 100, 1 * kSecond);
    switch_heartbeat_sample(&core, true, 100, 5 * kSecond);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 5 * kSecond), SWITCH_HEARTBEAT_CORE_ALIVE);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 1 * kSecond + SWITCH_HEARTBEAT_LATE_NS - 1),
              SWITCH_HEARTBEAT_CORE_ALIVE);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 1 * kSecond + SWITCH_HEARTBEAT_LATE_NS), SWITCH_HEARTBEAT_CORE_MASKED);
}

TEST(SwitchHeartbeat, AnInterruptOrAnIdleSpellStartsTheQuietTimeAgain)
{
    switch_heartbeat_core core = {};
    switch_heartbeat_sample(&core, true, 100, 0);
    switch_heartbeat_sample(&core, true, 101, 10 * kSecond);
    switch_heartbeat_sample(&core, true, 101, 20 * kSecond);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 20 * kSecond), SWITCH_HEARTBEAT_CORE_ALIVE);
    switch_heartbeat_sample(&core, false, 101, 30 * kSecond);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 60 * kSecond), SWITCH_HEARTBEAT_CORE_IDLE);
    switch_heartbeat_sample(&core, true, 101, 61 * kSecond);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 61 * kSecond), SWITCH_HEARTBEAT_CORE_ALIVE);
    switch_heartbeat_sample(&core, true, 101, 73 * kSecond);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 73 * kSecond), SWITCH_HEARTBEAT_CORE_MASKED);
}

// The beat thread samples with a later now than the interrupt that judges: no unsigned wrap reads as masked or late.
TEST(SwitchHeartbeat, ASampleOrABeatAfterNowIsNeitherMaskedNorLate)
{
    switch_heartbeat_core core = {};
    switch_heartbeat_sample(&core, true, 7, 30 * kSecond);
    EXPECT_EQ(switch_heartbeat_core_kind(&core, 29 * kSecond), SWITCH_HEARTBEAT_CORE_ALIVE);
    EXPECT_EQ(switch_heartbeat_class(&core, 1, 29 * kSecond, 30 * kSecond), SWITCH_HEARTBEAT_HEALTHY);
}

TEST(SwitchHeartbeat, AnyWdt4InterruptFindsTheBeatLate)
{
    switch_heartbeat_core cores[4] = {};
    const uint64_t beat = 100 * kSecond;
    for (unsigned cpu = 0; cpu < 4; ++cpu)
        switch_heartbeat_sample(&cores[cpu], cpu != 2, 1, beat);
    // START reloads the period on a running TMR6: the first expiry is 47 whole ticks and part of one later.
    for (uint64_t phase : {uint64_t{1}, kTick / 2, kTick}) {
        const uint64_t now = beat + (SWITCH_HEARTBEAT_PERIOD - 1) * kTick + phase;
        for (unsigned cpu = 0; cpu < 4; ++cpu)
            switch_heartbeat_sample(&cores[cpu], cpu != 2, 2 + cpu + now, now);
        EXPECT_EQ(switch_heartbeat_class(cores, 4, now, beat), SWITCH_HEARTBEAT_UNSCHEDULED) << phase;
    }
    EXPECT_EQ(switch_heartbeat_class(cores, 4, beat + SWITCH_HEARTBEAT_LATE_NS - 1, beat), SWITCH_HEARTBEAT_HEALTHY);
}

TEST(SwitchHeartbeat, AnyMaskedCoreIsClassAElseALateBeatIsClassB)
{
    switch_heartbeat_core cores[4] = {};
    for (auto &core : cores)
        switch_heartbeat_sample(&core, true, 1, 0);
    for (unsigned cpu = 0; cpu < 4; ++cpu)
        switch_heartbeat_sample(&cores[cpu], cpu != 2, 2 + cpu, 13 * kSecond);
    EXPECT_EQ(switch_heartbeat_class(cores, 4, 13 * kSecond, 2 * kSecond), SWITCH_HEARTBEAT_HEALTHY);
    EXPECT_EQ(switch_heartbeat_class(cores, 4, 13 * kSecond, 1 * kSecond), SWITCH_HEARTBEAT_UNSCHEDULED);
    for (unsigned cpu = 0; cpu < 4; ++cpu)
        switch_heartbeat_sample(&cores[cpu], cpu != 2, cpu == 3 ? 5 : 20 + cpu, 26 * kSecond);
    EXPECT_EQ(switch_heartbeat_core_kind(&cores[3], 26 * kSecond), SWITCH_HEARTBEAT_CORE_MASKED);
    EXPECT_EQ(switch_heartbeat_class(cores, 4, 26 * kSecond, 26 * kSecond), SWITCH_HEARTBEAT_MASKED);
    EXPECT_EQ(switch_heartbeat_class(cores, 3, 26 * kSecond, 26 * kSecond), SWITCH_HEARTBEAT_HEALTHY);
}

TEST(SwitchHeartbeatSource, ThePowerTrailHoldsItFromThePowerStepToTheWake)
{
    const std::string trail = switch_ios::test::SourceText("../../diagnostics/switch/SwitchPowerTrail.c");
    const std::string watch = switch_ios::test::Body(trail, "void switch_power_trail_watch(");
    const size_t hold = watch.find("switch_heartbeat_hold(SWITCH_HEARTBEAT_HOLD_POWER);");
    const size_t arm = watch.find("switch_panic_save_watchdog(seconds);");
    const size_t release = watch.find("switch_heartbeat_release(SWITCH_HEARTBEAT_HOLD_POWER);");
    ASSERT_NE(hold, std::string::npos);
    ASSERT_NE(release, std::string::npos);
    EXPECT_LT(hold, arm);
    EXPECT_LT(arm, release);
    const std::string heartbeat = switch_ios::test::SourceText("../switch/diag/heartbeat.c");
    EXPECT_THAT(heartbeat, HasSubstr("kdp_register_callout(switch_heartbeat_debugger, NULL);"));
}

TEST(SwitchHeartbeatSource, ItsPanicsAreWatchdogPanicsThatNameTheirClass)
{
    const std::string heartbeat = switch_ios::test::SourceText("../switch/diag/heartbeat.c");
    EXPECT_THAT(heartbeat, HasSubstr("panic(\"SwitchHeartbeat: class a, "));
    EXPECT_THAT(heartbeat, HasSubstr("panic(\"SwitchHeartbeat: class b, "));
    EXPECT_THAT(heartbeat, HasSubstr("switch_power_trail_beat_interrupt(now);\n"
                                     "    switch_heartbeat_panic(class, cores, count, now);"));
    const std::string save = switch_ios::test::SourceText("../../diagnostics/switch/SwitchPanicSave.c");
    EXPECT_THAT(save, HasSubstr("starts_with(panic_line, \"watchdog timeout: no checkins from \") ||\n"
                                "                     switch_panic_heartbeat_watchdog(panic_line);"));
    const std::string part = switch_ios::test::SourceText("../../diagnostics/switch/SwitchPanicHeartbeat.c");
    EXPECT_THAT(part, HasSubstr("static const char prefix[] = \"SwitchHeartbeat:\";"));
}

TEST(SwitchHeartbeatSource, TheDriverSaysWhatItDidAndEnablesTheLineOnlyWhenArmed)
{
    const std::string driver =
        switch_ios::test::SourceText("../../drivers/NativeSwitchWatchdog/NativeSwitchWatchdog.cpp");
    EXPECT_THAT(driver, HasSubstr("if (self->off_ && !self->connected_ && !self->heartbeatTried_)\n"
                                  "        IOLog(\"SwitchWatchdog: watchdogd connected; userspace watchdog off for "
                                  "this boot; %s\\n\", self->armHeartbeat());\n"));
    EXPECT_THAT(driver, HasSubstr("    if (armed)\n        heartbeat_->enable();\n    else\n"
                                  "        loop_->removeEventSource(heartbeat_);\n"));
}

// Every line of code naming the heartbeat sits in a DEVELOPMENT || DEBUG block.
void ExpectOnlyInDebug(const std::string &source, const std::string &name)
{
    std::istringstream lines(source);
    std::string line;
    bool debug = false;
    while (std::getline(lines, line)) {
        const size_t text = line.find_first_not_of(' ');
        const bool comment =
            text != std::string::npos &&
            (line.compare(text, 2, "//") == 0 || line.compare(text, 2, "/*") == 0 || line.compare(text, 1, "*") == 0);
        if (line == "#if DEVELOPMENT || DEBUG") {
            debug = true;
        } else if (line.rfind("#else", 0) == 0 || line.rfind("#endif", 0) == 0) {
            debug = false;
        } else if (!comment &&
                   (line.find("heartbeat") != std::string::npos || line.find("Heartbeat") != std::string::npos)) {
            EXPECT_TRUE(debug) << name << ": " << line;
        }
    }
}

TEST(SwitchHeartbeatSource, ReleaseKernelsHaveNoneOfIt)
{
    const std::string heartbeat = switch_ios::test::SourceText("../switch/diag/heartbeat.c");
    const size_t open = heartbeat.find("\n#if DEVELOPMENT || DEBUG\n");
    ASSERT_NE(open, std::string::npos);
    EXPECT_GT(heartbeat.find("\nstatic "), open);
    const std::string close = "\n#endif /* DEVELOPMENT || DEBUG */\n";
    ASSERT_GT(heartbeat.size(), close.size());
    EXPECT_EQ(heartbeat.compare(heartbeat.size() - close.size(), close.size(), close), 0);
    const std::string hooks = switch_ios::test::SourceText("../switch/include/switch/heartbeat.h");
    EXPECT_THAT(hooks, HasSubstr("#else\nstatic inline void switch_heartbeat_hold(__unused unsigned why)\n{}\n"));
    const std::string part = switch_ios::test::SourceText("../../diagnostics/switch/SwitchPanicHeartbeat.h");
    EXPECT_THAT(part, HasSubstr("#else\n#define switch_panic_heartbeat_watchdog(panic_line)  "
                                "((void)sizeof(panic_line), 0)\n#define switch_panic_heartbeat_story(watchdog_reset) "
                                "((void)sizeof(watchdog_reset), (const char *)0)\n#endif\n"));
    const std::string driver =
        switch_ios::test::SourceText("../../drivers/NativeSwitchWatchdog/NativeSwitchWatchdog.cpp");
    EXPECT_THAT(driver, HasSubstr("#else\n    IOLog(\"SwitchWatchdog: watchdogd connected; userspace watchdog "
                                  "armed%s\\n\",\n          self->power_ ? \"\" : \" (no sleep notifications)\");\n"
                                  "#endif\n"));
    ExpectOnlyInDebug(driver, "NativeSwitchWatchdog.cpp");
}

}  // namespace
