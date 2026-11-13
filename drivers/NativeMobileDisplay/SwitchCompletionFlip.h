// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window A flip that the producer's completion writes itself. The offer prepares it under the display's gate (the
// ring entry, its START_ADDR and layout); the completion writes it from whichever thread delivers it (the GPU's
// interrupt pass, a present work item), or the producer's interrupt filter writes it earlier still, before the
// completion is delivered; the gate puts it in the ring at the completion's own pass, which the next frame end runs
// first when it comes before. Plain state: every transition is made under the IOSimpleLock that also orders the gate's
// reads of window A's latch against the writes. Only the gate arms, so a flip it sees idle cannot be written behind
// its back.
#pragma once
#include <stdint.h>

struct SwitchCompletionFlip {
    enum : uint8_t {
        kIdle,
        kArmed,    // the completion may write it
        kWritten,  // requested by the completion; not in the ring until the completion's pass
        kEarly,    // requested by the producer's interrupt filter; its completion not delivered yet
        kAdopted,  // an early flip the gate put in the ring before its completion: that pass enters nothing
    };

    enum : uint8_t {
        kFromCompletion,
        kFromGpuFilter,
        kFromVicFilter
    };

    static constexpr uint32_t kAnyArm = 0;  // a filter that only ever sees its own offer's flip

    uint8_t state = kIdle;
    uint8_t writer = kFromCompletion;
    int entry = -1;
    uint32_t start = 0, kind = 0, stride = 0, hOffset = 0;
    uint32_t generation = 0;  // of the arm, never kAnyArm: what a filter waiter left from an earlier offer checks
    bool layout = false;
    uint64_t requestedAt = 0;  // the display's frame ends when it was written
    uint64_t writtenAt = 0;    // mach time of that write
    uint64_t deliveredAt = 0;  // mach time an early flip's completion was delivered

    void arm(int i, uint32_t address)
    {
        entry = i;
        start = address;
        layout = false;
        writer = kFromCompletion;
        generation = generation + 1 == kAnyArm ? 1 : generation + 1;
        state = kArmed;
    }

    void armLayout(uint32_t surfaceKind, uint32_t lineStride, uint32_t offset)
    {
        kind = surfaceKind;
        stride = lineStride;
        hOffset = offset;
        layout = true;
    }

    // The producer's interrupt filter, its job certainly done: true when it is to write the flip now.
    bool claimEarly(uint32_t armed, uint8_t by)
    {
        if (state != kArmed || (armed != kAnyArm && armed != generation))
            return false;
        state = kEarly;
        writer = by;
        return true;
    }

    // The completion's verdict: true when it is to write the flip now. A failed frame is never flipped to; a flip its
    // filter already wrote is the completion's own from here (completionFlipVerdict made its status 0).
    bool claim(int status)
    {
        if (state == kEarly) {
            state = kWritten;
            return false;
        }
        if (state != kArmed)
            return false;
        state = status ? kIdle : kWritten;
        return status == 0;
    }

    bool early() const
    {
        return state == kEarly || state == kAdopted;
    }

    // A refused offer, or a window A write of the gate's own that the prepared flip must not race.
    void withdraw()
    {
        if (state == kArmed)
            state = kIdle;
    }

    // presentEntry(i): true when the completion already wrote entry i (the caller puts it in the ring unless the gate
    // adopted it, and writes nothing). An early one is reached here when the completion's pass runs between its
    // gpuDonePending store and its claim. Anything still armed is withdrawn: the gate flips it itself.
    bool present(int i)
    {
        const bool written = (state == kWritten || early()) && entry == i;
        if (state == kArmed || written)
            state = kIdle;
        return written;
    }
};

// Windows B and C's set, staged by the gate with every word but the surface addresses: the completion writes those
// (stores) and the STATE_CONTROL pair, or host1x's syncpoint filter does earlier (kEarly), once every producer's job is
// certainly done. Same states and lock as SwitchCompletionFlip.
struct SwitchPlaneCompletionFlip {
    static constexpr unsigned kMaxStores = 4;  // START_ADDR and START_ADDR_U of windows B and C

    uint8_t state = SwitchCompletionFlip::kIdle;
    unsigned stores = 0;
    uint32_t word[kMaxStores] = {}, value[kMaxStores] = {};
    uint32_t update = 0, request = 0;
    uint32_t generation = 0;  // of the arm: what a filter waiter left from an earlier offer checks
    bool filtered = false;    // written by host1x's filter
    uint64_t requestedAt = 0, writtenAt = 0;

    void arm(uint32_t updateBits, uint32_t requestBits)
    {
        update = updateBits;
        request = requestBits;
        filtered = false;
        generation = generation + 1 == SwitchCompletionFlip::kAnyArm ? 1 : generation + 1;
        state = SwitchCompletionFlip::kArmed;
    }

    void clear()
    {
        stores = 0;
    }

    void store(uint32_t at, uint32_t to)
    {
        if (stores < kMaxStores) {
            word[stores] = at;
            value[stores] = to;
            ++stores;
        }
    }

    bool claimEarly(uint32_t armed)
    {
        if (state != SwitchCompletionFlip::kArmed || armed != generation)
            return false;
        state = SwitchCompletionFlip::kEarly;
        filtered = true;
        return true;
    }

    // A set its filter already wrote is the completion's own from here (completionFlipVerdict made its status 0).
    bool claim(int status)
    {
        if (state == SwitchCompletionFlip::kEarly) {
            state = SwitchCompletionFlip::kWritten;
            return false;
        }
        if (state != SwitchCompletionFlip::kArmed)
            return false;
        state = status ? SwitchCompletionFlip::kIdle : SwitchCompletionFlip::kWritten;
        return status == 0;
    }

    bool early() const
    {
        return state == SwitchCompletionFlip::kEarly || state == SwitchCompletionFlip::kAdopted;
    }

    void withdraw()
    {
        if (state == SwitchCompletionFlip::kArmed)
            state = SwitchCompletionFlip::kIdle;
    }

    // The gate's pass: true when the completion wrote it. An adopted set's request is the latch's already.
    bool take()
    {
        const bool written = state == SwitchCompletionFlip::kWritten || state == SwitchCompletionFlip::kEarly;
        if (written)
            state = SwitchCompletionFlip::kIdle;
        return written;
    }
};
