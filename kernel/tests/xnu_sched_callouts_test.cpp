// kernel/switch/sched/callouts.h on the host: thread call functions and timer actions by CPU, per CPU table and
// summed, each window's top four and the rest; both trees bracket thread_call_invoke's function and every
// IOTimerEventSource action where invokeAction runs it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>

#include "bazel/testing/source_text.h"
#include "sched/callouts.h"

using testing::HasSubstr;

namespace {

constexpr uint64_t kMs = 1000000;

uint64_t Same(uint64_t address)
{
    return address;
}

const char *const kKernel = "kernel";
const char *const kKernelHigh = "kernel-high";

uint64_t G(const char *name)
{
    return reinterpret_cast<uint64_t>(name);
}

const char *Name(uint64_t group)
{
    return group ? reinterpret_cast<const char *>(group) : "timer actions";
}

struct Report {
    std::unique_ptr<switch_callout_sum> seen = std::make_unique<switch_callout_sum>();
    std::unique_ptr<switch_callout_sum> window = std::make_unique<switch_callout_sum>();

    std::string Window(const switch_callout_table *tables, int count)
    {
        auto now = std::make_unique<switch_callout_sum>();
        for (int i = 0; i < count; i++)
            switch_callout_sum_table(now.get(), &tables[i]);
        switch_callout_window(now.get(), seen.get(), window.get());
        char line[512];
        switch_callout_format(line, sizeof(line), window.get(), Name, Same, 1, 1);
        return line;
    }
};

TEST(XnuSchedCallouts, EachGroupNamesItsBusiestFunctionsAcrossCpus)
{
    auto cpus = std::make_unique<switch_callout_table[]>(2);
    switch_callout_add(&cpus[0], 0xa000, G(kKernel), 700 * kMs);
    switch_callout_add(&cpus[1], 0xa000, G(kKernel), 36 * kMs);
    switch_callout_add(&cpus[1], 0xb000, G(kKernel), 12 * kMs);
    switch_callout_add(&cpus[0], 0xb100, G(kKernel), 3 * kMs);
    switch_callout_add(&cpus[0], 0xc000, G(kKernelHigh), 20 * kMs);
    switch_callout_add(&cpus[1], 0xd000, 0, 9 * kMs);
    Report r;
    EXPECT_EQ(r.Window(cpus.get(), 2), "kernel 0xa000 736 ms (2), 0xb000 12 ms (1) (others 3 ms); kernel-high 0xc000 "
                                       "20 ms (1) (others 0 ms); timer actions 0xd000 9 ms (1) (others 0 ms); "
                                       "unlisted 0 ms (0)");
    switch_callout_add(&cpus[1], 0xc000, G(kKernelHigh), 5 * kMs);
    EXPECT_EQ(r.Window(cpus.get(), 2), "kernel-high 0xc000 5 ms (1) (others 0 ms); unlisted 0 ms (0)")
        << "the window's share only";
}

TEST(XnuSchedCallouts, OneFunctionInTwoGroupsIsTwoRows)
{
    auto t = std::make_unique<switch_callout_table>();
    switch_callout_add(t.get(), 0xa000, G(kKernel), 4 * kMs);
    switch_callout_add(t.get(), 0xa000, G(kKernelHigh), 6 * kMs);
    Report r;
    EXPECT_EQ(r.Window(t.get(), 1), "kernel-high 0xa000 6 ms (1) (others 0 ms); kernel 0xa000 4 ms (1) (others 0 ms); "
                                    "unlisted 0 ms (0)");
}

TEST(XnuSchedCallouts, AFullTableCountsTheRestInItsOtherRow)
{
    auto t = std::make_unique<switch_callout_table>();
    for (uint64_t f = 1; f <= SWITCH_CALLOUT_ROWS + 2; f++)
        switch_callout_add(t.get(), f << 12, G(kKernel), kMs);
    EXPECT_EQ(t->other.calls, 2u);
    Report r;
    EXPECT_THAT(r.Window(t.get(), 1), HasSubstr("(others 62 ms); unlisted 2 ms (2)"));
}

class XnuSchedCalloutsSource : public testing::TestWithParam<const char *> {};

TEST_P(XnuSchedCalloutsSource, ThreadCallFunctionsAndTimerActionsAreBracketed)
{
    const std::string tree = GetParam();
    const std::string calls = switch_ios::test::SourceText("../" + tree + "/osfmk/kern/thread_call.c");
    EXPECT_THAT(switch_ios::test::Body(calls, "\nthread_call_invoke(thread_call_func_t func"),
                HasSubstr("#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED\n"
                          "\tconst uint64_t switch_begin = switch_callout_begin();\n#endif\n"
                          "\t(*func)(param0, param1);\n"
                          "#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED\n"
                          "\tswitch_callout_end((const void *)func, group->tcg_name, switch_begin);\n#endif\n"));
    const std::string timer = switch_ios::test::SourceText("../" + tree + "/iokit/Kernel/IOTimerEventSource.cpp");
    EXPECT_THAT(switch_ios::test::Body(timer, "\nIOTimerEventSource::invokeAction(IOEventSource::Action _action"),
                HasSubstr("\tconst uint64_t switch_begin = switch_callout_begin();\n\tif (kActionBlock & flags) {\n"
                          "\t\t((IOTimerEventSource::ActionBlock) actionBlock)(ts);\n\t} else {\n"
                          "\t\t((IOTimerEventSource::Action)_action)(_owner, ts);\n\t}\n"
                          "\tswitch_callout_end(address, NULL, switch_begin);\n"))
        << "keyed by the code invokeAction computes: a block source's invoke function, not the block object";
    EXPECT_THAT(timer, testing::Not(HasSubstr("switch_callout_end((const void *)doit")));
}

INSTANTIATE_TEST_SUITE_P(BothTrees, XnuSchedCalloutsSource, testing::Values("xnu-10063.141.1", "xnu-12377.121.6"));

}  // namespace
