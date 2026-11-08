// drivers/NativeSwitchBlock/SwitchSdBus.h against a model of the SD Host Controller Simplified Specification 4.20's
// PRESENT_STATE (2.2.9) and interrupt status (2.2.17): once a command's own interrupts are taken the bus is free at the
// first read, a command whose interrupts were not taken is refused, and a card still holding DAT0 after a write's
// transfer complete is released by an R1b before the next data command.
#include <gtest/gtest.h>

#include <cstdint>
#include <random>

#include "../SwitchSdBus.h"

namespace {
using Use = SwitchSdUse;
using State = SwitchSdBusState;

constexpr uint32_t kIdle = kSdPresentDat0 | 0xe00000u;  // DAT[3:0] high

// The controller as the spec describes it: the inhibits a command sets and the edges that clear them, each edge latching
// the interrupt it raises.
struct SpecController {
    uint32_t present = kIdle;
    uint32_t status = 0;
    bool cardBusy = false;
    bool usesDat = false;

    void command(Use use)
    {
        status = 0;
        usesDat = use != Use::kCommand;
        present |= kSdPresentCmdInhibit;
        if (usesDat)
            present |= kSdPresentDatInhibit | kSdPresentDatActive;
        if (use == Use::kRead)
            present |= kSdPresentReadActive;
        if (use == Use::kWrite)
            present |= kSdPresentWriteActive;
    }

    void respond()
    {
        present &= ~kSdPresentCmdInhibit;
        status |= kSdCommandComplete;
    }

    // The data phase or the busy ends; `holdDat0`: the card keeps programming past transfer complete, as a controller
    // ending a write early would leave it.
    void finish(bool holdDat0 = false)
    {
        present &= ~(kSdPresentDatInhibit | kSdPresentDatActive | kSdPresentReadActive | kSdPresentWriteActive);
        status |= kSdTransferComplete;
        cardBusy = holdDat0;
        if (holdDat0)
            present &= ~kSdPresentDat0;
    }

    void release()
    {
        cardBusy = false;
        present |= kSdPresentDat0;
    }
};

// One command as the driver runs it: claim, issue, the interrupts it awaits, ended().
void run(SwitchSdBus &bus, SpecController &hc, Use use, bool holdDat0 = false)
{
    ASSERT_EQ(bus.claim(hc.present, use), State::kFree);
    hc.command(use);
    hc.respond();
    if (use != Use::kCommand)
        hc.finish(holdDat0);
    bus.ended(hc.status, hc.present);
}

TEST(SwitchSdBus, WhatEachUseOwes)
{
    EXPECT_EQ(switchSdEnds(Use::kCommand), kSdCommandComplete);
    for (Use use : {Use::kRead, Use::kBusy, Use::kWrite, Use::kAbort})
        EXPECT_EQ(switchSdEnds(use), kSdCommandComplete | kSdTransferComplete);
    EXPECT_FALSE(switchSdHoldsCard(Use::kCommand));
    EXPECT_FALSE(switchSdHoldsCard(Use::kRead));
    for (Use use : {Use::kBusy, Use::kWrite, Use::kAbort})
        EXPECT_TRUE(switchSdHoldsCard(use));
    // CMD-line-only commands and the abort's CMD12 need Command Inhibit (CMD) clear; the rest the DAT side too.
    EXPECT_EQ(switchSdInhibits(Use::kCommand), kSdPresentCmdInhibit);
    EXPECT_EQ(switchSdInhibits(Use::kAbort), kSdPresentCmdInhibit);
    for (Use use : {Use::kRead, Use::kBusy, Use::kWrite}) {
        EXPECT_EQ(switchSdInhibits(use), kSdPresentCmdInhibit | kSdPresentDatInhibit | kSdPresentDatActive |
                                             kSdPresentWriteActive | kSdPresentReadActive);
    }
}

TEST(SwitchSdBus, FreshAndResetBusIsFree)
{
    SwitchSdBus bus;
    for (Use use : {Use::kCommand, Use::kRead, Use::kBusy, Use::kWrite, Use::kAbort})
        EXPECT_EQ(bus.state(kIdle, use), State::kFree);
    bus.issued(Use::kWrite);
    EXPECT_FALSE(bus.idle());
    bus.reset();
    EXPECT_TRUE(bus.idle());
    EXPECT_EQ(bus.state(kIdle, Use::kWrite), State::kFree);
}

TEST(SwitchSdBus, UntakenInterruptsRefuseTheNextCommand)
{
    SwitchSdBus bus;
    bus.issued(Use::kRead);
    EXPECT_EQ(bus.state(kIdle, Use::kCommand), State::kOwed);
    bus.ended(kSdCommandComplete, kIdle);  // a read's command complete alone: its data phase is still owed
    EXPECT_EQ(bus.state(kIdle, Use::kRead), State::kOwed);
    bus.ended(kSdTransferComplete, kIdle);
    EXPECT_EQ(bus.state(kIdle, Use::kRead), State::kFree);
    // An error interrupt without the awaited bit leaves it owed until the lines are reset.
    bus.issued(Use::kBusy);
    bus.ended(kSdCommandComplete | (1u << 16), kIdle);
    EXPECT_EQ(bus.state(kIdle, Use::kCommand), State::kOwed);
    bus.reset();
    EXPECT_EQ(bus.state(kIdle, Use::kCommand), State::kFree);
}

TEST(SwitchSdBus, InhibitAfterItsInterruptsIsHeldNotAWait)
{
    SwitchSdBus bus;
    EXPECT_EQ(bus.state(kIdle | kSdPresentCmdInhibit, Use::kCommand), State::kHeld);
    EXPECT_EQ(bus.state(kIdle | kSdPresentCmdInhibit, Use::kAbort), State::kHeld);
    for (uint32_t bit : {kSdPresentDatInhibit, kSdPresentDatActive, kSdPresentReadActive, kSdPresentWriteActive}) {
        EXPECT_EQ(bus.state(kIdle | bit, Use::kRead), State::kHeld);
        EXPECT_EQ(bus.state(kIdle | bit, Use::kWrite), State::kHeld);
        EXPECT_EQ(bus.state(kIdle | bit, Use::kBusy), State::kHeld);
        // The spec lets CMD-line commands and the abort out while the DAT lines are busy.
        EXPECT_EQ(bus.state(kIdle | bit, Use::kCommand), State::kFree);
        EXPECT_EQ(bus.state(kIdle | bit, Use::kAbort), State::kFree);
    }
}

TEST(SwitchSdBus, WriteReleasesTheCardOnlyWithDat0High)
{
    SwitchSdBus bus;
    SpecController hc;
    run(bus, hc, Use::kWrite);
    EXPECT_TRUE(bus.idle());
    // A controller that raised transfer complete while the card still programs: the card stays held, a data command
    // waits for its release, CMD-line commands and the releasing R1b go out.
    run(bus, hc, Use::kWrite, true);
    EXPECT_FALSE(bus.idle());
    EXPECT_EQ(bus.owed, 0u);
    EXPECT_EQ(bus.state(hc.present, Use::kWrite), State::kCardBusy);
    EXPECT_EQ(bus.state(hc.present, Use::kRead), State::kCardBusy);
    EXPECT_EQ(bus.state(hc.present, Use::kCommand), State::kFree);
    EXPECT_EQ(bus.claim(hc.present, Use::kBusy), State::kFree);
    hc.command(Use::kBusy);
    hc.respond();
    hc.release();
    hc.finish();
    bus.ended(hc.status, hc.present);
    EXPECT_TRUE(bus.idle());
    EXPECT_EQ(bus.state(hc.present, Use::kWrite), State::kFree);
}

TEST(SwitchSdBus, WriteNeedsDat0HighEvenAfterAReset)
{
    SwitchSdBus bus;
    EXPECT_EQ(bus.state(kIdle & ~kSdPresentDat0, Use::kWrite), State::kCardBusy);
    EXPECT_EQ(bus.state(kIdle & ~kSdPresentDat0, Use::kRead), State::kFree);
}

TEST(SwitchSdBus, TransferCompleteWithoutDat0KeepsTheCard)
{
    SwitchSdBus bus;
    bus.issued(Use::kAbort);
    bus.ended(kSdCommandComplete | kSdTransferComplete, kIdle & ~kSdPresentDat0);
    EXPECT_TRUE(bus.card);
    bus.ended(0, kIdle);  // DAT0 high, but no transfer complete taken since: not a release
    EXPECT_TRUE(bus.card);
}

// Random command sequences on the spec model: after each command's own interrupts the next command's single read finds
// the bus free, whatever came before, and nothing is ever owed.
TEST(SwitchSdBus, SpecControllerNeverNeedsAWait)
{
    std::mt19937 random(74);
    const Use uses[] = {Use::kCommand, Use::kRead, Use::kBusy, Use::kWrite};
    SwitchSdBus bus;
    SpecController hc;
    for (int i = 0; i < 20000; ++i) {
        const Use use = uses[random() % 4];
        run(bus, hc, use);
        ASSERT_TRUE(bus.idle()) << i;
        ASSERT_EQ(hc.present, kIdle) << i;
    }
}
}  // namespace
