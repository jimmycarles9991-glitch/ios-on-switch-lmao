// The pairing opens every rail session (SwitchJoyConRail.cpp, "Pairing"): a Joy-Con paired 2-3 s into a session a
// console boot had handshaken never paged the console when pulled off (boots 16 and 18), one paired right after its
// handshake did. So the handshake is the one place a pairing starts, a Joy-Con that ran unpaired until the address came
// has its session ended (91/11) and is handshaken again, and the session sends what Horizon's attach does before the
// pairing request. Every attach pairs, whatever the Joy-Con's pairing log holds (reading it is a debug build's
// diagnostic), and joyconpaird hands every rail record's key to bluetoothd.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchJoyConRailPairing, ThePairingOpensTheSession)
{
    const std::string rail = SourceText("../SwitchJoyConRail.cpp");
    EXPECT_EQ(Count(rail, "startPairing(s);"), 1u);
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::handshakeReply("), HasSubstr("startPairing(s);"));
    const std::string address = Body(rail, "void SwitchJoyConRail::bluetoothAddress(");
    EXPECT_THAT(address, Not(HasSubstr("startPairing")));
    EXPECT_THAT(address, Not(HasSubstr("endSession")));
    EXPECT_THAT(address, HasSubstr("s.pairPending |="));
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::tick("),
                HasSubstr("if (s.pairPending && hostKnown && s.unanswered == 0) {\n        endSession(s);"));
    const std::string end = Body(rail, "void SwitchJoyConRail::endSession(");
    EXPECT_THAT(end, HasSubstr("stopDmaLocked(s);"));
    EXPECT_THAT(end, HasSubstr("sendCommand(s, kInitHidDisconnect);"));
    // Its answer, or none, leads to detection, which handshakes a seated Joy-Con as on an attach.
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::received("),
                HasSubstr("if (cmd == kCmdInitRet && packet[6] == kInitHidDisconnect)\n            detach(s);"));
}

TEST(SwitchJoyConRailPairing, EverySessionSendsHorizonsAttachSequence)
{
    const std::string rail = SourceText("../SwitchJoyConRail.cpp");
    EXPECT_THAT(rail, HasSubstr("        kReadSlot,\n        kReadSlotTail,\n        kDeviceInfo,\n        kShipment,\n"
                                "        kSendHost,\n        kGetKey,\n        kSave\n"));
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::startPairing("),
                HasSubstr("s.step = kReportSlot ? kReadSlot : kDeviceInfo;"));
    EXPECT_THAT(rail, HasSubstr("#if DEVELOPMENT || DEBUG\nconstexpr bool kReportSlot = true;\n#else\nconstexpr bool "
                                "kReportSlot = false;"));
    const std::string walk = Body(rail, "void SwitchJoyConRail::pairingSlotReply(");
    EXPECT_THAT(walk, Not(HasSubstr("finishPairing(s, true")));
    EXPECT_THAT(walk, HasSubstr("s.step = kDeviceInfo;\n    sendPairStep(s);"));
    const std::string steps = Body(rail, "void SwitchJoyConRail::sendPairStep(");
    EXPECT_THAT(steps, HasSubstr("sendSubcommand(s, kSubShipment, &off, 1);"));
    EXPECT_THAT(steps, HasSubstr("switch_joycon_pairing_request(request, types[s.step - kSendHost], host)"));
    // Every subcommand of the rail's own is Horizon's 0x31-byte report.
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::sendSubcommand("),
                HasSubstr("switch_joycon_subcommand_report(report, s.counter++, id, data, length);"));
    EXPECT_EQ(Count(rail, "finishPairing(s, true,"), 1u);
    EXPECT_THAT(Body(rail, "void SwitchJoyConRail::pairingReply("), HasSubstr("finishPairing(s, true, \"rail\");"));
}

TEST(SwitchJoyConRailPairing, JoyconpairdHandsEveryRailKeyToBluetoothd)
{
    const std::string helper = SourceText("../helper/joyconpaird.c");
    EXPECT_THAT(helper, Not(HasSubstr("HostCapability")));
    EXPECT_THAT(Body(helper, "static void import("), HasSubstr("pair(address, name, classOfDevice,"));
}

}  // namespace
