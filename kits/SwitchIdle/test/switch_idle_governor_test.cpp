// kits/SwitchIdle's state choice over a 19.2 MHz model clock, and the hooks that run it in both XNU trees.
#include "kits/SwitchIdle/switch_idle_governor.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace {

constexpr uint64_t kMs = SWITCH_IDLE_TICKS_US(1000);
constexpr uint64_t kFrame = SWITCH_IDLE_TICKS_US(16667);
constexpr uint64_t kTmrMax = UINT64_C(1) << 29;

std::string Text(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string Body(const std::string &source, const std::string &signature)
{
    const size_t at = source.find(signature);
    if (at == std::string::npos)
        return {};
    return source.substr(at, source.find("\n}\n", at) - at);
}

// One core's idle periods: each ends at its timer (`deadline` after it starts) or, sooner, at `woken`.
struct Core {
    uint64_t predicted = 0, now = 1'000 * kMs, frame_at = 0;
    unsigned c7 = 0, wfi = 0;

    uint32_t idle(uint64_t deadline, uint64_t woken = UINT64_MAX)
    {
        const uint64_t until = now + deadline;
        const uint32_t state = switch_idle_choose(predicted, now, until, frame_at);
        const uint64_t wake_at = state == SWITCH_IDLE_C7 ? switch_idle_c7_wake_at(until) : until;
        const uint64_t end = woken < wake_at - now ? now + woken : wake_at;
        predicted = switch_idle_learn(predicted, end - now, switch_idle_timer_bound(end, wake_at));
        now = end;
        (state == SWITCH_IDLE_C7 ? c7 : wfi)++;
        return state;
    }
};

TEST(SwitchIdleGovernor, TheConsolesC7Numbers)
{
    EXPECT_EQ(SWITCH_IDLE_C7_RESIDENCY, 19200u);
    EXPECT_EQ(SWITCH_IDLE_C7_WAKE_LATENCY, 2496u);
    EXPECT_EQ(SWITCH_IDLE_FRAME_RESIDENCY, 640012u);
}

TEST(SwitchIdleGovernor, AQuietCoreLearnsC7WithinAFewPeriods)
{
    Core core;
    unsigned first = 0;
    for (unsigned i = 0; i < 20; ++i) {
        if (core.idle(100 * kMs) == SWITCH_IDLE_C7 && !first)
            first = i + 1;
    }
    EXPECT_GT(first, 0u);
    EXPECT_LE(first, 2u);
    EXPECT_EQ(core.c7, 20u - first + 1);
}

TEST(SwitchIdleGovernor, NeverC7BeforeTheBreakEven)
{
    Core core;
    core.predicted = 1000 * kMs;
    EXPECT_EQ(core.idle(SWITCH_IDLE_C7_RESIDENCY - 1), SWITCH_IDLE_WFI);
    EXPECT_EQ(core.idle(SWITCH_IDLE_C7_RESIDENCY), SWITCH_IDLE_C7);
    EXPECT_EQ(switch_idle_choose(1000 * kMs, 50, 50, 0), SWITCH_IDLE_WFI);
    EXPECT_EQ(switch_idle_choose(1000 * kMs, 50, 10, 0), SWITCH_IDLE_WFI);
}

TEST(SwitchIdleGovernor, AnEarlyWakeTurnsC7OffAtOnce)
{
    Core core;
    core.predicted = 1000 * kMs;
    EXPECT_EQ(core.idle(100 * kMs, 200), SWITCH_IDLE_C7);
    EXPECT_EQ(core.predicted, 200u);
    for (unsigned i = 0; i < 50; ++i)
        EXPECT_EQ(core.idle(100 * kMs, kMs / 2), SWITCH_IDLE_WFI) << i;
}

TEST(SwitchIdleGovernor, TheRestOfAPeriodAfterAnEarlyC7WakeKeepsThePrediction)
{
    Core core;
    core.predicted = 1000 * kMs;
    const uint64_t before = core.predicted;
    core.idle(SWITCH_IDLE_C7_WAKE_LATENCY);
    EXPECT_GE(core.predicted, before);
}

TEST(SwitchIdleGovernor, ACoreInEveryFrameStaysInWfiWhileFramesFlow)
{
    Core core;
    for (unsigned i = 0; i < 600; ++i) {
        core.frame_at = core.now;
        core.idle(100 * kMs, kFrame - 4 * kMs);
        core.now += 4 * kMs;
    }
    EXPECT_EQ(core.c7, 0u);
}

TEST(SwitchIdleGovernor, AnIdleCoreStillTakesC7WhileFramesFlowElsewhere)
{
    Core core;
    for (unsigned i = 0; i < 20; ++i) {
        core.frame_at = core.now - kFrame;
        core.idle(250 * kMs);
    }
    EXPECT_GE(core.c7, 15u);
}

TEST(SwitchIdleGovernor, FramesStopAndTheGuardLiftsAfterTheWindow)
{
    const uint64_t now = 10'000 * kMs;
    EXPECT_EQ(switch_idle_choose(1000 * kMs, now, now + 20 * kMs, now - 99 * kMs), SWITCH_IDLE_WFI);
    EXPECT_EQ(switch_idle_choose(1000 * kMs, now, now + 20 * kMs, now - 101 * kMs), SWITCH_IDLE_C7);
    EXPECT_EQ(switch_idle_choose(1000 * kMs, now, now + 40 * kMs, now - kMs), SWITCH_IDLE_C7);
}

TEST(SwitchIdleGovernor, TheWakeTimerIsNeverLate)
{
    for (const uint64_t deadline : {SWITCH_IDLE_C7_RESIDENCY, 5 * kMs, 100 * kMs, 27'000 * kMs}) {
        const uint64_t now = 77 * kMs;
        const uint64_t at = switch_idle_c7_wake_at(now + deadline);
        const uint64_t ticks = switch_idle_timer_ticks(now, at, kTmrMax);
        EXPECT_EQ(now + ticks + SWITCH_IDLE_C7_WAKE_LATENCY, now + deadline) << deadline;
    }
}

TEST(SwitchIdleGovernor, ADeadlineBeyondTheTimerWakesAtItsReach)
{
    EXPECT_EQ(switch_idle_timer_ticks(0, 120'000 * kMs, kTmrMax), kTmrMax);
    EXPECT_EQ(switch_idle_timer_ticks(500, 400, kTmrMax), 1u);
}

TEST(SwitchIdleHooks, BothTreesIdleAndResumeThroughTheHooks)
{
    for (const auto &[cpu, init] : {std::pair{CPU_10063, INIT_10063}, {CPU_12377, INIT_12377}}) {
        const std::string idle = Body(Text(cpu), "\ncpu_idle(void)\n");
        ASSERT_FALSE(idle.empty()) << cpu;
        EXPECT_NE(idle.find("#else\n\t\tswitch_idle_wfi(cpu_data_ptr);\n#endif"), std::string::npos) << cpu;
        EXPECT_EQ(idle.find("__builtin_arm_wfi()"), std::string::npos) << cpu;
        EXPECT_NE(Text(cpu).find("#include <switch/idle.h>\n"), std::string::npos) << cpu;

        const std::string resume = Body(Text(init), "\narm_init_idle_cpu(");
        ASSERT_FALSE(resume.empty()) << init;
        EXPECT_NE(resume.find("\tfiq_context_init(FALSE);\n\n\tswitch_idle_resumed(cpu_data_ptr);\n"
                              "\tcpu_idle_exit(TRUE);"),
                  std::string::npos)
            << init;
        EXPECT_NE(Text(init).find("#endif /* HIBERNATION */\n#include <switch/idle.h>\n"), std::string::npos) << init;
    }
}

}  // namespace
