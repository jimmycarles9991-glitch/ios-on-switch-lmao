// A model XUDC and SwitchXUDCData.cpp's data path in miniature, for xudc_completion_test (which request each transfer
// event is for) and xudc_interrupt_test (which events raise an interrupt). See xudc_completion_test.cpp for what the
// controller and the driver model do.
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "../XudcTransfer.h"

namespace xudc_model {

using xudc::Decision;
using xudc::Outcome;
using xudc::Verdict;

constexpr uint32_t kMps = 512;
constexpr uint32_t kSerialBytes = 65536;

struct Rng {
    std::mt19937_64 engine;

    explicit Rng(uint64_t seed) : engine(seed)
    {}

    uint64_t below(uint64_t n)
    {
        return std::uniform_int_distribution<uint64_t>(0, n - 1)(engine);
    }

    uint64_t range(uint64_t lo, uint64_t hi)
    {
        return std::uniform_int_distribution<uint64_t>(lo, hi)(engine);
    }

    bool chance(double p)
    {
        return std::uniform_real_distribution<double>(0.0, 1.0)(engine) < p;
    }
};

struct Ev {
    uint32_t dci, index, code, residue;
    bool blocking = false;  // from a TRB with BEI, not an error (xHCI 4.17.5): written without an interrupt
};

// The event ring as the driver reads it: in order; one slot stays empty, as the xHCI producer counts it. An event
// that finds it full is dropped (counted): the hardware's overflow.
struct EventRing {
    explicit EventRing(size_t slots) : capacity(slots)
    {}

    size_t capacity;
    std::deque<Ev> fifo;
    size_t deepest = 0;
    uint64_t dropped = 0;
    bool pending = false;  // IPE: a non-blocking event went in since the ring was last emptied

    void raise(const Ev &e)
    {
        if (fifo.size() + 1 >= capacity) {
            ++dropped;
            return;
        }
        fifo.push_back(e);
        deepest = std::max(deepest, fifo.size());
        pending |= !e.blocking;
    }
};

// What the controller does on a TD's last TRB after a short packet ended the TD before it.
enum class Second {
    None,
    ShortCode,
    SuccessCode
};

struct Behaviour {
    Second second;  // the second event
};

template <uint32_t Slots> struct Wire {
    struct Trb {
        uint32_t length, flags;
        uint64_t request;  // which request's TD this TRB belongs to (the check's bookkeeping, not the hardware's)
    };

    Trb trb[Slots] = {};
    uint32_t link = xudc::trbType(xudc::TRB_LINK) | xudc::TRB_TOGGLE;  // after TRB Slots - 1
    uint32_t cycle = 1;
    bool legacyLink = false;  // the link as Hardware::pushData wrote it before linkControl: cycle only, never CHAIN
    uint32_t enqueue = 0;     // the driver's producer position (Hardware::pushData)
    uint32_t dequeue = 0;     // the next TRB the controller reads
    uint32_t owned = 0;       // TRBs written that the controller has not passed yet
    uint64_t overwrites = 0;  // TRBs written over ones the controller had not passed (a driver bug)

    uint32_t push(uint32_t length, uint32_t flags, uint64_t request)
    {
        if (owned >= Slots)
            ++overwrites;
        const uint32_t index = enqueue;
        trb[index] = {length, flags, request};
        enqueue = (enqueue + 1) % Slots;
        if (!enqueue) {
            link = legacyLink ? (link & ~xudc::TRB_CYCLE) | cycle : xudc::linkControl(link, flags, cycle);
            cycle ^= 1;
        }
        ++owned;
        return index;
    }
};

// The TD the controller is on (or skipping the rest of).
struct Progress {
    bool active;
    uint32_t first, trbs, j, fill, got, total;
    uint64_t request;
};

template <uint32_t Slots> struct Controller {
    uint32_t dci = 0;
    bool in = false;
    Behaviour how = {};
    Wire<Slots> wire;
    EventRing *events = nullptr;
    Progress cur = {}, skip = {};
    bool skipping = false;
    std::map<uint64_t, uint64_t> delivered;  // bytes the controller moved per request
    uint64_t babble = 0;  // a packet that did not fit its TRB (the layout's whole-packet rule broken)

    static uint32_t at(uint32_t first, uint32_t i)
    {
        return (first + i) % Slots;
    }

    bool blocks(uint32_t index) const
    {
        return (wire.trb[index].flags & xudc::TRB_BEI) != 0;
    }

    bool load()
    {
        if (cur.active)
            return true;
        if (skipping || !wire.owned)
            return false;
        cur = {true, wire.dequeue, 0, 0, 0, 0, 0, wire.trb[wire.dequeue].request};
        for (uint32_t i = 0;; ++i) {
            const uint32_t index = at(cur.first, i);
            const auto &t = wire.trb[index];
            cur.total += t.length;
            const bool linkEnds = index == Slots - 1 && !(wire.link & xudc::TRB_CHAIN);
            if (!(t.flags & xudc::TRB_CHAIN) || linkEnds) {
                cur.trbs = i + 1;
                break;
            }
        }
        return true;
    }

    void pass(const Progress &p)
    {
        wire.dequeue = at(p.first, p.trbs);
        wire.owned -= p.trbs;
    }

    // OUT: the host's next packet (0..mps bytes). False: no TD to take it (the controller NAKs; the host retries).
    bool packet(uint32_t bytes)
    {
        if (!load())
            return false;
        const auto &t = wire.trb[at(cur.first, cur.j)];
        if (bytes > t.length - cur.fill) {
            ++babble;
            bytes = t.length - cur.fill;
        }
        cur.fill += bytes;
        cur.got += bytes;
        delivered[cur.request] += bytes;
        if (bytes < kMps) {  // short (or zero-length): the TD ends here
            const uint32_t residue = t.length - cur.fill;
            events->raise({dci, at(cur.first, cur.j), xudc::COMP_SHORT_PACKET, residue, blocks(at(cur.first, cur.j))});
            if (cur.j + 1 < cur.trbs) {
                skipping = true;
                skip = cur;
            } else {
                pass(cur);
            }
            cur.active = false;
            return true;
        }
        if (cur.fill == t.length) {
            if (t.flags & xudc::TRB_IOC)
                events->raise({dci, at(cur.first, cur.j), xudc::COMP_SUCCESS, 0, blocks(at(cur.first, cur.j))});
            if (cur.j + 1 == cur.trbs) {
                pass(cur);
                cur.active = false;
            } else {
                ++cur.j;
                cur.fill = 0;
            }
        }
        return true;
    }

    // The rest of a TD after its short packet: walked, its second event if this controller raises one.
    bool finishSkip()
    {
        if (!skipping)
            return false;
        const uint32_t last = at(skip.first, skip.trbs - 1);
        if (how.second == Second::ShortCode)
            events->raise({dci, last, xudc::COMP_SHORT_PACKET, wire.trb[last].length, blocks(last)});
        if (how.second == Second::SuccessCode)
            events->raise({dci, last, xudc::COMP_SUCCESS, 0, blocks(last)});
        pass(skip);
        skipping = false;
        return true;
    }

    // IN: the next TD, all of it.
    bool sendIn()
    {
        if (!load())
            return false;
        delivered[cur.request] += cur.total;
        const uint32_t last = at(cur.first, cur.trbs - 1);
        if (wire.trb[last].flags & xudc::TRB_IOC)
            events->raise({dci, last, xudc::COMP_SUCCESS, 0, blocks(last)});
        pass(cur);
        cur.active = false;
        return true;
    }

    // Hardware::dropQueued: paused, the dequeue moved to the enqueue position; what was in progress is gone.
    void drop()
    {
        cur.active = false;
        skipping = false;
        wire.dequeue = wire.enqueue;
        wire.owned = 0;
    }
};

struct Layout {
    std::vector<xudc::Run> runs;
    uint32_t length;
};

// What AppleUSBDeviceMux hands the controller: a read or a write over mbufs: 2 KiB clusters, 4 KiB pages, 16 KiB jumbo
// clusters, or one wired buffer; anywhere below 4 GiB (whole packets: reads are multiples of the packet size).
inline Layout mbufs(Rng &rng, uint32_t length)
{
    Layout l{{}, length};
    const uint32_t chunk = static_cast<uint32_t>(std::vector<uint64_t>{2048, 4096, 16384, 65536}[rng.below(4)]);
    for (uint32_t off = 0; off < length; off += chunk) {
        const uint32_t take = std::min(chunk, length - off);
        l.runs.push_back({0x8000'0000 + rng.below(1u << 16) * 65536, take});
    }
    return l;
}

// AppleUSBDeviceMux's reads without ScatterGatherSupport: one of its 32 KiB buffers from the controller's
// createIOBuffer, contiguous at a 16 KiB page below 4 GiB; one in four crosses a 64 KiB boundary (two TRBs).
inline Layout muxBuffer(Rng &rng, uint32_t length)
{
    return {{{0x8000'0000 + rng.below(1u << 12) * 16384, length}}, length};
}

using ReadLayout = Layout (*)(Rng &, uint32_t);

struct Completion {
    uint64_t request;
    Outcome outcome;
    uint32_t actual;
};

struct Tally {
    uint64_t heads = 0, second = 0, stale = 0, lost = 0, unexplained = 0, serial = 0, shortEarly = 0, failed = 0;
};

// The legacy matching: the request whose TRBs hold the event completes, every one before it
// with it (OUT: failed with 0 bytes), and its TRBs are free at once.
struct LegacyMatching {};

template <uint32_t Slots, class Matching = void> struct Endpoint {
    using Tds = xudc::TdRing<Slots, 64>;
    static constexpr uint32_t kMaxPieces = Slots - 2;  // SwitchXUDC.h kMaxPieces = kDataRingTrbs - 3

    struct Req {
        uint64_t id;
        Layout layout;
        uint32_t done;
    };

    Controller<Slots> hw;
    Tds tds{};
    std::deque<Req> queue;
    bool serialBusy = false;
    // SwitchXUDCData.cpp "Interrupts": a TD another follows onto the ring in the same kick blocks its event's interrupt
    // (BEI on its last TRB); the driver sets it on IN only. drainSoon: SwitchXUDC::drainSoon.
    bool blockIn = false, blockOut = false;
    std::function<void()> drainSoon;
    uint32_t serialLength = 0;
    uint64_t nextId = 0;
    std::vector<Completion> completed;
    Tally tally;

    bool out() const
    {
        return !hw.in;
    }

    enum class Admit {
        Queued,
        Wait,
        Serial
    };

    Admit admit(Req &r)
    {
        const uint32_t room = tds.room();
        std::vector<xudc::Piece> pieces(kMaxPieces);
        xudc::PlanStats why = {};
        const auto runs = static_cast<uint32_t>(r.layout.runs.size());
        const uint32_t n = xudc::plan(r.layout.runs.data(), runs, r.layout.length, kMps, out(), 1ull << 32,
                                      pieces.data(), std::min(room, kMaxPieces), &why);
        if (!n) {
            xudc::PlanStats ignored = {};
            const bool fits = xudc::plan(r.layout.runs.data(), runs, r.layout.length, kMps, out(), 1ull << 32,
                                         pieces.data(), kMaxPieces, &ignored) != 0;
            return fits && tds.onRing() ? Admit::Wait : Admit::Serial;
        }
        const uint32_t base = (xudc::TRB_NORMAL << 10) | (out() ? xudc::TRB_ISP : 0);
        uint32_t first = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const bool last = i + 1 == n;
            const uint32_t flags = base | (last ? xudc::TRB_IOC : xudc::TRB_CHAIN);
            const uint32_t index = hw.wire.push(pieces[i].length, flags, r.id);
            if (!i)
                first = index;
        }
        tds.push({static_cast<uint16_t>(first), static_cast<uint16_t>(n), r.layout.length});
        return Admit::Queued;
    }

    void serialPiece()
    {
        Req &r = queue.front();
        serialLength = std::min(r.layout.length - r.done, kSerialBytes);
        const uint32_t index =
            hw.wire.push(serialLength, (xudc::TRB_NORMAL << 10) | xudc::TRB_ISP | xudc::TRB_IOC, r.id);
        tds.push({static_cast<uint16_t>(index), 1, serialLength});
    }

    // The held last TRB written with BEI once the next TD is queued: the model controller runs only between the
    // driver's calls, so a flag set here before kick() returns is the driver's deferred write.
    void kick()
    {
        int64_t held = -1;
        bool waiting = false;
        while (!serialBusy && !tds.fenced() && tds.onRing() < queue.size()) {
            const Admit a = admit(queue[tds.onRing()]);
            if (a == Admit::Queued) {
                const auto &t = tds.td(tds.onRing() - 1);
                if (held >= 0 && (hw.in ? blockIn : blockOut))
                    hw.wire.trb[held].flags |= xudc::TRB_BEI;
                held = Tds::at(t.first, t.trbs - 1u);
                continue;
            }
            if (a == Admit::Wait || tds.onRing()) {
                waiting = a == Admit::Wait;
                break;
            }
            ++tally.serial;
            serialBusy = true;
            queue.front().done = 0;
            serialPiece();
            break;
        }
        if (waiting && drainSoon)
            drainSoon();
    }

    void enqueue(const Layout &l)
    {
        queue.push_back({nextId++, l, 0});
        kick();
    }

    // The head is done: off the queue, the function told (it queues its next transfer), the ring refilled.
    std::function<void(Endpoint &)> onComplete;

    void completeHead(Outcome outcome, uint32_t actual)
    {
        const Req r = queue.front();
        queue.pop_front();
        completed.push_back({r.id, outcome, actual});
        if (outcome != Outcome::Done)
            ++tally.failed;
        if (onComplete)
            onComplete(*this);
        kick();
    }

    // SwitchXUDC::transferEvent.
    void event(const Ev &e)
    {
        if constexpr (std::is_same_v<Matching, LegacyMatching>)
            return legacyEvent(e);
        const Decision d = tds.decide(e.index, e.code, e.residue, hw.in, [this](uint32_t t) {
            return hw.wire.trb[t].length;
        });
        switch (d.verdict) {
        case Verdict::Head:
            break;
        case Verdict::SecondEvent:
            ++tally.second;
            tds.apply(d);
            return kick();
        case Verdict::Stale:
            ++tally.stale;
            return;
        case Verdict::Lost:
            ++tally.lost;
            return;
        case Verdict::Unexplained:
            ++tally.unexplained;
            return;
        }
        ++tally.heads;
        if (serialBusy) {
            tds.apply(d);
            Req &r = queue.front();
            const uint32_t actual = d.outcome == Outcome::Done ? d.actual : 0;
            r.done += actual;
            if (d.outcome == Outcome::Done && r.done < r.layout.length && actual == serialLength)
                return serialPiece();
            serialBusy = false;
            return completeHead(d.outcome, r.done);
        }
        if (d.armTail)
            ++tally.shortEarly;
        tds.apply(d);
        completeHead(d.outcome, d.outcome == Outcome::Done ? d.actual : 0);
    }

    void legacyEvent(const Ev &e)
    {
        uint32_t k = 0, pos = 0;
        for (; k < tds.onRing(); ++k) {
            const auto &t = tds.td(k);
            pos = (e.index + Slots - t.first) % Slots;
            if (pos < t.trbs)
                break;
        }
        if (k == tds.onRing())
            return;  // no request holds it: ignored
        const auto &t = tds.td(k);
        uint32_t actual = 0;
        if (hw.in) {
            actual = e.residue <= t.bytes ? t.bytes - e.residue : 0;
        } else {
            std::vector<uint32_t> lengths;
            for (uint32_t i = 0; i < t.trbs; ++i)
                lengths.push_back(hw.wire.trb[Tds::at(t.first, i)].length);
            const xudc::OutResult o = xudc::outEvent(lengths.data(), t.trbs, pos, e.residue);
            actual = o.failed ? 0 : o.actual;
        }
        for (uint32_t i = 0; i <= k; ++i) {
            const bool mine = i == k;
            const Outcome outcome = mine || hw.in ? Outcome::Done : Outcome::Error;
            const uint32_t moved = mine ? actual : hw.in ? tds.td(0).bytes : 0;
            tds.apply({Verdict::Head, Outcome::Done, false, 0, 0, 0});  // TRBs free at once
            completeHead(outcome, moved);
        }
    }

    // SwitchXUDC::cancel: the controller stopped, every request aborted, the ring fenced (lifted at once when the event
    // ring is empty and no drain is under way).
    void cancel(bool draining, const EventRing &events)
    {
        hw.drop();
        serialBusy = false;
        std::deque<Req> aborted;
        aborted.swap(queue);
        if (tds.cancel() && !draining && events.fifo.empty())
            tds.liftFence();
        if (drainSoon)
            drainSoon();
        for (const Req &r : aborted)
            completed.push_back({r.id, Outcome::Error, 0});
    }
};

// A request's bytes as the controller moved them, against what the driver reported.
template <class E> void checkCompletions(const E &ep, bool aborts, const char *what)
{
    SCOPED_TRACE(what);
    uint64_t expect = 0;
    for (const Completion &c : ep.completed) {
        ASSERT_EQ(c.request, expect) << "every request completes once, in order";
        ++expect;
        if (c.outcome != Outcome::Done) {
            ASSERT_TRUE(aborts) << "request " << c.request << " failed";
            continue;
        }
        const auto it = ep.hw.delivered.find(c.request);
        const uint64_t moved = it == ep.hw.delivered.end() ? 0 : it->second;
        ASSERT_EQ(c.actual, moved) << "request " << c.request << ": the length is what the controller moved";
    }
}

template <uint32_t Slots, class Matching = void> struct Session {
    using Ep = Endpoint<Slots, Matching>;
    Rng rng;
    EventRing events;
    Ep outEp, inEp;
    std::deque<uint32_t> packets;  // the host's OUT packets still to send (each <= mps; a short one ends a transfer)
    uint64_t hostBytes = 0, messages = 0, cancels = 0;
    bool draining = false;
    double cancelChance = 0;
    uint32_t readBytes;
    ReadLayout readLayout;
    size_t perEndpointDeepest = 0;
    // Interrupt-driven: a drain runs only when the event ring raises an interrupt (a non-blocking event went in) or the
    // driver asks for one (drainSoon); else whenever. stranded: events left in the event ring with no interrupt coming
    // and nothing left for the hardware to do. hostReadsIn: the host polls the IN endpoint. inStream: the device writes
    // again after each write completes (AppleUSBDeviceMux with data to send).
    bool interruptDriven = false, drainsAsked = true, hostReadsIn = true, inStream = true, stranded = false;
    bool signalled = false;
    uint64_t interrupts = 0;
    uint32_t inBurst = 1;  // writes the device queues after each one completes: 1 to inBurst, kicked together

    Session(uint64_t seed, Behaviour how, size_t eventCapacity, uint32_t reads, uint32_t depth,
            ReadLayout layout = mbufs)
        : rng(seed), events(eventCapacity), readBytes(reads), readLayout(layout)
    {
        for (Ep *ep : {&outEp, &inEp}) {
            ep->hw.dci = ep == &outEp ? 2 : 3;
            ep->hw.in = ep == &inEp;
            ep->hw.how = how;
            ep->hw.events = &events;
        }
        for (Ep *ep : {&outEp, &inEp}) {
            ep->drainSoon = [this] {
                if (drainsAsked && !draining && !events.fifo.empty())
                    signalled = true;
            };
        }
        // AppleUSBDeviceMux: a new read for each one done, a new write while it has data.
        outEp.onComplete = [this](Ep &ep) {
            if (cancelChance && rng.chance(cancelChance))
                return cancel(rng.chance(0.5) ? inEp : ep);
            ep.queue.push_back({ep.nextId++, readLayout(rng, readBytes), 0});
        };
        inEp.onComplete = [this](Ep &ep) {
            const auto writes = static_cast<uint32_t>(inBurst > 1 ? rng.range(1, inBurst) : 1);
            for (uint32_t i = 0; inStream && i < writes && ep.queue.size() < 64; ++i)  // SwitchXUDC kQueueDepth
                ep.queue.push_back({ep.nextId++, write(), 0});
        };
        for (uint32_t i = 0; i < depth; ++i) {
            outEp.enqueue(readLayout(rng, readBytes));
            inEp.enqueue(write());
        }
    }

    Layout write()
    {
        const auto length = static_cast<uint32_t>(rng.chance(0.7) ? rng.range(16, 2048) : rng.range(2048, 65536));
        Layout l{{}, length};
        for (uint32_t off = 0; off < length;) {  // mbufs at odd places: IN has no cache-line rule
            const auto take = std::min<uint32_t>(length - off, static_cast<uint32_t>(rng.range(1, 8192)));
            l.runs.push_back({0x9000'0000 + rng.below(1u << 20) * 64 + rng.below(64), take});
            off += take;
        }
        return l;
    }

    // One mux packet from the Mac: mostly TCP acknowledgements and window updates (a 16-byte mux header and a TCP
    // header), some data, now and then more than one read holds; a transfer of whole packets ends with a ZLP.
    void message()
    {
        const double r = rng.range(0, 999) / 1000.0;
        const auto length = static_cast<uint32_t>(r < 0.75   ? rng.range(36, 120)
                                                  : r < 0.90 ? rng.range(121, 4096)
                                                  : r < 0.97 ? kMps * rng.range(1, 32)
                                                             : rng.range(4097, 3 * readBytes));
        for (uint32_t left = length;; left -= kMps) {
            if (left < kMps) {
                packets.push_back(left);
                break;
            }
            packets.push_back(kMps);
        }
        hostBytes += length;
        ++messages;
    }

    void cancel(Ep &ep)
    {
        ++cancels;
        // Bytes the host sent into a TD that is gone: an abort loses them (both sides of the check drop them).
        ep.cancel(draining, events);
        for (uint32_t i = 0; i < 8; ++i)
            ep.queue.push_back({ep.nextId++, &ep == &inEp ? write() : readLayout(rng, readBytes), 0});
        ep.kick();
    }

    // One step of the hardware: a packet from the host, an IN TD to the host, or the end of a skipped TD.
    bool hardwareStep()
    {
        switch (rng.below(3)) {
        case 0:
            if (packets.empty())
                return false;
            if (!outEp.hw.packet(packets.front()))
                return false;
            packets.pop_front();
            return true;
        case 1:
            return hostReadsIn && inEp.hw.sendIn();
        default:
            return outEp.hw.finishSkip() || inEp.hw.finishSkip();
        }
    }

    size_t pendingFor(uint32_t dci) const
    {
        return static_cast<size_t>(std::count_if(events.fifo.begin(), events.fifo.end(), [dci](const Ev &e) {
            return e.dci == dci;
        }));
    }

    // SwitchXUDC::serviceEvents: until the event ring is empty, the hardware going on meanwhile; then the fences lift.
    void drain(double concurrency)
    {
        draining = true;
        perEndpointDeepest = std::max({perEndpointDeepest, pendingFor(2), pendingFor(3)});
        while (!events.fifo.empty()) {
            const Ev e = events.fifo.front();
            events.fifo.pop_front();
            (e.dci == 2 ? outEp : inEp).event(e);
            while (rng.chance(concurrency))
                hardwareStep();
        }
        draining = false;
        events.pending = false;
        const bool liftOut = outEp.tds.fenced(), liftIn = inEp.tds.fenced();
        if (liftOut)
            outEp.tds.liftFence();
        if (liftIn)
            inEp.tds.liftFence();
        if (liftOut)
            outEp.kick();
        if (liftIn)
            inEp.kick();
    }

    bool interruptDue()
    {
        const bool due = events.pending || signalled;
        interrupts += events.pending ? 1 : 0;
        signalled = false;
        return due;
    }

    // Runs the session for `count` host messages, then until everything sent has been read.
    void run(uint64_t count, double concurrency, uint32_t burst)
    {
        uint32_t idle = 0;
        while (messages < count || !packets.empty() || outEp.hw.skipping || inEp.hw.skipping || !events.fifo.empty()) {
            if (messages < count && packets.size() < 64)
                message();
            const uint32_t steps = static_cast<uint32_t>(rng.range(1, burst));
            bool moved = false;
            for (uint32_t i = 0; i < steps; ++i)
                moved |= hardwareStep();
            if (interruptDriven ? interruptDue() : rng.chance(0.5) || messages >= count)
                drain(concurrency);
            if (outEp.tally.lost || inEp.tally.lost)
                return;
            if (!interruptDriven || messages < count || moved || events.pending || signalled) {
                idle = 0;
            } else if (++idle > 4096) {
                stranded = true;
                return;
            }
        }
    }
};

template <uint32_t Slots> void checkSession(Session<Slots> &s, bool aborts)
{
    for (auto *ep : {&s.outEp, &s.inEp}) {
        SCOPED_TRACE(ep->hw.in ? "IN" : "OUT");
        EXPECT_EQ(ep->tally.lost, 0u) << "no event lost";
        EXPECT_EQ(ep->tally.unexplained, 0u) << "every event for a request or a cancel";
        EXPECT_EQ(ep->hw.wire.overwrites, 0u) << "never a TRB the controller had not passed";
        EXPECT_EQ(ep->hw.babble, 0u) << "every packet fit its TRB";
        if (!aborts)
            EXPECT_EQ(ep->tally.failed, 0u) << "no request failed";
        checkCompletions(*ep, aborts, ep->hw.in ? "IN" : "OUT");
        EXPECT_LE(s.perEndpointDeepest, Slots - 1) << "events owed per endpoint fit the event ring's sizing";
    }
    EXPECT_EQ(s.events.dropped, 0u) << "the event ring never filled";
    if (!aborts) {  // every byte the Mac sent reached a completed read, in order
        uint64_t got = 0;
        for (const Completion &c : s.outEp.completed)
            got += c.actual;
        EXPECT_EQ(got, s.hostBytes);
    }
}

inline std::string describe(const Behaviour &how)
{
    return how.second == Second::None        ? "one event per short TD"
           : how.second == Second::ShortCode ? "second event (short packet) on the last TRB"
                                             : "second event (success) on the last TRB";
}

const Behaviour kBehaviours[] = {{Second::None}, {Second::ShortCode}, {Second::SuccessCode}};

using Ring255 = xudc::TdRing<255, 64>;  // a 256-TRB ring with the driver's 64 requests
using Ring15 = xudc::TdRing<15, 8>;

// A TD's TRB lengths for decide(): ring index -> length.
struct Lengths {
    std::map<uint32_t, uint32_t> at;

    uint32_t operator()(uint32_t index) const
    {
        return at.at(index);
    }
};

template <class R> void put(R &ring, Lengths &lengths, uint32_t first, std::vector<uint32_t> trbs)
{
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < trbs.size(); ++i) {
        lengths.at[R::at(first, i)] = trbs[i];
        bytes += trbs[i];
    }
    ring.push({static_cast<uint16_t>(first), static_cast<uint16_t>(trbs.size()), bytes});
}

}  // namespace xudc_model
