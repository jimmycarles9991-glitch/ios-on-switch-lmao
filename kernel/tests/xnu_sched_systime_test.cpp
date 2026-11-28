// kernel/switch/sched/systime.h on the host: which bucket a slice of a user thread's kernel time goes to, and the line;
// both trees' sleh_synchronous count an entry where the exception is marked, before interrupts are enabled, and
// recount hands it every kernel-level slice.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "bazel/testing/source_text.h"
#include "sched/systime.h"

using testing::HasSubstr;

namespace {

constexpr uint16_t kUser = SWITCH_SYSTIME_TRACE_USER;

TEST(XnuSchedSystime, SyscallsSplitByTrapNumber)
{
    EXPECT_EQ(switch_systime_trace_bucket(kUser | SWITCH_SYSTIME_EC_SVC_64, 4), SWITCH_SYSTIME_UNIX);
    EXPECT_EQ(switch_systime_trace_bucket(kUser | SWITCH_SYSTIME_EC_SVC_64, 0), SWITCH_SYSTIME_UNIX) << "syscall()";
    for (const int64_t msg : {-31, -32, -47})
        EXPECT_EQ(switch_systime_trace_bucket(kUser | SWITCH_SYSTIME_EC_SVC_64, msg), SWITCH_SYSTIME_MACH_MSG) << msg;
    EXPECT_EQ(switch_systime_trace_bucket(kUser | SWITCH_SYSTIME_EC_SVC_64, -36), SWITCH_SYSTIME_MACH)
        << "mach_wait_until";
}

TEST(XnuSchedSystime, FaultsAndTheTranslatorsTrapHaveTheirOwnBuckets)
{
    EXPECT_EQ(switch_systime_trace_bucket(kUser | SWITCH_SYSTIME_EC_DABORT_EL0, 0), SWITCH_SYSTIME_DATA_ABORT);
    EXPECT_EQ(switch_systime_trace_bucket(kUser | SWITCH_SYSTIME_EC_IABORT_EL0, 0), SWITCH_SYSTIME_INSTRUCTION_ABORT);
    EXPECT_EQ(switch_systime_trace_bucket(kUser | SWITCH_SYSTIME_EC_UNCATEGORIZED, 0), SWITCH_SYSTIME_UNDEFINED);
    EXPECT_EQ(switch_systime_trace_bucket(kUser | 0x3c, 0), SWITCH_SYSTIME_OTHER) << "BRK";
}

TEST(XnuSchedSystime, NoMarkedExceptionIsOutsideATrap)
{
    EXPECT_EQ(switch_systime_trace_bucket(0, 0), SWITCH_SYSTIME_OUTSIDE);
    EXPECT_EQ(switch_systime_trace_bucket(SWITCH_SYSTIME_EC_SVC_64, 4), SWITCH_SYSTIME_OUTSIDE)
        << "a kernel exception's code has no user bit";
}

TEST(XnuSchedSystime, TheLineGivesEachBucketsTimeAndEntries)
{
    switch_systime w = {};
    w.time[SWITCH_SYSTIME_UNIX] = 3200 * 1000000ull;
    w.entries[SWITCH_SYSTIME_UNIX] = 412000;
    w.time[SWITCH_SYSTIME_DATA_ABORT] = 2400 * 1000000ull;
    w.entries[SWITCH_SYSTIME_DATA_ABORT] = 310000;
    w.time[SWITCH_SYSTIME_OUTSIDE] = 1800 * 1000000ull;
    char line[512];
    switch_systime_format(line, sizeof(line), &w, 1, 1);
    EXPECT_EQ(std::string(line),
              "unix 3200 ms (412000), mach_msg 0 ms (0), other mach 0 ms (0), data abort 2400 ms (310000), "
              "instruction abort 0 ms (0), undefined 0 ms (0), other sync 0 ms (0), outside a trap 1800 ms");
    char small[16];
    switch_systime_format(small, sizeof(small), &w, 1, 1);
    EXPECT_EQ(std::string(small), "unix 3200 ms (4") << "cut, still terminated";
}

class XnuSchedSystimeSource : public testing::TestWithParam<const char *> {};

TEST_P(XnuSchedSystimeSource, SlehCountsAnEntryWhereItMarksTheException)
{
    const std::string tree = GetParam();
    const std::string sleh = switch_ios::test::SourceText("../" + tree + "/osfmk/arm64/sleh.c");
    EXPECT_THAT(sleh, HasSubstr("#define ARM64_KDBG_CODE_USER   (1 << 8)")) << "SWITCH_SYSTIME_TRACE_USER";
    const std::string synchronous = switch_ios::test::Body(sleh, "\nsleh_synchronous(arm_context_t *context");
    const size_t mark = synchronous.find("\t\tthread->machine.exception_trace_code = (uint16_t)(ARM64_KDBG_CODE_USER | "
                                         "class);\n\t\tswitch_systime_entered(thread);\n");
    const size_t enable = synchronous.find("ml_set_interrupts_enabled(TRUE);");
    ASSERT_NE(mark, std::string::npos);
    ASSERT_NE(enable, std::string::npos);
    EXPECT_LT(mark, enable) << "one writer per CPU: counted before interrupts are enabled";
}

TEST_P(XnuSchedSystimeSource, RecountHandsOverEveryKernelSlice)
{
    const std::string tree = GetParam();
    const std::string recount = switch_ios::test::SourceText("../" + tree + "/osfmk/kern/recount.c");
    const std::string absorb = switch_ios::test::Body(recount, "\nrecount_absorb_snap(struct recount_snap *to_add");
    EXPECT_THAT(absorb, HasSubstr("recount_usage_add_snap(&tk_track->rt_usage, level, to_add);\n\t\tif (level == "
                                  "RCT_LVL_KERNEL) {\n\t\t\tswitch_systime_absorb(thread, task, "
                                  "to_add->rsn_time_mach);\n\t\t}\n"));
}

TEST_P(XnuSchedSystimeSource, TheTrapIsKeptUntilItsLastSliceIsHandedOver)
{
    const std::string tree = GetParam();
    const std::string recount = switch_ios::test::SourceText("../" + tree + "/osfmk/kern/recount.c");
    EXPECT_THAT(
        switch_ios::test::Body(recount, "\nrecount_enter_user(void)"),
        HasSubstr("recount_transition(RCT_LVL_KERNEL, RCT_LVL_USER);\n\tswitch_systime_returned(current_thread());"))
        << "sleh clears exception_trace_code before this transition hands over the trap's last kernel slice";
    EXPECT_THAT(switch_ios::test::SourceText("../" + tree + "/osfmk/arm/thread.h"),
                HasSubstr("\tuint16_t                  exception_trace_code;\n#if DEVELOPMENT || DEBUG\n\tuint8_t "
                          "                  switch_systime_trap;"))
        << "the reserved byte after the trace code, so machine_thread keeps its layout; release keeps reserved7";
}

INSTANTIATE_TEST_SUITE_P(BothTrees, XnuSchedSystimeSource, testing::Values("xnu-10063.141.1", "xnu-12377.121.6"));

TEST(XnuSchedSystimeSource, ThePmuLinePricesEveryReturnToUserTwice)
{
    const std::string systime = switch_ios::test::SourceText("../switch/sched/systime.c");
    EXPECT_THAT(switch_ios::test::Body(systime, "\nvoid switch_systime_returned(thread_t thread)"),
                HasSubstr("add(&systime_cpu[cpu_number()].returns, 1);"))
        << "recount_enter_user runs at every return to EL0, an interrupt's as well as a trap's";
    EXPECT_THAT(systime, HasSubstr("report_pmu(line, size, window_ns, 2 * window_returns);"))
        << "each return pairs with its recount_leave_user";
}

TEST(XnuSchedSystimeSource, ReleaseKernelsCompileTheHooksToNothing)
{
    const std::string header = switch_ios::test::SourceText("../switch/include/switch/sched.h");
    const std::string stubs = header.substr(header.find("#else /* DEVELOPMENT || DEBUG */"));
    EXPECT_THAT(stubs, HasSubstr("static inline void switch_systime_entered("));
    EXPECT_THAT(stubs, HasSubstr("static inline void switch_systime_absorb("));
    EXPECT_THAT(stubs, HasSubstr("static inline void switch_systime_returned("));
    EXPECT_THAT(switch_ios::test::SourceText("../switch/sched/systime.c"),
                HasSubstr("\n#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED\n"));
}

}  // namespace
