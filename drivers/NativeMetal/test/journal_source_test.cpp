// SPDX-License-Identifier: MIT
// The journal line of a state the raw GM20B path completed at its creation is built on the creating thread, which can
// be Core Animation's render thread, and written on the frontend's persist queue: that thread never takes the
// journal's lock or its write(2). Release builds compile the call out.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(JournalSource, ARawMadeStatesLineIsBuiltOnItsThreadAndWrittenOnThePersistQueue)
{
    const std::string frontend = SourceText("../NativeBridgeMetalFrontend.mm");
    const std::string call =
        Body(frontend, "extern \"C\" void native_bridge_metal_journal_render(id descriptor, NSUInteger options)");
    const std::string gated = Between(call, "#if SWITCH_FRAME_TIMELINE", "#endif");
    const size_t queued = gated.find("dispatch_async(persistQueue(), ^{");
    ASSERT_NE(queued, std::string::npos);
    const std::string onThread = gated.substr(0, queued);
    EXPECT_THAT(onThread, HasSubstr("renderPipelineLine("));
    EXPECT_THAT(onThread, Not(HasSubstr("journal(")));
    EXPECT_THAT(onThread, Not(HasSubstr("journalRenderPipeline(")));
    EXPECT_THAT(Between(gated.substr(queued), "^{", "});"), HasSubstr("journal(line);"));

    const std::string line = Body(frontend, "static std::string renderPipelineLine(");
    EXPECT_THAT(line, HasSubstr("pthread_threadid_np(nullptr, &tid);"));
    EXPECT_THAT(line, HasSubstr("quartzCoreCallers()"));
    EXPECT_THAT(line, Not(HasSubstr("journal(")));
}

TEST(JournalSource, ThePersistQueueIsOneSerialUtilityQueue)
{
    const std::string queue =
        Body(SourceText("../NativeBridgeMetalFrontend.mm"), "static dispatch_queue_t persistQueue()");
    EXPECT_THAT(queue, HasSubstr("dispatch_once("));
    EXPECT_THAT(queue, HasSubstr("DISPATCH_QUEUE_SERIAL"));
    EXPECT_THAT(queue, HasSubstr("QOS_CLASS_UTILITY"));
}

TEST(JournalSource, TheRawPathCallsTheJournalOnlyInDevelopmentBuilds)
{
    const std::string states = SourceText("../../TegraGM20BMetal/raw/runtime/RawMetalStates.mm");
    EXPECT_THAT(Between(states, "#if SWITCH_RAW_DIAGNOSTICS\n    // the journal names m2v's functions", "#endif"),
                HasSubstr("native_bridge_metal_journal_render(twinDescriptor(a.descriptor), 0);"));
    const std::string frontend = SourceText("../NativeBridgeMetalFrontend.mm");
    EXPECT_THAT(frontend, HasSubstr("#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG\nconstexpr bool kJournal = true;"));
}

}  // namespace
