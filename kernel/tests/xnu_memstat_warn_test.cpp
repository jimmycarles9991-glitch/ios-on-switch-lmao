// kernel/switch/include/switch/memstat_warn.h: a daemon that goes clean above 80% of its inactive limit is warned once per
// growth, not at every clean transition (review-memory3: iconservicesagent at ~16 MB against 15 MB was warned at every XPC
// transaction's end), and both trees' memorystatus_dirty_set use it.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "switch/memstat_warn.h"

using switch_ios::test::Body;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
constexpr uint64_t kMiB = 1ull << 20;
constexpr const char *k10063 = "../xnu-10063.141.1/bsd/kern/kern_memorystatus.c";
constexpr const char *k12377 = "../xnu-12377.121.6/bsd/kern/kern_memorystatus.c";
}  // namespace

TEST(XnuMemstatWarn, ADaemonThatStaysHighIsWarnedOnceNotPerTransaction)
{
    uint64_t warned = 0;
    unsigned warnings = 0;
    for (int transaction = 0; transaction < 100; ++transaction)
        warnings += switch_memstat_inactive_warn(16 * kMiB + uint64_t(transaction) * 4096, 15 * kMiB, &warned);
    EXPECT_EQ(warnings, 1u);
}

TEST(XnuMemstatWarn, OneThatGrewAStepIsWarnedAgain)
{
    uint64_t warned = 0;
    EXPECT_TRUE(switch_memstat_inactive_warn(13 * kMiB, 15 * kMiB, &warned));
    EXPECT_FALSE(switch_memstat_inactive_warn(13 * kMiB + 512 * 1024, 15 * kMiB, &warned)) << "half a MiB: under 5%";
    EXPECT_TRUE(switch_memstat_inactive_warn(14 * kMiB, 15 * kMiB, &warned)) << "a MiB more: past 5% of 15 MiB";
}

TEST(XnuMemstatWarn, BelowTheLevelOrWithNoLimitNothingIsWarned)
{
    uint64_t warned = 0;
    EXPECT_FALSE(switch_memstat_inactive_warn(11 * kMiB, 15 * kMiB, &warned)) << "73% of 15 MiB";
    EXPECT_FALSE(switch_memstat_inactive_warn(100 * kMiB, 0, &warned));
    EXPECT_EQ(warned, 0u);
}

TEST(XnuMemstatWarn, TheInactiveLimitAppliedRearmsIt)
{
    uint64_t warned = 0;
    EXPECT_TRUE(switch_memstat_inactive_warn(13 * kMiB, 15 * kMiB, &warned));
    EXPECT_FALSE(switch_memstat_inactive_warn(13 * kMiB, 15 * kMiB, &warned));
    warned = 0;
    EXPECT_TRUE(switch_memstat_inactive_warn(13 * kMiB, 15 * kMiB, &warned));
}

// Both trees warn through it at the clean transition, with the mark on the proc, and clear the mark where they apply the
// inactive limit; 10063 warns only while the process ages (its equivalent of 12377's memstat_proc_is_active_locked)
TEST(XnuMemstatWarnSource, BothTreesWarnThroughItAndClearItAtTheInactiveLimit)
{
    for (const char *path : {k10063, k12377}) {
        const CodeText code(SourceText(path));
        EXPECT_TRUE(code.has("#include <switch/memstat_warn.h>")) << path;
        EXPECT_TRUE(code.has("switch_memstat_inactive_warn(footprint, limit, &p->p_memstat_inactive_warned)")) << path;
        EXPECT_FALSE(code.has("footprint >= limit / 100 * MEMSTAT_INACTIVE_WARNING_LEVEL")) << path;
    }
    EXPECT_TRUE(Body(SourceText(k12377), "memstat_update_memlimit_locked(proc_t p, bool use_active)")
                    .find("p->p_memstat_inactive_warned = 0;") != std::string::npos);
    const std::string memorystatus10063 = SourceText(k10063);
    const size_t macro = memorystatus10063.find("#define CACHE_INACTIVE_LIMITS_LOCKED(p, is_fatal)");
    ASSERT_NE(macro, std::string::npos);
    EXPECT_THAT(memorystatus10063.substr(macro, memorystatus10063.find("MACRO_END", macro) - macro),
                HasSubstr("(p)->p_memstat_inactive_warned = 0;"));
    EXPECT_TRUE(CodeText(SourceText(k10063))
                    .has("if (was_dirty && p == current_proc() && p->p_memstat_memlimit_inactive > 0 && "
                         "(p->p_memstat_dirty & P_DIRTY_AGING_IN_PROGRESS))"));
    for (const char *path :
         {"../xnu-10063.141.1/bsd/sys/proc_internal.h", "../xnu-12377.121.6/bsd/sys/proc_internal.h"})
        EXPECT_THAT(SourceText(path), HasSubstr("p_memstat_inactive_warned;")) << path;
}
