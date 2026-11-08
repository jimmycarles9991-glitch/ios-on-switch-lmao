// Guards on NativeSwitchBlock.cpp's use of SwitchSdBus.h (sd_bus_test.cpp tests the tracker against the spec's model;
// the driver is not built for the host): every command claims the bus by one PRESENT_STATE read and never waits for it,
// a card still holding DAT0 is released by interrupt, and the one bounded PRESENT_STATE spin left is SDR104 tuning's.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::Index;
using switch_ios::test::Search;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeSwitchBlock.cpp");
    return source;
}

std::string body(const char *signature)
{
    return Body(driver(), signature);
}
}  // namespace

// The claim is one read: no loop, no clock, no sleep. A write finding the card busy waits by interrupt (awaitRelease).
TEST(SdBusSource, ClaimNeverWaits)
{
    const std::string claim = body("bool NativeSwitchBlock::claimBus(");
    EXPECT_FALSE(Search(claim, R"(\b(for|while|IOSleep|IODelay|switch_now_ns|awaitTuningIdle)\b)"));
    EXPECT_EQ(Count(claim, "r32(PRESENT)"), 2u);  // the claim, and again only after a release
    EXPECT_THAT(claim, HasSubstr("state == SwitchSdBusState::kCardBusy && awaitRelease() == 0"));
    const std::string release = body("int NativeSwitchBlock::awaitRelease()");
    EXPECT_THAT(release, HasSubstr("w16(COMMAND, 13u << 8 | RSP_R1B);"));
    EXPECT_THAT(release, HasSubstr("awaitStatus(INT_TRANSFER, kReleaseLimitNs)"));
    EXPECT_FALSE(Search(release, R"(\b(for|while|IOSleep|IODelay)\b)"));
    EXPECT_THAT(driver(), Not(HasSubstr("waitIdle(")));
}

// Every path that writes COMMAND claims the bus first and records the end it took; resets clear the tracker.
TEST(SdBusSource, EveryCommandClaimsAndEnds)
{
    for (const char *signature : {"int NativeSwitchBlock::issue(", "int NativeSwitchBlock::cqCommand(",
                                  "int NativeSwitchBlock::cardCommand(", "void NativeSwitchBlock::recover("}) {
        const std::string text = body(signature);
        EXPECT_LT(Index(text, "claimBus("), Index(text, "w16(COMMAND")) << signature;
    }
    // The release claims the tracker directly: claimBus calls it for a busy card.
    const std::string release = body("int NativeSwitchBlock::awaitRelease()");
    EXPECT_LT(Index(release, "bus.claim(r32(PRESENT), SwitchSdUse::kBusy)"), Index(release, "w16(COMMAND"));
    EXPECT_THAT(release, Not(HasSubstr("claimBus(")));
    for (const char *signature : {"int NativeSwitchBlock::complete(", "int NativeSwitchBlock::cqCommand(",
                                  "int NativeSwitchBlock::cardCommand(", "void NativeSwitchBlock::recover(",
                                  "int NativeSwitchBlock::awaitRelease()"}) {
        EXPECT_THAT(body(signature), HasSubstr("ended(s);")) << signature;
    }
    EXPECT_THAT(body("int NativeSwitchBlock::complete("),
                HasSubstr("if (!bus.idle() && awaitRelease())\n        return 10;"));
    EXPECT_THAT(body("void NativeSwitchBlock::resetLines()"), HasSubstr("bus.reset();"));
    EXPECT_THAT(body("void NativeSwitchBlock::powerOnController()"), HasSubstr("bus.reset();"));
    // COMMAND is written by those five and by tuning alone.
    EXPECT_EQ(Count(driver(), "w16(COMMAND,"), 6u);
}

// SDR104 tuning raises no transfer complete, so its PRESENT_STATE wait stays; nothing on the I/O path reaches it.
TEST(SdBusSource, TuningWaitStaysInTuning)
{
    EXPECT_EQ(Count(driver(), "awaitTuningIdle("), 4u);  // declaration, definition, tuneStep, tune
    EXPECT_THAT(body("void NativeSwitchBlock::tuneStep()"), HasSubstr("awaitTuningIdle("));
    EXPECT_THAT(body("bool NativeSwitchBlock::tune()"), HasSubstr("awaitTuningIdle("));
    for (const char *signature :
         {"int NativeSwitchBlock::issue(", "int NativeSwitchBlock::complete(", "void NativeSwitchBlock::beginBatch(",
          "void NativeSwitchBlock::endBatch(", "void NativeSwitchBlock::beginTask(", "int NativeSwitchBlock::endTask(",
          "int NativeSwitchBlock::cqCommand(", "void NativeSwitchBlock::worker("}) {
        EXPECT_FALSE(Search(body(signature), R"(\b(IOSleep|IODelay|awaitTuningIdle)\b)")) << signature;
    }
}
