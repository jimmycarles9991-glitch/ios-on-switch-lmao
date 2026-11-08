// SPDX-License-Identifier: BSD-2-Clause-Patent
// How SwitchXUDC.cpp lays a request out on a transfer ring (header "Data endpoints"), what an OUT transfer event says
// it received, and which request a transfer event is for (TdRing, "Completion accounting" below). No IOKit:
// test/xudc_transfer_test.cpp and test/xudc_completion_test.cpp drive the same code on the host.
//
// A request is one TD: its physically contiguous runs (IOMemoryDescriptor::getPhysicalSegment after prepare()) become
// chained Normal TRBs pointing at the client's memory, IOC on the last only. Every TRB but the last holds whole packets
// (a multiple of the endpoint's max packet size), so no packet is ever split between two TRBs whatever the controller
// makes of TRB boundaries inside a packet (Linux tegra-xudc never gives it one; an mbuf chain from AppleUSBDeviceMux
// would). A packet that cannot go straight to the client's memory goes through a 4 KiB page of the driver's bounce pool
// instead, packets in a run sharing a page: one that crosses a physical discontinuity, a 64 KiB boundary (no TRB may),
// or 4 GiB (the XUDC is 32-bit here, as upstream Linux treats it on T210); and for OUT, one with a 64-byte cache line
// that reaches past its run (a run's start or end off a line, under that packet or the one beside a packet across a
// break): the rest of that line is someone else's memory, and their dirty copy of it, written back by the clean and
// invalidate after the transfer, would carry the line's old bytes over what the controller wrote (inside a run, a line
// two packets share is all this transfer's). A buffer of whole pages below 4 GiB, what large I/O is, goes entirely
// direct.
//
// OUT residue. Linux tegra-xudc takes a TD's actual length as its length minus the transfer event's residue, which
// holds only if the controller reports what is left of the whole TD (its context keeps EDTLA, the bytes of the TD so
// far); xHCI hosts report what is left of the event's TRB. Linux never builds a multi-TRB OUT TD on this controller in
// practice, so which it is has not been seen. The driver learns it: until known, a multi-TRB OUT TD's first TRB is one
// packet with IOC, whose event (full: residue 0 or the rest of the TD; short: below one packet or above the rest) can
// only mean one of the two, and then each OUT event is read the one way. A request too small to probe (under two
// packets) goes through one bounce page as one TRB; an event that fits neither marks the residue broken, the request
// fails rather than report a length that may be wrong, and the endpoint takes OUT requests one TRB at a time from then.
#pragma once
#include <stdint.h>

namespace xudc {

enum : uint32_t {
    // TRB types and flags (xHCI layout).
    TRB_NORMAL = 1,
    TRB_DATA = 3,
    TRB_STATUS = 4,
    TRB_LINK = 6,
    TRB_TRANSFER_EVENT = 32,
    TRB_PORT_EVENT = 34,
    TRB_SETUP_EVENT = 63,
    TRB_CYCLE = 1u << 0,
    TRB_TOGGLE = 1u << 1,
    TRB_ISP = 1u << 2,
    TRB_CHAIN = 1u << 4,
    TRB_IOC = 1u << 5,
    TRB_DIR_IN = 1u << 16,
    // Completion codes (Linux tegra-xudc TRB_CMPL_CODE_*).
    COMP_SUCCESS = 1,
    COMP_STALL = 6,
    COMP_SHORT_PACKET = 13,
    COMP_EVENT_RING_FULL = 21,
    // Endpoint context types.
    EP_BULK_OUT = 2,
    EP_INTR_OUT = 3,
    EP_CONTROL = 4,
    EP_BULK_IN = 6,
    EP_INTR_IN = 7,
};

constexpr uint32_t trbType(uint32_t type)
{
    return type << 10;
}

// A ring's link TRB as the producer passes it: the cycle bit hands it to the controller, CHAIN is the TRB's before it
// (Linux xhci inc_enq). xHCI ends a TD at a link TRB without CHAIN, so a TD that wraps would become two: an OUT short
// packet before the link completes the request, and the host's next transfer lands in the TRBs after the link, in that
// request's memory, its event taken for the TD's second.
constexpr uint32_t linkControl(uint32_t link, uint32_t previous, uint32_t cycle)
{
    return (link & ~(TRB_CYCLE | TRB_CHAIN)) | (previous & TRB_CHAIN) | (cycle & TRB_CYCLE);
}

constexpr uint32_t kRingTrbs = 16;  // EP0's ring (in the control area); the last one is the link TRB
constexpr uint32_t kDataRingTrbs =
    256;  // a data endpoint's ring, one 4 KiB page of its own (SwitchXUDC "Data endpoints"); the last is the link TRB
constexpr uint32_t kMaxDci = 32;
// The event ring: two segments of this many events (Linux tegra-xudc's XUDC_EVENT_RING_SIZE). It holds every event the
// controller can owe at once, so it never overflows: each TRB on a ring raises at most one event (the ISP event of the
// short packet that ends its TD, or its IOC event), a data ring holds at most kDataRingTrbs - 2 TRBs of TDs and
// reserved tails (TdRing::room), a cancel's stale events stand in for TRBs that were on the ring (nothing goes on a
// fenced ring), EP0 has one TD at a time; the rest is setup and port events, paced by the host through EP0 (256 left
// for those). A ring sized for one TD per endpoint (64 events) fills: 64 TDs per endpoint can complete in one burst
// while the drain waits, and an event the controller cannot write is lost.
constexpr uint32_t kEventSegmentTrbs = 4096;
static_assert((kMaxDci - 2) * (kDataRingTrbs - 2) + (kRingTrbs - 1) + 256 < 2 * kEventSegmentTrbs,
              "the event ring holds every event the rings can owe");

constexpr uint32_t kTrbMaxBytes = 65536;  // a TRB's length field and the 64 KiB boundary no TRB may cross
constexpr uint32_t kBouncePageBytes = 4096;
constexpr uint32_t kCacheLine = 64;

struct Run {  // a physically contiguous part of the request, in request order
    uint64_t pa;
    uint32_t length;
};

struct Piece {        // one TRB
    uint64_t pa;      // the client's memory; unused for a bounce piece (the caller gives it a page)
    uint32_t offset;  // in the request
    uint32_t length;
    bool bounce;
};

enum : uint8_t {
    kResidueUnknown,
    kResiduePerTrb,
    kResiduePerTd,
    kResidueBroken
};

struct PlanStats {
    uint32_t bounceBytes, discontinuity, boundary, high, edge;  // bounced bytes; packets bounced by reason
};

// Lays out a request of `total` bytes in `runs` (covering it in order) for an endpoint of max packet `mps`: pieces in
// request order, at most `max`. firstPacket: the first piece is one packet alone (the OUT residue probe). reach: the
// first physical address the controller cannot use. Returns the piece count, 0 if more than `max` would be needed.
inline uint32_t plan(const Run *runs, uint32_t runCount, uint32_t total, uint32_t mps, bool out, bool firstPacket,
                     uint64_t reach, Piece *pieces, uint32_t max, PlanStats *stats)
{
    if (!mps)
        return 0;
    const uint32_t directMax = kTrbMaxBytes / mps * mps;
    const uint32_t bounceMax = kBouncePageBytes >= mps ? kBouncePageBytes / mps * mps : 0;
    if (!bounceMax)
        return 0;
    uint32_t count = 0;
    uint32_t run = 0;
    uint32_t runStart = 0;  // the run holding `offset`, and its first offset
    Piece *open = nullptr;  // the piece the next packet may join
    if (!total) {           // a zero-length transfer: one empty TRB
        if (!max)
            return 0;
        pieces[0] = {0, 0, 0, false};
        return 1;
    }
    for (uint32_t offset = 0; offset < total;) {
        const uint32_t packet = total - offset < mps ? total - offset : mps;
        while (run < runCount && offset >= runStart + runs[run].length)
            runStart += runs[run++].length;
        if (run == runCount)
            return 0;  // the runs end before the request does
        const uint64_t pa = runs[run].pa + (offset - runStart);
        const bool inRun = offset + packet <= runStart + runs[run].length;
        const bool crosses = (pa >> 16) != ((pa + packet - 1) >> 16);
        const bool high = pa + packet > reach;
        const uint64_t lineMask = ~static_cast<uint64_t>(kCacheLine - 1);
        const bool edge = out && inRun &&
                          ((pa & lineMask) < runs[run].pa ||
                           ((pa + packet + kCacheLine - 1) & lineMask) > runs[run].pa + runs[run].length);
        const bool bounce = !inRun || crosses || high || edge;
        if (bounce) {
            stats->bounceBytes += packet;
            if (!inRun) {
                ++stats->discontinuity;
            } else if (high) {
                ++stats->high;
            } else if (crosses) {
                ++stats->boundary;
            } else {
                ++stats->edge;
            }
        }
        const bool probeCut = firstPacket && offset == mps;  // the probe packet stands alone
        const bool joins = (open != nullptr) && !probeCut && open->bounce == bounce &&
                           (bounce ? open->length + packet <= bounceMax
                                   : open->pa + open->length == pa && open->length + packet <= directMax &&
                                         (pa & (kTrbMaxBytes - 1)) != 0);
        if (joins) {
            open->length += packet;
        } else {
            if (count == max)
                return 0;
            open = &pieces[count++];
            *open = {bounce ? 0 : pa, offset, packet, bounce};
        }
        offset += packet;
    }
    return count;
}

struct OutResult {
    bool done;        // the TD is complete (else: a mid-TD event, the probe's)
    bool failed;      // the residue fits neither reading: fail the request
    uint32_t actual;  // bytes received, when done
    uint8_t learned;  // kResidue*: what this event showed (kResidueUnknown: nothing)
};

// A transfer event in an OUT TD of `count` TRBs (`lengthOf(i)`: TRB i's length), at TRB `index`, with `residue`, short
// packet or not, read with the residue as known so far.
template <class LengthOf>
inline OutResult outEventAt(uint8_t known, LengthOf lengthOf, uint32_t count, uint32_t index, uint32_t residue,
                            bool shortPacket)
{
    uint32_t before = 0;
    uint32_t after = 0;
    uint32_t total = 0;
    uint32_t length = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t l = lengthOf(i);
        total += l;
        if (i < index)
            before += l;
        if (i > index)
            after += l;
        if (i == index)
            length = l;
    }
    OutResult r = {true, false, 0, kResidueUnknown};
    if (index >= count) {
        r.failed = true;
        return r;
    }
    if (index == count - 1) {  // the last TRB: both readings agree
        r.failed = residue > length;
        r.actual = r.failed ? 0 : total - residue;
        return r;
    }
    if (!shortPacket) {  // a full TRB in the middle: only the probe asks for one
        r.done = false;
        r.learned = residue == after ? kResiduePerTd : kResidueBroken;
        if (residue == 0)
            r.learned = kResiduePerTrb;
        return r;
    }
    const bool perTrb = residue <= length;
    const bool perTd = residue > after && residue <= length + after;
    uint8_t reading = known;
    if (known == kResidueUnknown) {
        reading = perTd && !perTrb ? kResiduePerTd : 0;
        if (perTrb && !perTd)
            reading = kResiduePerTrb;
    }
    if ((reading == kResiduePerTrb && perTrb) || (reading == kResiduePerTd && perTd)) {
        r.actual = reading == kResiduePerTrb ? before + length - residue : total - residue;
        if (known == kResidueUnknown)
            r.learned = reading;
        return r;
    }
    r.failed = true;
    r.learned = kResidueBroken;
    return r;
}

inline OutResult outEvent(uint8_t known, const uint32_t *lengths, uint32_t count, uint32_t index, uint32_t residue,
                          bool shortPacket)
{
    return outEventAt(
        known,
        [lengths](uint32_t i) {
            return lengths[i];
        },
        count, index, residue, shortPacket);
}

// Completion accounting: a data endpoint's requests are TDs on its ring in request order, and the controller works
// through them in that order (xHCI; Linux tegra-xudc). Every TD asks for its own event: IOC on its last TRB (and on the
// OUT probe's first, above), and ISP on every OUT TRB, so the short packet that ends an OUT TD early raises an event on
// the TRB it landed in. So an event is always for the oldest request on the ring (the head), and requests complete
// strictly in order, each from its own event, with its own length.
//
// One TD, two events. A short packet before an OUT TD's last TRB raises the Short Packet event there (ISP) and the
// controller skips the rest of the TD, reading its TRBs to find the end; an xHCI controller also raises the last TRB's
// IOC event on the way (Linux xhci-hcd's spurious success after a short transfer), and whether this one does is not
// documented (Linux tegra-xudc never builds such a TD: its OUT TDs are one TRB below 64 KiB). The head completes at the
// first event either way; its TRBs stay reserved (the tail: no new TD may be written over them while the controller may
// still read them) until the controller shows it is past them: that second event on the last TRB (absorbed, counted),
// or the first event of a later TD (it raised none). The slot room() always leaves free is the TD's last TRB, so it is
// the TRBs before it, not the second event, that need the reservation.
//
// A cancel (abort, deactivate, off the bus) takes everything off the ring while the event ring may still hold events
// for those TRBs: the endpoint is fenced. Nothing goes on its ring and every event for it is stale, until an event
// drain that ran after the cancel has emptied the event ring (SwitchXUDC.cpp serviceEvents), or at once if it was
// empty: the controller wrote those events before the cancel's pause completed.
//
// What is left can only be a lost event: an event for a later request while the head has had none (its IOC or ISP
// event never came, or the event ring overflowed). Completing that later request would fail the ones before it, their
// data lost without a trace (usbmux drops the rest of its stream after a gap); the driver panics with one line naming
// the endpoint, the TRB and the event instead. An event on a TRB no request holds loses nothing: it is counted and
// logged.
enum class Verdict : uint8_t {
    Unexplained,  // a TRB no request holds, or outside the ring
    Head,         // for the head request's TD (pos: its TRB within the TD)
    SecondEvent,  // the last TRB's event of the TD retired at a short packet before it: absorbed
    Stale,        // the endpoint is fenced: for TRBs a cancel took off the ring
    Lost,         // for request k > 0 while the head has had no event: an event was lost
};

enum class Outcome : uint8_t {
    Pending,  // the OUT probe's event: the rest of the TD is still coming
    Done,     // complete with `actual`
    Failed,   // OUT residue fits neither reading (outEvent): fail the request
    Error,    // a completion code other than success, short packet or stall
    Stall,
};

struct Decision {
    Verdict verdict;
    Outcome outcome;  // Head only
    bool armTail;     // Head, completing: the TD ended before its last TRB, so its TRBs stay reserved
    uint8_t learned;  // Head, OUT: the residue reading this event showed (kResidueUnknown: nothing)
    uint32_t actual;  // Done: bytes moved
    uint32_t k, pos;  // Head, Lost: the request (0 = the head) and the TRB within its TD the event names
};

// The TDs on one data endpoint's ring of `Slots` TRBs (the link TRB not counted; TRB i of a TD at `first` is at
// (first + i) % Slots), at most MaxTds at once, and what an event means for them (decide) and does to them (apply).
// Zero-initialized is empty and unfenced (the driver's endpoints are zeroed memory). The driver places a TD's TRBs at
// the ring's enqueue position, within room().
template <uint32_t Slots, uint32_t MaxTds> class TdRing {
public:
    struct Td {
        uint16_t first, trbs;  // ring index of the first TRB, TRBs in the TD
        uint32_t bytes;        // the request's length (the serial path: this piece's)
        bool probe;            // OUT residue probe: IOC on the first TRB too
    };

    static constexpr uint32_t kNoTrb = ~0u;  // an event TRB outside the ring
    static_assert(Slots >= 4 && Slots <= 0xffff && MaxTds > 0, "ring geometry");

    // TRBs a new TD may take: one slot always stays free (as Linux tegra-xudc keeps one), the tail is reserved, and a
    // fenced ring takes nothing.
    uint32_t room() const
    {
        return fenced_ ? 0 : Slots - 1 - used_ - tail_.trbs;
    }

    uint32_t onRing() const
    {
        return count_;
    }

    uint32_t tailTrbs() const
    {
        return tail_.trbs;
    }

    bool busy() const
    {
        return count_ || tail_.trbs;
    }  // TRBs the controller may still raise an event for

    bool fenced() const
    {
        return fenced_;
    }

    const Td &td(uint32_t k) const
    {
        return tds_[(head_ + k) % MaxTds];
    }

    static uint32_t at(uint32_t first, uint32_t i)
    {
        return (first + i) % Slots;
    }

    // A TD went onto the ring (the caller checked room() and that fewer than MaxTds are on it).
    void push(const Td &t)
    {
        tds_[(head_ + count_++) % MaxTds] = t;
        used_ += t.trbs;
    }

    // What a transfer event on ring TRB `index` (kNoTrb: outside the ring) with `code` and `residue` means; `in` is the
    // endpoint's direction, `known` the OUT residue reading so far, lengthOf(ring index) a TRB's length as written.
    template <class LengthOf>
    Decision decide(uint32_t index, uint32_t code, uint32_t residue, bool in, uint8_t known, LengthOf lengthOf) const
    {
        Decision d = {Verdict::Unexplained, Outcome::Pending, false, kResidueUnknown, 0, 0, 0};
        if (fenced_) {
            d.verdict = Verdict::Stale;
            return d;
        }
        if (index >= Slots)
            return d;
        if (tail_.trbs && index == at(tail_.first, tail_.trbs - 1u)) {
            d.verdict = Verdict::SecondEvent;
            return d;
        }
        uint32_t k = 0;
        uint32_t pos = 0;
        for (; k < count_; ++k) {
            const Td &t = td(k);
            pos = index >= t.first ? index - t.first : index + Slots - t.first;
            if (pos < t.trbs)
                break;
        }
        if (k == count_)
            return d;  // not on the ring (the tail's other TRBs, a free slot)
        d.k = k;
        d.pos = pos;
        if (k) {
            d.verdict = Verdict::Lost;
            return d;
        }
        const Td &t = td(0);
        const bool last = pos + 1u == t.trbs;
        const bool shortPacket = code == COMP_SHORT_PACKET;
        if (code != COMP_SUCCESS && !shortPacket) {
            d.verdict = Verdict::Head;
            d.outcome = code == COMP_STALL ? Outcome::Stall : Outcome::Error;
            d.armTail = !last;
            return d;
        }
        if (!last && !shortPacket && (in || !t.probe || pos != 0))
            return d;  // a full TRB nothing asked an event for
        d.verdict = Verdict::Head;
        if (in) {
            d.outcome = Outcome::Done;
            d.actual = residue <= t.bytes ? t.bytes - residue : 0;
            d.armTail = !last;
            return d;
        }
        const OutResult o = outEventAt(
            known,
            [&](uint32_t i) {
                return lengthOf(at(t.first, i));
            },
            t.trbs, pos, residue, shortPacket);
        d.learned = o.learned;
        if (!o.done)
            return d;  // the probe's packet: Pending
        d.outcome = o.failed ? Outcome::Failed : Outcome::Done;
        d.actual = o.failed ? 0 : o.actual;
        d.armTail = !last;
        return d;
    }

    // What decide() found, done: the second event frees the tail; any event of the head shows the controller past the
    // tail; a completing head leaves the ring (its TRBs reserved as the new tail if it ended early).
    void apply(const Decision &d)
    {
        if (d.verdict == Verdict::SecondEvent) {
            tail_ = Td{};
            return;
        }
        if (d.verdict != Verdict::Head)
            return;
        tail_ = Td{};
        if (d.outcome == Outcome::Pending)
            return;
        const Td t = td(0);
        head_ = (head_ + 1) % MaxTds;
        --count_;
        used_ -= t.trbs;
        if (d.armTail)
            tail_ = t;
    }

    // Everything off the ring (the caller has stopped the controller on it). Returns whether the endpoint is fenced:
    // something was on it, so the event ring may hold events for it.
    bool cancel()
    {
        if (busy())
            fenced_ = true;
        head_ = count_ = used_ = 0;
        tail_ = Td{};
        return fenced_;
    }

    void liftFence()
    {
        fenced_ = false;
    }

private:
    Td tds_[MaxTds];
    uint32_t head_, count_, used_;
    Td tail_;
    bool fenced_;
};

}  // namespace xudc
