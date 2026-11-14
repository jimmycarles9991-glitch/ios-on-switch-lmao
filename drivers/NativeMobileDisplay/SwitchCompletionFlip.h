// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window A flip that the producer's completion writes itself. The offer prepares it under the display's gate (the
// ring entry, its START_ADDR and layout); the completion writes it from whichever thread delivers it (the GPU's
// interrupt pass, a present work item), or the producer's interrupt filter writes it earlier still, before the
// completion is delivered; the gate puts it in the ring at the completion's own pass, which the next frame end runs
// first when it comes before. Only the gate arms, so a flip it sees idle cannot be written behind its back.
//
// No lock: the state and the arm's generation are one word, and every transition is one store or one compare-and-swap
// on it. A writer claims an armed flip (kClaimed) before its register stores and publishes kEarly or kWritten after
// them, with interrupts off between (its stores are a few posted writes), so whoever meets kClaimed waits only for
// those. The generation in the word makes a stale claimer (a filter left from an earlier offer) fail its swap. The rest
// is written by one side at a time: the gate's arm before the word says kArmed, the claimer's times while it holds
// kClaimed; each is read after the word that published it.
#pragma once
#include <stdint.h>

// The claimers' and the gate's waits on kClaimed spin on this.
inline void switch_flip_relax()
{
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
}

// Between the gate's reads of DC A's latch and its check that no claim came meanwhile: device reads before the load.
inline void switch_flip_read_fence()
{
#if defined(__aarch64__)
    __asm__ volatile("dmb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

// One flip's state word: kIdle .. kClaimed in the low byte, the arm's generation (never 0) above it.
struct SwitchFlipWord {
    enum : uint8_t {
        kIdle,
        kArmed,    // the completion (or a filter) may write it
        kWritten,  // requested by the completion; not in the ring until the completion's pass
        kEarly,    // requested by the producer's interrupt filter; its completion not delivered yet
        kAdopted,  // an early flip the gate put in the ring before its completion: that pass enters nothing
        kClaimed,  // a writer's register stores under way
    };

    uint32_t bits = kIdle;

    static uint8_t stateOf(uint32_t w)
    {
        return static_cast<uint8_t>(w & 0xffu);
    }

    static uint32_t generationOf(uint32_t w)
    {
        return w >> 8;
    }

    static bool earlyState(uint8_t state)
    {
        return state == kEarly || state == kAdopted;
    }

    uint32_t now() const
    {
        return __atomic_load_n(&bits, __ATOMIC_ACQUIRE);
    }

    uint8_t state() const
    {
        return stateOf(now());
    }

    uint32_t generation() const
    {
        return generationOf(now());
    }

    bool early() const
    {
        return earlyState(state());
    }

    // The word once no claimer is between its claim and its stores.
    uint32_t settled() const
    {
        uint32_t w = now();
        while (stateOf(w) == kClaimed) {
            switch_flip_relax();
            w = now();
        }
        return w;
    }

    bool swap(uint32_t &from, uint8_t to)
    {
        return __atomic_compare_exchange_n(&bits, &from, (from & ~0xffu) | to, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_ACQUIRE);
    }

    // The gate, with the payload in place: armed under the next generation.
    void arm()
    {
        const uint32_t next = generationOf(__atomic_load_n(&bits, __ATOMIC_RELAXED)) + 1u;
        __atomic_store_n(&bits, (next ? next : 1u) << 8 | kArmed, __ATOMIC_RELEASE);
    }

    // The claimer, its stores done.
    void claimed(uint8_t to)
    {
        __atomic_store_n(&bits, (generationOf(__atomic_load_n(&bits, __ATOMIC_RELAXED)) << 8) | to, __ATOMIC_SEQ_CST);
    }

    // A producer's interrupt filter, its job certainly done, for the arm it was set up for.
    bool claimEarly(uint32_t armed)
    {
        uint32_t w = now();
        return stateOf(w) == kArmed && generationOf(w) == armed && swap(w, kClaimed);
    }

    // The completion's verdict: true when it is to write the flip now (then claimed(kWritten)). A failed frame is never
    // flipped to; a flip its filter already wrote is the completion's own from here.
    bool claim(int status)
    {
        for (;;) {
            uint32_t w = settled();
            if (stateOf(w) == kEarly) {
                if (swap(w, kWritten))
                    return false;
            } else if (stateOf(w) == kArmed) {
                if (swap(w, status ? kIdle : kClaimed))
                    return status == 0;
            } else {
                return false;
            }
        }
    }

    // The gate: an armed flip withdrawn (true), anything else left as it is.
    bool withdraw()
    {
        for (;;) {
            uint32_t w = settled();
            if (stateOf(w) != kArmed)
                return false;
            if (swap(w, kIdle))
                return true;
        }
    }

    // The gate, before it reads the latch or saves it: an early flip becomes the ring's (kAdopted). What it found.
    uint8_t adopt()
    {
        for (;;) {
            uint32_t w = settled();
            if (stateOf(w) != kEarly)
                return stateOf(w);
            if (swap(w, kAdopted))
                return kEarly;
        }
    }

    // The gate: whatever the flip was, idle now unless `keep` says to leave it; the state it was in. Only the gate moves
    // a written or adopted flip, so those take a store; an armed or early one may be claimed or delivered meanwhile.
    template <class Keep> uint8_t idleUnless(Keep keep)
    {
        for (;;) {
            uint32_t w = settled();
            const uint8_t state = stateOf(w);
            if (state == kIdle || keep(state))
                return state;
            if (state == kWritten || state == kAdopted) {
                __atomic_store_n(&bits, w & ~0xffu, __ATOMIC_RELEASE);
                return state;
            }
            if (swap(w, kIdle))
                return state;
        }
    }
};

struct SwitchCompletionFlip : SwitchFlipWord {
    enum : uint8_t {
        kFromCompletion,
        kFromGpuFilter,
        kFromVicFilter
    };

    // The payload: the gate's from the arm, the claimer's (writer and times) from its claim.
    struct View {
        uint8_t state = kIdle;
        uint8_t writer = kFromCompletion;
        int entry = -1;
        uint32_t start = 0, kind = 0, stride = 0, hOffset = 0;
        uint32_t generation = 0;
        bool layout = false;
        uint64_t requestedAt = 0;  // the display's frame ends when it was written
        uint64_t writtenAt = 0;    // mach time of that write
        uint64_t deliveredAt = 0;  // mach time an early flip's completion was delivered

        bool early() const
        {
            return earlyState(state);
        }
    };

    uint8_t writer = kFromCompletion;
    int entry = -1;
    uint32_t start = 0, kind = 0, stride = 0, hOffset = 0;
    bool layout = false;
    uint64_t requestedAt = 0, writtenAt = 0, deliveredAt = 0;

    // The gate, the flip idle: the frame's entry, START_ADDR and layout, then the word.
    void arm(int i, uint32_t address, uint32_t surfaceKind, uint32_t lineStride, uint32_t offset)
    {
        entry = i;
        start = address;
        kind = surfaceKind;
        stride = lineStride;
        hOffset = offset;
        layout = true;
        writer = kFromCompletion;
        SwitchFlipWord::arm();
    }

    bool claimEarly(uint32_t armed, uint8_t by)
    {
        if (!SwitchFlipWord::claimEarly(armed))
            return false;
        writer = by;
        return true;
    }

    bool claim(int status)
    {
        if (!SwitchFlipWord::claim(status))
            return false;
        writer = kFromCompletion;
        return true;
    }

    // presentEntry(i), the gate: entry i as the completion wrote it (written; the caller puts it in the ring unless the
    // gate adopted it, and writes nothing), or anything still armed withdrawn (the gate flips it itself). An early one
    // is reached here when the completion's pass runs between its gpuDonePending store and its claim. The state it was in.
    uint8_t present(int i, bool &written)
    {
        const uint8_t was = idleUnless([&](uint8_t state) {
            return state != kArmed && !((state == kWritten || earlyState(state)) && entry == i);
        });
        written = (was == kWritten || earlyState(was)) && entry == i;
        return was;
    }

    // A copy of the payload with the state it was read under.
    View view(uint8_t state) const
    {
        View v;
        v.state = state;
        v.writer = writer;
        v.entry = entry;
        v.start = start;
        v.kind = kind;
        v.stride = stride;
        v.hOffset = hOffset;
        v.generation = generation();
        v.layout = layout;
        v.requestedAt = requestedAt;
        v.writtenAt = writtenAt;
        v.deliveredAt = __atomic_load_n(&deliveredAt, __ATOMIC_RELAXED);
        return v;
    }
};

// Windows B and C's set, staged by the gate with every word but the surface addresses: the completion writes those
// (stores) and the STATE_CONTROL pair, or host1x's syncpoint filter does earlier (kEarly), once every producer's job is
// certainly done. Same states and lock as SwitchCompletionFlip.
struct SwitchPlaneCompletionFlip : SwitchFlipWord {
    static constexpr unsigned kMaxStores = 4;  // START_ADDR and START_ADDR_U of windows B and C

    unsigned stores = 0;
    uint32_t word[kMaxStores] = {}, value[kMaxStores] = {};
    uint32_t update = 0, request = 0;
    bool filtered = false;  // written by host1x's filter
    uint64_t requestedAt = 0, writtenAt = 0;

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

    // The gate, the stores staged: armed under the next generation.
    void arm(uint32_t updateBits, uint32_t requestBits)
    {
        update = updateBits;
        request = requestBits;
        filtered = false;
        SwitchFlipWord::arm();
    }

    bool claimEarly(uint32_t armed)
    {
        if (!SwitchFlipWord::claimEarly(armed))
            return false;
        filtered = true;
        return true;
    }

    // The gate's pass: true when the completion (or the filter) wrote it. An adopted set's request is the latch's
    // already.
    bool take()
    {
        const uint8_t was = idleUnless([](uint8_t state) {
            return state != kWritten && state != kEarly;
        });
        return was == kWritten || was == kEarly;
    }
};
