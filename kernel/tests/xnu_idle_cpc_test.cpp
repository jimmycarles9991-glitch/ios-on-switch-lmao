// kernel/switch/idle/idle.c as xnu-12377 builds it with CONFIG_CPU_COUNTERS. 12377's CPC_CPU_IDLE leaves the PMU
// counting, which WFI keeps and a C7 power-down loses, so idle.c folds the counts in and masks PMIs (CPC_CPU_OFFLINE)
// before asking for C7, and restarts them (CPC_CPU_ACTIVE_COLD) when the firmware returns instead. A core that did
// power down comes back through cpu_idle_exit(TRUE), which issues CPC_CPU_ACTIVE_COLD itself.
#include <gtest/gtest.h>

#include <switch/idle.h>

#include <cstdint>
#include <string>
#include <vector>

#include "bazel/testing/fresh_process.h"
#include "kits/SwitchIdle/switch_idle_governor.h"
#include "xnu_idle_cpc_stubs.h"

namespace {
constexpr uint64_t kMs = SWITCH_IDLE_TICKS_US(1000);
constexpr int64_t kDenied = -3;  // PSCI_E_DENIED
constexpr int64_t kTaken = 0;    // CPU_SUSPEND returned at once for a pending interrupt
constexpr uint32_t kCpu = 1;

uint64_t gNow = 1'000 * kMs;
uint64_t gWfiUntil;
int64_t gAnswer;
cpu_data gCurrent;
thread_call_func_t gReport;
std::vector<std::string> gEvents;

const char *name(cpc_cpu_event_t event)
{
    switch (event) {
    case CPC_CPU_OFFLINE:
        return "offline";
    case CPC_CPU_ACTIVE_COLD:
        return "active-cold";
    default:
        return "other";
    }
}

int64_t powerDown(uint32_t, uint64_t)
{
    gEvents.emplace_back("c7");
    gNow += 1;
    return gAnswer;
}

void resumed(uint32_t)
{}

const switch_idle_platform kPlatform = {powerDown, resumed, uint64_t{1} << 29};

// One idle of kCpu whose timer is `away` ticks off: WFI lasts to the timer, a C7 request returns with gAnswer.
std::vector<std::string> idle(uint64_t away = 100 * kMs)
{
    gEvents.clear();
    gCurrent = {static_cast<int>(kCpu), gNow + away};
    gWfiUntil = gCurrent.rtcPop;
    switch_idle_wfi(&gCurrent);
    return gEvents;
}

// Idles until one asks for C7; that idle's events.
std::vector<std::string> idleUntilC7()
{
    for (unsigned i = 0; i < 100'000; ++i) {
        std::vector<std::string> events = idle();
        if (events.size() != 1 || events[0] != "wfi")
            return events;
    }
    ADD_FAILURE() << "no idle asked for C7";
    return {};
}
}  // namespace

extern "C" {
void cpc_cpu_transition(cpc_cpu_event_t event, void *cpu)
{
    EXPECT_EQ(cpu, &gCurrent);
    gEvents.emplace_back(name(event));
}

uint64_t mach_absolute_time(void)
{
    return gNow;
}

cpu_data_t *getCpuDatap(void)
{
    return &gCurrent;
}

void ClearIdlePop(int)
{}

void switch_secondary_vectors_leave(void)
{}

void xnu_idle_wfi(void)
{
    gEvents.emplace_back("wfi");
    if (gWfiUntil > gNow)
        gNow = gWfiUntil;
}

thread_call_t thread_call_allocate_with_options(thread_call_func_t func, thread_call_param_t, int, unsigned)
{
    gReport = func;
    return reinterpret_cast<thread_call_t>(&gReport);
}

bool thread_call_enter(thread_call_t)
{
    return true;
}

void os_log(void *, const char *, ...)
{}

void os_log_error(void *, const char *, ...)
{}
}

// Before ArmCpu starts C7, and after it for an idle shorter than C7's break-even.
TEST(XnuIdleCpc, AWfiIdleLeavesTheCountersRunning)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        EXPECT_EQ(idle(), std::vector<std::string>{"wfi"});
        switch_idle_start(&kPlatform);
        EXPECT_EQ(idle(SWITCH_IDLE_C7_RESIDENCY / 2), std::vector<std::string>{"wfi"});
    });
}

TEST(XnuIdleCpc, AC7RequestSavesTheCountersFirstAndARefusedOneRestartsThem)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_idle_start(&kPlatform);
        gAnswer = kDenied;
        EXPECT_EQ(idleUntilC7(), (std::vector<std::string>{"offline", "c7", "active-cold"}));
    });
}

TEST(XnuIdleCpc, AC7RequestTheFirmwareReturnedForAPendingInterruptRestartsThem)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        switch_idle_start(&kPlatform);
        gAnswer = kTaken;
        EXPECT_EQ(idleUntilC7(), (std::vector<std::string>{"offline", "c7", "active-cold"}));
    });
}
