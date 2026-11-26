// drivers/SwitchUSB's device-mode interrupts on the host (SwitchXUDCData.cpp "Interrupts"): an IN TD another TD follows
// onto the ring in the same kick blocks its event's interrupt (BEI), against the model XUDC of xudc_completion_test
// (xudc_model.h) with drains run only by an interrupt or the driver's drainSoon, as SwitchXUDC's event drain runs. The
// controller follows xHCI 1.2c 4.17.5: a blocked event is written, raises no interrupt, and a later non-blocking event's
// interrupt brings the drain that reads it.
//
// Checked: long usbmux sessions lose nothing and leave no event without an interrupt to read it, with fewer interrupts;
// a TD queued after the last one was issued keeps that one's interrupt; a cancel and a halt ask for the drain that
// their stopped endpoint's blocked events would otherwise wait for forever; and why OUT never blocks: a read the host's
// short message ends would wait for a reply that cannot come.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "xudc_model.h"

using namespace xudc_model;

namespace {

constexpr size_t kEvents = size_t{2} * xudc::kEventSegmentTrbs;

Session<255> &interruptDriven(Session<255> &s, bool blockIn)
{
    s.interruptDriven = true;
    s.inEp.blockIn = blockIn;
    s.inBurst = 4;
    return s;
}

// Requests queued and kicked once, so they go onto the ring together.
template <class E> void queueTogether(E &ep, const std::vector<Layout> &layouts)
{
    for (const Layout &l : layouts)
        ep.queue.push_back({ep.nextId++, l, 0});
    ep.kick();
}

// The hardware runs until it has nothing left to do, the event drain running only when an interrupt or the driver asks.
void settle(Session<255> &s)
{
    for (uint32_t idle = 0; idle < 4096;) {
        bool moved = false;
        for (int i = 0; i < 4; ++i)
            moved |= s.hardwareStep();
        if (s.interruptDue()) {
            s.drain(0.0);
            idle = 0;
            continue;
        }
        idle = moved ? 0 : idle + 1;
    }
    s.stranded = !s.events.fifo.empty();
}

bool blocked(const Session<255> &s, uint64_t request)
{
    for (const auto &trb : s.inEp.hw.wire.trb) {
        if (trb.request == request && (trb.flags & xudc::TRB_IOC) && (trb.flags & xudc::TRB_BEI))
            return true;
    }
    return false;
}

}  // namespace

// Every controller behaviour, a long usbmux session, interrupts only, the device writing in bursts of up to four:
// nothing lost, every request in order with its bytes, no event stranded; writes queued together raise one interrupt
// for the lot. (AppleUSBDeviceMux queues each write by its own io() call, so on the device a write goes onto the ring
// alone, as its last, unless it waited for room: there BEI is rare.)
TEST(XudcInterrupts, BlockedInEventsStrandNothing)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<255> plain(31, how, kEvents, 16384, 64);
        interruptDriven(plain, false).run(20000, 0.3, 24);
        checkSession(plain, false);
        EXPECT_FALSE(plain.stranded);
        Session<255> s(31, how, kEvents, 16384, 64);
        interruptDriven(s, true).run(20000, 0.3, 24);
        checkSession(s, false);
        EXPECT_FALSE(s.stranded) << "an event left with no interrupt to read it";
        EXPECT_LT(s.interrupts, plain.interrupts);
        std::printf("XUDC interrupts (%s): %llu with BEI on IN, %llu without, over %zu writes and %zu reads\n",
                    describe(how).c_str(), static_cast<unsigned long long>(s.interrupts),
                    static_cast<unsigned long long>(plain.interrupts), s.inEp.completed.size(),
                    s.outEp.completed.size());
    }
}

// A TD queued alone is the ring's last: its event interrupts. Queued later, after it was issued, the next ones never
// change it; the earlier of them blocks, the last does not.
TEST(XudcInterrupts, QueuedAfterTheLastWasIssued)
{
    Session<255> s(7, kBehaviours[1], kEvents, 16384, 0);
    interruptDriven(s, true);
    s.inStream = false;
    queueTogether(s.inEp, {s.write()});
    queueTogether(s.inEp, {s.write(), s.write()});
    EXPECT_FALSE(blocked(s, 0)) << "issued as the ring's last";
    EXPECT_TRUE(blocked(s, 1));
    EXPECT_FALSE(blocked(s, 2));
    settle(s);
    EXPECT_FALSE(s.stranded);
    ASSERT_EQ(s.inEp.completed.size(), 3u);
    checkCompletions(s.inEp, false, "IN");
}

// IN writes queued together; the host reads the first two (blocked events) and the device aborts the endpoint. The ring
// is fenced until a drain has read those events, and no interrupt is coming: the cancel asks for the drain, and the
// writes queued after it go out. Without the drain they would wait forever.
TEST(XudcInterrupts, CancelAsksForTheDrain)
{
    for (const bool asked : {true, false}) {
        SCOPED_TRACE(asked ? "drain asked" : "no drain asked");
        Session<255> s(9, kBehaviours[0], kEvents, 16384, 0);
        interruptDriven(s, true);
        s.inStream = false;
        s.drainsAsked = asked;
        queueTogether(s.inEp, {s.write(), s.write(), s.write()});
        ASSERT_TRUE(s.inEp.hw.sendIn());
        ASSERT_TRUE(s.inEp.hw.sendIn());
        ASSERT_FALSE(s.events.pending) << "both events blocked";
        s.cancel(s.inEp);
        settle(s);
        if (!asked) {
            EXPECT_TRUE(s.stranded);
            EXPECT_TRUE(s.inEp.tds.fenced()) << "fenced for good";
            continue;
        }
        EXPECT_FALSE(s.stranded);
        EXPECT_EQ(s.inEp.completed.size(), 3u + 8u) << "the aborted three and the eight queued after";
        checkCompletions(s.inEp, true, "IN");
        EXPECT_GT(s.inEp.tally.stale, 0u) << "the blocked events met the fence";
    }
}

// The host reads two of three writes queued together and stops polling; the function halts the endpoint. The two the
// controller finished have blocked events and the third will raise none: the halt asks for the drain that completes
// them.
TEST(XudcInterrupts, HaltAsksForTheDrain)
{
    for (const bool asked : {true, false}) {
        SCOPED_TRACE(asked ? "drain asked" : "no drain asked");
        Session<255> s(13, kBehaviours[0], kEvents, 16384, 0);
        interruptDriven(s, true);
        s.inStream = false;
        queueTogether(s.inEp, {s.write(), s.write(), s.write()});
        ASSERT_TRUE(s.inEp.hw.sendIn());
        ASSERT_TRUE(s.inEp.hw.sendIn());
        s.hostReadsIn = false;
        if (asked)
            s.inEp.drainSoon();  // SwitchXUDC::handleStallEndpoint after setHalt
        settle(s);
        if (!asked) {
            EXPECT_TRUE(s.stranded);
            EXPECT_TRUE(s.inEp.completed.empty()) << "two writes the host read, never completed";
            continue;
        }
        ASSERT_EQ(s.inEp.completed.size(), 2u) << "the two the host read";
        checkCompletions(s.inEp, false, "IN");
    }
}

// Why OUT never blocks: reads queued together, the host sends one short message and waits for the reply. With BEI on
// the read the message lands in, its short packet's event is blocked and nothing else is coming: the read never
// completes. The driver's rule (IN only) completes it at once.
TEST(XudcInterrupts, OutNeverBlocks)
{
    for (const bool blockOut : {false, true}) {
        SCOPED_TRACE(blockOut ? "BEI on OUT" : "no BEI on OUT");
        Session<255> s(17, kBehaviours[0], kEvents, 16384, 0);
        interruptDriven(s, true);
        s.inStream = false;
        s.outEp.blockOut = blockOut;
        queueTogether(s.outEp, {muxBuffer(s.rng, 16384), muxBuffer(s.rng, 16384), muxBuffer(s.rng, 16384)});
        s.packets.push_back(100);
        s.hostBytes = 100;
        settle(s);
        if (blockOut) {
            EXPECT_TRUE(s.stranded);
            EXPECT_TRUE(s.outEp.completed.empty()) << "the host's message never reaches the function";
            continue;
        }
        EXPECT_FALSE(s.stranded);
        ASSERT_EQ(s.outEp.completed.size(), 1u);
        EXPECT_EQ(s.outEp.completed[0].actual, 100u);
    }
}
