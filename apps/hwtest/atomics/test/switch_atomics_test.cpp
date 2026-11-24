extern "C" {
#include "apps/hwtest/atomics/switch_atomics.h"
}

#include <gtest/gtest.h>

#include <string>

namespace {
constexpr unsigned kThreads = 8;
constexpr unsigned kOps = 8000;
}  // namespace

TEST(Atomics, TheBuildAssemblesTheFormsItWasAskedFor)
{
    EXPECT_EQ(switch_atomics_lse(), SWITCH_ATOMICS_EXPECT_LSE != 0);
}

TEST(Atomics, EveryFormHoldsItsInvariantsFromMoreThreadsThanCores)
{
    ASSERT_EQ(switch_atomics_forms(), 194u);
    for (unsigned i = 0; i < switch_atomics_forms(); i++) {
        switch_atomics_row row{};
        ASSERT_TRUE(switch_atomics_form(i, kThreads, kOps, &row)) << i;
        EXPECT_EQ(row.violations, 0u) << row.name;
        EXPECT_EQ(row.clobbers, 0u) << row.name;
        EXPECT_FALSE(row.branched) << row.name << ": nothing translates on the Mac";
        EXPECT_GE(row.ops, kThreads * kOps / 2) << row.name;
    }
}

TEST(Atomics, AppleEntryPointsHoldTheSameInvariants)
{
    ASSERT_GT(switch_atomics_library_forms(), 20u);
    for (unsigned i = 0; i < switch_atomics_library_forms(); i++) {
        switch_atomics_row row{};
        ASSERT_TRUE(switch_atomics_library_form(i, kThreads, kOps, &row)) << i;
        EXPECT_EQ(row.violations, 0u) << row.name;
        EXPECT_EQ(row.ops, kThreads * kOps) << row.name;
    }
}

TEST(Atomics, NoRowThatMustOrderSeesStoreBuffering)
{
    unsigned judged = 0;
    for (unsigned i = 0; i < switch_atomics_litmus_tests(); i++) {
        switch_litmus_row row{};
        ASSERT_TRUE(switch_atomics_litmus(i, 1u << 18, &row)) << i;
        EXPECT_EQ(row.runs, 1u << 18) << row.name;
        EXPECT_EQ(row.has_site, std::string(row.name).find("_translated_") != std::string::npos) << row.name;
        EXPECT_FALSE(row.branched) << row.name;
        if (row.judged) {
            judged++;
            EXPECT_EQ(row.forbidden, 0u) << row.name << " over " << row.runs << " runs";
        }
    }
    EXPECT_EQ(judged, SWITCH_ATOMICS_EXPECT_LSE ? 4u : 2u);
}

TEST(Atomics, RefusesWhatItCannotRun)
{
    switch_atomics_row row{};
    switch_litmus_row litmus{};
    EXPECT_FALSE(switch_atomics_form(switch_atomics_forms(), kThreads, kOps, &row));
    EXPECT_FALSE(switch_atomics_form(0, 1, kOps, &row));
    EXPECT_FALSE(switch_atomics_form(0, 17, kOps, &row));
    EXPECT_FALSE(switch_atomics_library_form(switch_atomics_library_forms(), kThreads, kOps, &row));
    EXPECT_FALSE(switch_atomics_litmus(switch_atomics_litmus_tests(), 1u << 18, &litmus));
    EXPECT_FALSE(switch_atomics_litmus(0, 100, &litmus));
}
