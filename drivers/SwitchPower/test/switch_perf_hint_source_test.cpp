// SPDX-License-Identifier: BSD-2-Clause-Patent
// The parts of switch_perf_hint that run outside SwitchPerfGovernor.cpp, as their sources stand: what the touch filter
// pays in primary interrupt context for the GPU's idle clock (gpu_clock.c tegra_gm20b_perf_hint) and the EMC
// (SwitchEMC::hint). No lock, no read-modify-write, no sequentially consistent access. And XNU's task policy, in both
// trees, hands every change of a task's effective role to switch_perf_role: a resume to the front is a launch.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {
const std::vector<std::string> kForbidden = {
    "Lock",
    "lock(",
    "_mutex",
    "__atomic_exchange_n",
    "__atomic_compare_exchange_n",
    "__atomic_fetch_",
    "__ATOMIC_SEQ_CST",
    "deadline(",
};

std::vector<std::string> found(const std::string &body)
{
    std::vector<std::string> hits;
    for (const std::string &token : kForbidden) {
        if (body.find(token) != std::string::npos)
            hits.push_back(token);
    }
    return hits;
}
}  // namespace

TEST(SwitchPerfHintSource, TheGpuHintIsAPlainStoreOfATime)
{
    const std::string source = switch_ios::test::SourceText("../../TegraGM20B/gpu_clock.c");
    const std::string body = switch_ios::test::Body(source, "void tegra_gm20b_perf_hint(");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(found(body), std::vector<std::string>{});
    EXPECT_NE(body.find("__atomic_store_n(&tgm_idle.last_busy, nvxnu_nanotime(), __ATOMIC_RELAXED)"),
              std::string::npos);
}

TEST(SwitchPerfHintSource, TheEmcHintWritesItsCpusSlot)
{
    const std::string source =
        switch_ios::test::SourceText("../SwitchEMC.cpp") + switch_ios::test::SourceText("../SwitchEMCHints.h");
    const std::string body = switch_ios::test::Body(source, "void SwitchEMC::hint()");
    ASSERT_FALSE(body.empty());
    EXPECT_EQ(found(body), std::vector<std::string>{});
    EXPECT_NE(body.find("hints[cpu]"), std::string::npos);
}

TEST(SwitchPerfHintSource, BothKernelsHandEveryRoleChangeToTheGovernor)
{
    for (const char *tree : {"xnu-10063.141.1", "xnu-12377.121.6"}) {
        const std::string source =
            switch_ios::test::SourceText(std::string("../../../kernel/") + tree + "/osfmk/kern/task_policy.c");
        const std::string body = switch_ios::test::Body(source, "\ntask_policy_update_internal_locked(");
        ASSERT_FALSE(body.empty()) << tree;
        EXPECT_NE(body.find("if (prev.tep_role != next.tep_role) {\n"
                            "\t\tswitch_perf_role(next.tep_role == TASK_FOREGROUND_APPLICATION);\n"),
                  std::string::npos)
            << tree;
    }
}
