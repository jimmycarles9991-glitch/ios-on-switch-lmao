// SPDX-License-Identifier: MIT
// Guards on how the raw path keeps a command buffer with a Metal event on raw, which no host model runs (the facade, the
// frontend and m2v's events):
//  - a plain MTLEvent stays on raw as a shared one does: RawMetal asks an event for a shared event's value, set and
//    notifications, not for MTLSharedEvent (m2v_source_test holds the frontend's plain event to answering them);
//  - a request for a cross-process shared event is one SwitchRefusal line (frontend, sharedevent.handle).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(RawEventsSource, APlainEventStaysOnRawAsASharedOneDoes)
{
    const std::string raw = SourceText("../raw/runtime/RawMetalEvents.mm");
    const std::string record =
        Between(raw, "- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal", "[self record:m];");
    EXPECT_THAT(record, Not(HasSubstr("conformsToProtocol:@protocol(MTLSharedEvent)")));
    for (const char *selector :
         {"@selector(signaledValue)", "@selector(setSignaledValue:)", "@selector(notifyListener:atValue:block:)"})
        EXPECT_THAT(record, HasSubstr(selector));
}

TEST(RawEventsSource, ACrossProcessSharedEventIsARefusalLine)
{
    const std::string plugin = SourceText("../TegraGM20BMetalPlugin.mm");
    const std::string refuse = Between(plugin, "static void crossProcessEvent(", "\n}\n");
    EXPECT_THAT(refuse, HasSubstr("\"newSharedEventHandle\""));
    EXPECT_THAT(refuse, HasSubstr("\"newSharedEventWithHandle:\""));
    EXPECT_THAT(refuse, HasSubstr("switch_refusal(\"frontend\", \"sharedevent.handle\","));
    for (const char *hook : {"void missing(const char *cls, const char *selector)",
                             "void answeredZero(const char *cls, const char *selector)"})
        EXPECT_THAT(Between(plugin, hook, "\n}\n"), HasSubstr("crossProcessEvent(cls, selector);"));
}

}  // namespace
