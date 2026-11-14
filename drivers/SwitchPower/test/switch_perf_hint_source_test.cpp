// SPDX-License-Identifier: BSD-2-Clause-Patent
// The parts of switch_perf_hint that run outside SwitchPerfGovernor.cpp, as their sources stand: what the touch filter
// pays in primary interrupt context for the GPU's idle clock (gpu_clock.c tegra_gm20b_perf_hint) and the EMC
// (SwitchEMC::hint). No lock, no read-modify-write, no sequentially consistent access.
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
