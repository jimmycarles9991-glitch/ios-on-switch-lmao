// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <string>

namespace {

namespace xnu17 {
#include "launchd_policy_tail_17.h"
static const char kTail[] = NATIVE_SWITCH_POLICY_TAIL;
#undef NATIVE_SWITCH_POLICY_TAIL
}  // namespace xnu17

namespace xnu27 {
#include "launchd_policy_tail_27.h"
static const char kTail[] = NATIVE_SWITCH_POLICY_TAIL;
#undef NATIVE_SWITCH_POLICY_TAIL
}  // namespace xnu27

TEST(PrngseeddPolicy, Ios27DisablesPrngseeddBesideExistingPolicies)
{
    const std::string tail(xnu27::kTail);
    EXPECT_NE(tail.find("<key>com.apple.prngseedd</key><true/>"), std::string::npos);
    EXPECT_NE(tail.find("<key>com.apple.lskdd</key><true/>"), std::string::npos);
    EXPECT_NE(tail.find("<key>com.apple.softwareupdateservicesd</key><true/>"), std::string::npos);
}

TEST(PrngseeddPolicy, Ios17KeepsItsPolicyUnchanged)
{
    const std::string tail(xnu17::kTail);
    EXPECT_EQ(tail.find("com.apple.prngseedd"), std::string::npos);
    EXPECT_NE(tail.find("<key>com.apple.lskdd</key><true/>"), std::string::npos);
}

}  // namespace
