// drivers/SwitchUSB's device-mode completion accounting (XudcTransfer.h "Completion accounting", TdRing) on the host,
// against a model XUDC: which request each transfer event is for, and that every request completes exactly once, in
// order, with the bytes the controller put in it.
//
// The model controller works through the TDs on a transfer ring the way xHCI and Linux tegra-xudc describe it: TRBs in
// ring order, a TD ends at its TRB without CHAIN (or, as xHCI reads it, at a link TRB without CHAIN); OUT packets fill
// whole-packet TRBs; an IOC TRB that fills raises an event; a short packet (or a zero-length one) ends the TD with an
// event on the TRB it landed in (ISP), the residue that TRB's, and the controller then skips the rest of
// the TD, a while later, reading those TRBs as it goes, and may raise a second event on the TD's last TRB (IOC) when
// it gets there. Events go through one FIFO of a fixed size.
//
// The model driver is SwitchXUDCData.cpp's data path in miniature: requests laid out by XudcTransfer.h plan() from
// mbuf-like scatter-gather onto the ring within TdRing::room(), events matched by TdRing::decide/apply, the next
// request queued from each completion (as AppleUSBDeviceMux does), the serial path when only a reserved tail holds the
// room, a cancel fencing the ring until a drain has emptied the event ring. The model also checks the controller's
// side: the driver never writes a TRB the controller has not passed yet.
//
// LegacyMatching runs on the same model: an event completes the request whose TRBs hold it and fails every OUT request
// before it with 0 bytes, and the TRBs free at once. Second events alone cost it nothing (its one free slot is always
// the short TD's last TRB) but it rewrites TRBs the controller is still skipping, and a 64-event ring overflows under a
// burst, which it turns into silently failed reads.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "xudc_model.h"

using namespace xudc_model;

TEST(XudcCompletion, ShortPacketOneEvent)
{
    Ring255 ring{};
    Lengths len;
    put(ring, len, 0, {512, 2048, 2048});  // an OUT read of three TRBs; 100 bytes arrive
    EXPECT_EQ(ring.room(), 254u - 3);
    Decision d = ring.decide(0, xudc::COMP_SHORT_PACKET, 412, false, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.outcome, Outcome::Done);
    EXPECT_EQ(d.actual, 100u);
    EXPECT_TRUE(d.armTail);
    ring.apply(d);
    EXPECT_EQ(ring.onRing(), 0u);
    EXPECT_EQ(ring.tailTrbs(), 3u) << "the TD's TRBs stay reserved";
    EXPECT_EQ(ring.room(), 254u - 3);
    put(ring, len, 3, {2048, 2048});  // the next read; this controller raises no second event
    d = ring.decide(4, xudc::COMP_SUCCESS, 0, false, len);
    ASSERT_EQ(d.verdict, Verdict::Head) << "the next TD's own event";
    EXPECT_EQ(d.actual, 4096u);
    EXPECT_FALSE(d.armTail);
    ring.apply(d);
    EXPECT_EQ(ring.tailTrbs(), 0u) << "the controller is past the tail";
    EXPECT_EQ(ring.room(), 254u);
}

TEST(XudcCompletion, ShortPacketTwoEvents)
{
    for (const uint32_t code : {xudc::COMP_SHORT_PACKET, xudc::COMP_SUCCESS}) {
        Ring255 ring{};
        Lengths len;
        put(ring, len, 10, {512, 2048, 2048});
        put(ring, len, 13, {512, 2048});
        Decision d = ring.decide(10, xudc::COMP_SHORT_PACKET, 412, false, len);  // 100 bytes
        ASSERT_EQ(d.verdict, Verdict::Head);
        EXPECT_EQ(d.actual, 100u);
        ring.apply(d);
        d = ring.decide(12, code, 2048, false, len);  // the last TRB's event, nothing in it
        EXPECT_EQ(d.verdict, Verdict::SecondEvent) << "absorbed, whatever its code";
        ring.apply(d);
        EXPECT_EQ(ring.tailTrbs(), 0u);
        EXPECT_EQ(ring.onRing(), 1u) << "the next read untouched";
        d = ring.decide(14, xudc::COMP_SHORT_PACKET, 2048, false, len);  // a ZLP after 512
        ASSERT_EQ(d.verdict, Verdict::Head);
        EXPECT_EQ(d.actual, 512u);
        EXPECT_FALSE(d.armTail) << "short on its last TRB: one event";
    }
}

TEST(XudcCompletion, MultiTrbTd)
{
    Ring255 ring{};
    Lengths len;
    put(ring, len, 0, {4096, 4096, 4096, 1024});
    Decision d = ring.decide(1, xudc::COMP_SUCCESS, 0, false, len);
    EXPECT_EQ(d.verdict, Verdict::Unexplained) << "a full TRB without IOC: nothing asked for it";
    d = ring.decide(3, xudc::COMP_SUCCESS, 0, false, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 13312u);
    d = ring.decide(3, xudc::COMP_SUCCESS, 0, true, len);  // IN
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 13312u);
    d = ring.decide(2, xudc::COMP_SHORT_PACKET, 4096 - 7, false, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 8192u + 7);
    EXPECT_TRUE(d.armTail);
    d = ring.decide(2, 4 /* USB transaction error */, 0, false, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.outcome, Outcome::Error);
    EXPECT_TRUE(d.armTail) << "ended before its last TRB: reserved as after a short packet";
}

TEST(XudcCompletion, RingWrap)
{
    Ring15 ring{};
    Lengths len;
    put(ring, len, 12, {512, 512, 512, 512, 512});  // TRBs 12, 13, 14, 0, 1
    put(ring, len, 2, {512, 512});                  // 2, 3
    EXPECT_EQ(ring.room(), 14u - 7);
    for (const uint32_t index : {12u, 13u, 14u, 0u}) {
        const Decision d = ring.decide(index, xudc::COMP_SHORT_PACKET, 512, false, len);
        ASSERT_EQ(d.verdict, Verdict::Head) << index;
        EXPECT_EQ(d.pos, (index + 15 - 12) % 15);
    }
    Decision d = ring.decide(1, xudc::COMP_SUCCESS, 0, false, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 2560u);
    d = ring.decide(3, xudc::COMP_SUCCESS, 0, false, len);
    EXPECT_EQ(d.verdict, Verdict::Lost) << "the second TD's event while the first has had none";
    EXPECT_EQ(d.k, 1u);
    EXPECT_EQ(d.pos, 1u);
    d = ring.decide(7, xudc::COMP_SUCCESS, 0, false, len);
    EXPECT_EQ(d.verdict, Verdict::Unexplained) << "a free TRB";
    d = ring.decide(Ring15::kNoTrb, xudc::COMP_SUCCESS, 0, false, len);
    EXPECT_EQ(d.verdict, Verdict::Unexplained) << "outside the ring";
}

// The ring full of mux reads and the head ends at a short packet on its first TRB: the next read cannot go onto the
// TD's TRBs (the controller may still be skipping through them) until an event shows the controller past it. Without
// the tail, 11 would be free at once from TRB 252: 252..254 and 0..7, the TD's own; the one free slot, TRB 8, its last.
TEST(XudcCompletion, LinkCarriesTheChain)
{
    const uint32_t link = xudc::trbType(xudc::TRB_LINK) | xudc::TRB_TOGGLE;
    const uint32_t middle = xudc::trbType(xudc::TRB_NORMAL) | xudc::TRB_ISP | xudc::TRB_CHAIN;
    const uint32_t last = xudc::trbType(xudc::TRB_NORMAL) | xudc::TRB_ISP | xudc::TRB_IOC;
    EXPECT_EQ(xudc::linkControl(link, middle, 1), link | xudc::TRB_CHAIN | xudc::TRB_CYCLE) << "a TD across the link";
    EXPECT_EQ(xudc::linkControl(link | xudc::TRB_CHAIN | xudc::TRB_CYCLE, last, 0), link) << "a TD ends before it";
}

TEST(XudcCompletion, TailKeepsTheSkippedTrbs)
{
    Ring255 ring{};
    Lengths len;
    uint32_t enqueue = 0;
    std::vector<uint32_t> nine(9, 2048);
    nine[0] = 512;
    while (ring.room() >= 9) {  // 28 reads of 9 TRBs: 252 of 254
        put(ring, len, enqueue, nine);
        enqueue = Ring255::at(enqueue, 9);
    }
    ASSERT_EQ(ring.onRing(), 28u);
    Decision d = ring.decide(0, xudc::COMP_SHORT_PACKET, 512 - 60, false, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 60u);
    ring.apply(d);
    EXPECT_EQ(ring.room(), 2u) << "TRBs 252 and 253 only";
    d = ring.decide(8, xudc::COMP_SUCCESS, 0, false, len);
    EXPECT_EQ(d.verdict, Verdict::SecondEvent);
    ring.apply(d);
    EXPECT_EQ(ring.room(), 11u) << "past it: the TD's TRBs are free";
}

TEST(XudcCompletion, CancelFencesUntilTheEventsAreRead)
{
    Ring255 ring{};
    Lengths len;
    put(ring, len, 40, {2048, 2048});
    put(ring, len, 42, {2048});
    EXPECT_TRUE(ring.cancel());
    EXPECT_TRUE(ring.fenced());
    EXPECT_EQ(ring.room(), 0u) << "nothing goes on a fenced ring";
    EXPECT_EQ(ring.onRing(), 0u);
    for (const uint32_t index : {41u, 42u, 7u}) {
        const Decision d = ring.decide(index, xudc::COMP_SUCCESS, 0, false, len);
        EXPECT_EQ(d.verdict, Verdict::Stale) << index;
    }
    ring.liftFence();
    EXPECT_EQ(ring.room(), 254u);
    EXPECT_FALSE(ring.cancel()) << "nothing on the ring: no fence";
    Ring255 tailOnly{};
    put(tailOnly, len, 0, {512, 2048});
    tailOnly.apply(tailOnly.decide(0, xudc::COMP_SHORT_PACKET, 500, false, len));
    EXPECT_TRUE(tailOnly.cancel()) << "a tail may still raise its second event";
}

// A long usbmux-like session per controller behaviour: 16 KiB mux reads over 2 KiB clusters (9 TRBs or so, 64 of them
// queued: the ring stays full), writes of any size, the hardware racing the drain.
TEST(XudcSession, UsbmuxStreamNoLoss)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<255> s(11, how, size_t{2} * xudc::kEventSegmentTrbs, 16384, 64);
        s.run(25000, 0.3, 24);
        checkSession(s, false);
        EXPECT_GT(s.outEp.tally.shortEarly, 10000u) << "TDs ended before their last TRB";
        if (how.second != Second::None) {
            EXPECT_EQ(s.outEp.tally.second, s.outEp.tally.shortEarly) << "each one's second event absorbed";
        } else {
            EXPECT_EQ(s.outEp.tally.second, 0u);
        }
        std::printf("XUDC session (%s): %llu messages, %llu bytes; OUT %zu reads (%llu ended early, %llu second "
                    "events, %llu serial), IN %zu writes; event ring at most %zu deep\n",
                    describe(how).c_str(), static_cast<unsigned long long>(s.messages),
                    static_cast<unsigned long long>(s.hostBytes), s.outEp.completed.size(),
                    static_cast<unsigned long long>(s.outEp.tally.shortEarly),
                    static_cast<unsigned long long>(s.outEp.tally.second),
                    static_cast<unsigned long long>(s.outEp.tally.serial), s.inEp.completed.size(), s.events.deepest);
    }
}

// The driver's ring (kDataRingTrbs, 1023 slots) under the same stream: what the event ring's TD-based bound in
// SwitchXUDC.h assumes, at most 2 events a TD and 64 TDs on a ring, holds; nothing lost.
TEST(XudcSession, DriversRingSize)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<xudc::kDataRingTrbs - 1> s(29, how, size_t{2} * xudc::kEventSegmentTrbs, 16384, 64);
        s.run(10000, 0.3, 24);
        checkSession(s, false);
        EXPECT_LE(s.perEndpointDeepest, 2u * 64 + 1) << "events one endpoint owed at once";
    }
}

// A ring of 15 TRBs: it wraps every request or two, a read of 2 KiB clusters (8 TRBs) and its tail do not fit at once,
// so a read with nothing on the ring beside a tail takes the serial path.
TEST(XudcSession, SmallRingWraps)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<15> s(5, how, size_t{2} * xudc::kEventSegmentTrbs, 16384, 8);
        s.run(20000, 0.5, 8);
        checkSession(s, false);
        EXPECT_GT(s.outEp.tally.serial, 0u) << "a tail blocked a read with nothing on the ring";
    }
}

// AppleUSBDeviceMux's reads as on the device (32 KiB buffers, two queued, a quarter of them two TRBs) on a controller
// that raises second events. With the link written without CHAIN, a read across the link that a short packet ends
// before it leaves its last TRB to the controller as a TD of its own: the host's next transfer goes there, into a read
// already completed, and its event passes for the read's second. With CHAIN carried: no loss.
TEST(XudcSession, ReadAcrossTheLinkStaysOneTd)
{
    for (const bool legacy : {true, false}) {
        SCOPED_TRACE(legacy ? "link without CHAIN" : "link with the TD's CHAIN");
        Session<255> s(19, {Second::ShortCode}, size_t{2} * xudc::kEventSegmentTrbs, 32768, 2, muxBuffer);
        ASSERT_EQ(s.outEp.hw.wire.cycle, 1u) << "no wrap yet";
        s.outEp.hw.wire.legacyLink = s.inEp.hw.wire.legacyLink = legacy;
        s.run(5000, 0.3, 24);
        uint64_t got = 0;
        uint64_t wrong = 0;
        for (const Completion &c : s.outEp.completed) {
            got += c.actual;
            const auto it = s.outEp.hw.delivered.find(c.request);
            wrong += c.actual != (it == s.outEp.hw.delivered.end() ? 0 : it->second) ? 1 : 0;
        }
        if (!legacy) {
            checkSession(s, false);
            continue;
        }
        EXPECT_GT(wrong, 0u) << "host bytes in reads already completed";
        EXPECT_LT(got, s.hostBytes);
        EXPECT_EQ(s.outEp.tally.second, s.outEp.tally.shortEarly) << "the lost transfer's event passes for a second";
        checkCompletions(s.inEp, false, "IN");
        std::printf("XUDC mux reads, link without CHAIN: %llu of %zu reads given bytes after they completed, %llu of "
                    "%llu host bytes lost\n",
                    static_cast<unsigned long long>(wrong), s.outEp.completed.size(),
                    static_cast<unsigned long long>(s.hostBytes - got), static_cast<unsigned long long>(s.hostBytes));
    }
}

// Aborts at random, from completions inside the drain too: stale events dropped, nothing lost or reused early.
TEST(XudcSession, CancelsMidStream)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<255> s(23, how, size_t{2} * xudc::kEventSegmentTrbs, 16384, 32);
        s.cancelChance = 0.002;
        s.run(20000, 0.3, 24);
        checkSession(s, true);
        EXPECT_GT(s.cancels, 5u);
        EXPECT_GT(s.outEp.tally.stale + s.inEp.tally.stale, 0u) << "stale events met a fence";
    }
}

// An event ring that does overflow (64 events, the hardware running far ahead of the drain), on a controller
// raising one event per TD: the first event lost shows as a Lost verdict (the driver's panic), and every request
// completed before it is right: never a silent loss. (With second events a lost short-packet event could pass for its
// TD's own: the event ring is sized so it cannot overflow, kEventSegmentTrbs, and the controller's ring-full report
// panics.)
TEST(XudcSession, OverflowIsNeverSilent)
{
    Session<255> s(3, {Second::None}, 64, 16384, 64);
    s.run(40000, 0.0, 400);
    EXPECT_GT(s.events.dropped, 0u);
    EXPECT_GT(s.outEp.tally.lost + s.inEp.tally.lost, 0u) << "the drop was seen";
    EXPECT_EQ(s.outEp.tally.failed + s.inEp.tally.failed, 0u);
    checkCompletions(s.outEp, false, "OUT");
    checkCompletions(s.inEp, false, "IN");
}

namespace {
struct LegacyResult {
    uint64_t failed, wrong, overwrites, dropped;
};

LegacyResult legacy(Behaviour how, size_t eventCapacity, double concurrency, uint32_t burst)
{
    Session<255, LegacyMatching> s(11, how, eventCapacity, 16384, 64);
    s.run(25000, concurrency, burst);
    uint64_t wrong = 0;
    for (const Completion &c : s.outEp.completed) {
        const auto it = s.outEp.hw.delivered.find(c.request);
        if (c.outcome == Outcome::Done && c.actual != (it == s.outEp.hw.delivered.end() ? 0 : it->second))
            ++wrong;
    }
    return {s.outEp.tally.failed, wrong, s.outEp.hw.wire.overwrites, s.events.dropped};
}
}  // namespace

// The same sessions with the legacy matching. Second events alone: no read lost (they find no request), but TRBs
// rewritten while the controller skips them. A 64-event ring under a burst: reads failed with 0 bytes, silently.
TEST(XudcSession, LegacyMatching)
{
    const LegacyResult second = legacy({Second::SuccessCode}, size_t{2} * xudc::kEventSegmentTrbs, 0.3, 24);
    EXPECT_EQ(second.failed + second.wrong, 0u);
    EXPECT_GT(second.overwrites, 0u);
    const LegacyResult burst = legacy({Second::None}, 64, 0.0, 400);
    EXPECT_GT(burst.dropped, 0u);
    EXPECT_GT(burst.failed, 0u) << "overflow becomes silently failed reads";
    std::printf("XUDC legacy matching, second events %llu reads failed, %llu wrong, %llu TRBs rewritten before the "
                "controller passed them; with a 64-event ring under a burst %llu events dropped, %llu reads failed "
                "with 0 bytes, %llu wrong\n",
                static_cast<unsigned long long>(second.failed), static_cast<unsigned long long>(second.wrong),
                static_cast<unsigned long long>(second.overwrites), static_cast<unsigned long long>(burst.dropped),
                static_cast<unsigned long long>(burst.failed), static_cast<unsigned long long>(burst.wrong));
}
