// SPDX-License-Identifier: BSD-2-Clause-Patent
// Isochronous endpoints on the XUDC (SwitchXUDC.cpp "Isochronous endpoints"): the frame list IOUSBDeviceFamily hands
// over, the endpoint context's interval, which bytes of a request each frame is, and what the controller's events mean
// for the requests on an isochronous ring (IsocRing). No IOKit: test/xudc_isoch_test.cpp drives the same code.
//
// The contract (21H461: IOUSBDeviceController's isochronous io, AppleUSBXDCI's completion, AppleUSBMike as a client):
// a request is a buffer and a list of frames; frame i's bytes follow frame i - 1's in the buffer, one frame per service
// interval. At completion each frame says what happened: status (0, or kIOReturnError for a frame whose transfer
// failed), the bytes moved (IN: its request count; OUT: what arrived) and the time of its (micro)frame, and the request
// says 0 or kIOReturnError. Its frame list pointer stays where the family put it: the completion hands it back.
//
// The hardware side follows Linux tegra-xudc: one Isoch TRB per frame with Start Isoch ASAP (no frame id), the context
// interval set to bInterval as Linux sets it, ISP on OUT so a short frame raises its own event, IOC on the request's
// last frame. A frame never spans two TRBs (it fits one bounce page if it cannot go direct), so a frame is a TD of one
// TRB and its event's residue is that frame's.
#pragma once
#include <stdint.h>
#include "XudcTransfer.h"

namespace xudc {

enum : uint32_t {
    TRB_ISOCH = 5,
    TRB_SIA = 1u << 31,
    EP_ISOCH_OUT = 1,
    EP_ISOCH_IN = 5,
    COMP_RING_UNDERRUN = 14,
    COMP_RING_OVERRUN = 15,
    COMP_MISSED_SERVICE = 23,
    COMP_ISOCH_BUFFER_OVERRUN = 31,
};

constexpr uint32_t kMaxIsochEndpoints = 4;                // Linux tegra-xudc XUDC_MAX_ISOCH_EPS
constexpr int32_t kIsocFrameError = (int32_t)0xe00002bc;  // kIOReturnError

// IOUSBDeviceIsocFrame (kalloc_var element of 24 bytes in 21H461's IOUSBDeviceFamily).
struct IsocFrame {
    int32_t status;
    uint16_t requestCount, actualCount;
    uint64_t timeStamp;    // AbsoluteTime of the frame's service interval
    uint64_t frameNumber;  // the client's own count
};

static_assert(sizeof(IsocFrame) == 24, "IOUSBDeviceIsocFrame");

// AppleUSBXDCI's own filler frames carry this in timeStamp and are not stamped.
constexpr uint64_t kIsocNoStamp = 0xe0005108;

// One service interval: 2^(bInterval - 1) microframes at high speed, frames at full speed (USB 2.0 9.6.6).
constexpr uint64_t isocIntervalNs(bool fullSpeed, uint32_t bInterval)
{
    const uint32_t exponent = bInterval < 1 ? 0 : bInterval > 16 ? 15 : bInterval - 1;
    return (fullSpeed ? 1000000ull : 125000ull) << exponent;
}

// The frames, the last one serviced at `last`, each one interval before the next.
inline void isocStamp(IsocFrame *frames, uint32_t count, uint64_t last, uint64_t interval)
{
    for (uint32_t i = count; i-- > 0; last -= interval)
        if (frames[i].timeStamp != kIsocNoStamp)
            frames[i].timeStamp = last;
}

// Frame bytes [offset, offset + length) of a request whose memory is `runs` (physically contiguous, in order) as one
// TRB: straight from the client's memory when they lie in one run, cross no 64 KiB boundary, sit below `reach` and, for
// OUT, every cache line they touch is the request's own (XudcTransfer.h); else through one bounce page. False when the
// runs do not hold the frame or it is larger than a page.
inline bool planFrame(const Run *runs, uint32_t count, uint32_t offset, uint32_t length, bool out, uint64_t reach,
                      Piece *piece)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
        total += runs[i].length;
    if (length > kBouncePageBytes || (uint64_t)offset + length > total)
        return false;
    *piece = {0, offset, length, length != 0};
    if (!length)
        return true;
    for (uint32_t i = 0, start = 0; i < count; start += runs[i++].length) {
        if (offset >= start + runs[i].length)
            continue;
        const uint64_t pa = runs[i].pa + (offset - start), end = runs[i].pa + runs[i].length;
        const uint64_t lineMask = ~(uint64_t)(kCacheLine - 1);
        const bool fits = offset + length <= start + runs[i].length;
        const bool direct =
            fits && (pa >> 16) == ((pa + length - 1) >> 16) && pa + length <= reach &&
            (!out || ((pa & lineMask) >= runs[i].pa && ((pa + length + kCacheLine - 1) & lineMask) <= end));
        if (direct)
            *piece = {pa, offset, length, false};
        return true;
    }
    return false;
}

enum class IsocVerdict : uint8_t {
    Recorded,     // a frame of a request on the ring
    Stale,        // the endpoint is fenced after a cancel
    Starved,      // ring underrun or overrun: the host asked for a frame with nothing queued
    Unexplained,  // a TRB no request holds
};

// The isochronous requests on one endpoint's ring of `Slots` TRBs, frame i of a request at (first + i) % Slots. The
// controller works through them in order, so an event for request k also means the requests before it are past: their
// frames without an event moved whole (IN sent, OUT filled without a short packet). Zero-initialized is empty.
template <uint32_t Slots, uint32_t MaxRequests, uint32_t MaxFrames> class IsocRing {
public:
    enum : uint8_t {
        kPending,
        kDone,
        kFailed
    };

    struct Frame {
        uint16_t length, actual;
        uint8_t state;
    };

    struct Request {
        uint16_t first, frames;
        Frame frame[MaxFrames];
    };

    static_assert(Slots >= 4 && Slots <= 0xffff && MaxRequests > 0 && MaxFrames > 0, "ring geometry");

    uint32_t room() const
    {
        return fenced_ ? 0 : Slots - 1 - used_;
    }

    uint32_t onRing() const
    {
        return count_;
    }

    uint32_t complete() const
    {
        return complete_;
    }  // requests from the head whose every frame is known

    bool fenced() const
    {
        return fenced_;
    }

    bool busy() const
    {
        return count_ != 0;
    }

    const Request &request(uint32_t k) const
    {
        return requests_[(head_ + k) % MaxRequests];
    }

    // A request went onto the ring (the caller checked room(), onRing() < MaxRequests and frames <= MaxFrames).
    void push(uint16_t first, const uint16_t *lengths, uint16_t frames)
    {
        Request &r = requests_[(head_ + count_++) % MaxRequests];
        r.first = first;
        r.frames = frames;
        for (uint16_t i = 0; i < frames; ++i)
            r.frame[i] = {lengths[i], 0, kPending};
        used_ += frames;
    }

    IsocVerdict event(uint32_t index, uint32_t code, uint32_t residue)
    {
        if (fenced_)
            return IsocVerdict::Stale;
        if (code == COMP_RING_UNDERRUN || code == COMP_RING_OVERRUN)
            return IsocVerdict::Starved;
        for (uint32_t k = complete_; k < count_ && index < Slots; ++k) {
            Request &r = requests_[(head_ + k) % MaxRequests];
            const uint32_t at = index >= r.first ? index - r.first : index + Slots - r.first;
            if (at >= r.frames)
                continue;
            for (uint32_t j = complete_; j < k; ++j)
                settle(requests_[(head_ + j) % MaxRequests], requests_[(head_ + j) % MaxRequests].frames);
            Frame &f = r.frame[at];
            const bool moved = code == COMP_SUCCESS || code == COMP_SHORT_PACKET;
            f.state = moved && residue <= f.length ? kDone : kFailed;
            f.actual = f.state == kDone ? (uint16_t)(f.length - residue) : 0;
            settle(r, at);
            complete_ = k + (at + 1u == r.frames ? 1 : 0);
            return IsocVerdict::Recorded;
        }
        return IsocVerdict::Unexplained;
    }

    // The head leaves the ring (complete() > 0).
    void pop()
    {
        used_ -= requests_[head_].frames;
        head_ = (head_ + 1) % MaxRequests;
        --count_;
        --complete_;
    }

    // Everything off the ring (the caller has stopped the controller on it). Returns whether the endpoint is fenced.
    bool cancel()
    {
        if (count_)
            fenced_ = true;
        head_ = count_ = used_ = complete_ = 0;
        return fenced_;
    }

    void liftFence()
    {
        fenced_ = false;
    }

private:
    // Frames before `upTo` that raised no event moved whole.
    static void settle(Request &r, uint32_t upTo)
    {
        for (uint32_t i = 0; i < upTo; ++i)
            if (r.frame[i].state == kPending)
                r.frame[i] = {r.frame[i].length, r.frame[i].length, kDone};
    }

    Request requests_[MaxRequests];
    uint32_t head_, count_, used_, complete_;
    bool fenced_;
};

}  // namespace xudc
