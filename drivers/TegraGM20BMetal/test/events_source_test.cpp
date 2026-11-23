// SPDX-License-Identifier: MIT
// Guards on how the driver keeps a command buffer with a Metal event on raw, which no host model runs (the facade, the
// frontend and m2v's events): a plain MTLEvent stays on raw as a shared one does: TegraGM20BMetal asks an event for a shared
// event's value, set and notifications, not for MTLSharedEvent.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

TEST(GM20BEventsSource, APlainEventStaysOnRawAsASharedOneDoes)
{
    const std::string raw = SourceText("../TegraGM20BMetalEvents.mm");
    const std::string record =
        Between(raw, "- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal", "[self record:m];");
    EXPECT_THAT(record, Not(HasSubstr("conformsToProtocol:@protocol(MTLSharedEvent)")));
    for (const char *selector :
         {"@selector(signaledValue)", "@selector(setSignaledValue:)", "@selector(notifyListener:atValue:block:)"})
        EXPECT_THAT(record, HasSubstr(selector));
}

}  // namespace
