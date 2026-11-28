// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;

TEST(SwitchButtonsSource, SharesWorkLoopWithHidDevice)
{
    const std::string start = Body(SourceText("../SwitchButtons.cpp"), "bool SwitchButtons::start(");
    ASSERT_FALSE(start.empty());
    const size_t loopAt = start.find("loop = IOWorkLoop::workLoop();");
    const size_t createAt = start.find("switch_hid_create(kSwitchHIDButtons, loop)");
    const size_t enableAt = start.find("source->enable();");
    ASSERT_NE(loopAt, std::string::npos);
    ASSERT_NE(createAt, std::string::npos);
    ASSERT_NE(enableAt, std::string::npos);
    EXPECT_EQ(Count(start, "switch_hid_create("), 1u);
    EXPECT_GT(createAt, loopAt);
    EXPECT_LT(createAt, enableAt);
}
