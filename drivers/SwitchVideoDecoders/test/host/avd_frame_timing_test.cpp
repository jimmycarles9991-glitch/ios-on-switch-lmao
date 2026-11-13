// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decode session's slowest frame, split at its NVDEC submits and fences (AVD/AVDFrameTiming.h), and the debug-only
// line the session's end logs it in.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "drivers/SwitchVideoDecoders/AVD/AVDFrameTiming.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
constexpr const char *kDebugOnly = "#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG\n";

std::string DebugPart(const std::string &body)
{
    return Between(body, kDebugOnly, "#endif");
}
}  // namespace

TEST(AvdFrameTiming, AFrameSplitsAtItsSubmitAndFence)
{
    AVDFrameTiming t = {};
    avd_frame_begin(&t, 100);
    avd_frame_lap(&t, AVD_BEFORE_SUBMIT, 130);
    avd_frame_lap(&t, AVD_SUBMIT_TO_FENCE, 1130);
    avd_frame_end(&t, 1200);
    EXPECT_EQ(t.slowest[AVD_BEFORE_SUBMIT], 30u);
    EXPECT_EQ(t.slowest[AVD_SUBMIT_TO_FENCE], 1000u);
    EXPECT_EQ(t.slowest[AVD_FENCE_TO_OUTPUT], 70u);
}

TEST(AvdFrameTiming, HiddenFramesAddUpAndOnlyTheSlowestFrameIsKept)
{
    AVDFrameTiming t = {};
    avd_frame_begin(&t, 0);
    avd_frame_lap(&t, AVD_BEFORE_SUBMIT, 10);
    avd_frame_lap(&t, AVD_SUBMIT_TO_FENCE, 110);
    avd_frame_lap(&t, AVD_BEFORE_SUBMIT, 130);
    avd_frame_lap(&t, AVD_SUBMIT_TO_FENCE, 230);
    avd_frame_end(&t, 240);
    EXPECT_EQ(t.slowest[AVD_BEFORE_SUBMIT], 30u);
    EXPECT_EQ(t.slowest[AVD_SUBMIT_TO_FENCE], 200u);
    EXPECT_EQ(t.slowest[AVD_FENCE_TO_OUTPUT], 10u);

    avd_frame_begin(&t, 1000);
    avd_frame_lap(&t, AVD_BEFORE_SUBMIT, 1005);
    avd_frame_lap(&t, AVD_SUBMIT_TO_FENCE, 1105);
    avd_frame_end(&t, 1110);
    EXPECT_EQ(avd_frame_total(t.slowest), 240u) << "a faster frame leaves the slowest as it was";
    EXPECT_EQ(t.slowest[AVD_SUBMIT_TO_FENCE], 200u);
}

TEST(AvdFrameTiming, SessionEndLogsItsSlowestFrameInDebugBuildsOnly)
{
    const std::string source = SourceText("../../AVD/AVDDecoder.c");
    EXPECT_THAT(DebugPart(Body(source, "static void finalize(CMBaseObjectRef ref)\n")), HasSubstr("slowest frame"));
    EXPECT_THAT(DebugPart(Body(source, "static int jobSubmit(")), HasSubstr("AVD_BEFORE_SUBMIT"));
    EXPECT_THAT(DebugPart(Body(source, "static int jobWait(")), HasSubstr("AVD_SUBMIT_TO_FENCE"));
}
