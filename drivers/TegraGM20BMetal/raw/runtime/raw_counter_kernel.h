// SPDX-License-Identifier: MIT
// A sample's resolve with the anchor of its power-up from the kernel's table (TegraGM20BClockABI.h): resolved() on the CPU,
// and for the blit encoder's resolveCounters: a kernel the raw device compiles for itself (RawMetalCounterEncode.mm), each
// thread one record into one sample in nanoseconds, the table read where the GPU maps it when the resolve runs.
// It works in 32-bit words (no 64-bit integer kernel is proven through the runtime's compile path): the span from the
// anchor in 16-bit digits, divided by 384 and multiplied by 625 a digit at a time. resolvedInWords is the same arithmetic
// on the host, which raw_counters_test holds equal to resolved().
#pragma once
#include <cstdint>
#include "TegraGM20BClockABI.h"
#include "raw_counters.h"

namespace raw::counters {

// The kernel's table as one reading of it: its count first, then the anchors it counts
inline void snapshot(const volatile TegraGM20BAbiClockAnchors &live, TegraGM20BAbiClockAnchors &out)
{
    out = {};
    out.published = __atomic_load_n(&live.published, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < TegraGM20BClockAnchorSlots; i++) {
        out.slot[i].units = live.slot[i].units;
        out.slot[i].ns = live.slot[i].ns;
    }
}

inline Anchor anchorOf(const TegraGM20BAbiClockAnchors &t, uint64_t units)
{
    TegraGM20BAbiClockAnchor a = {};
    return tgm_clock_anchor_of(&t, t.published, units, &a) ? Anchor{a.units, a.ns, true} : Anchor{};
}

// A sample before every anchor kept (more power-ups since than the table keeps) is the error value
inline uint64_t resolved(const Record &r, const TegraGM20BAbiClockAnchors &t)
{
    if (!r.payload)
        return 0;
    const Anchor a = anchorOf(t, r.units);
    return a.valid ? nanoseconds(a, r.units) : kErrorValue;
}

inline constexpr const char *kResolveFunction = "switch_counter_resolve";

inline constexpr const char *kResolveSource = R"(#include <metal_stdlib>
using namespace metal;

struct SwitchCounterResolve {
    uint count;
};

struct SwitchClockAnchors {
    uint published;
    uint reserved[3];
    uint4 slot[63];
};

static uint2 since(uint4 a, uint unitsLo, uint unitsHi)
{
    uint dLo = unitsLo - a.x;
    uint dHi = unitsHi - a.y - (unitsLo < a.x ? 1u : 0u);
    const bool negative = (dHi >> 31) != 0;
    if (negative) {
        dLo = ~dLo + 1u;
        dHi = ~dHi + (dLo == 0u ? 1u : 0u);
    }
    const uint digits[4] = {dHi >> 16, dHi & 0xffffu, dLo >> 16, dLo & 0xffffu};
    uint q[4];
    uint rem = 0;
    for (uint k = 0; k < 4; k++) {
        const uint cur = (rem << 16) | digits[k];
        q[k] = cur / 384u;
        rem = cur % 384u;
    }
    uint m[4];
    uint carry = 0;
    for (uint k = 4; k-- > 0;) {
        const uint v = q[k] * 625u + carry;
        m[k] = v & 0xffffu;
        carry = v >> 16;
    }
    uint mHi = (m[0] << 16) | m[1], mLo = (m[2] << 16) | m[3];
    const uint frac = (625u * rem + (negative ? 383u : 0u)) / 384u;
    const uint t = mLo + frac;
    mHi += t < mLo ? 1u : 0u;
    mLo = t;
    if (!negative) {
        const uint lo = a.z + mLo;
        return uint2(lo, a.w + mHi + (lo < a.z ? 1u : 0u));
    }
    return uint2(a.z - mLo, a.w - mHi - (a.z < mLo ? 1u : 0u));
}

kernel void switch_counter_resolve(device const uint4 *records [[buffer(0)]], device uint *samples [[buffer(1)]],
                                   constant SwitchCounterResolve &p [[buffer(2)]],
                                   device const SwitchClockAnchors &anchors [[buffer(3)]],
                                   uint i [[thread_position_in_grid]])
{
    if (i >= p.count)
        return;
    const uint4 r = records[i];
    uint2 ns = uint2(0u, 0u);
    if (r.x != 0) {
        ns = uint2(0xffffffffu, 0xffffffffu);
        const uint published = anchors.published;
        const uint kept = min(published, 62u);
        for (uint k = 0; k < kept; k++) {
            const uint4 a = anchors.slot[(published - 1u - k) % 63u];
            if (a.y < r.w || (a.y == r.w && a.x <= r.z)) {
                ns = since(a, r.z, r.w);
                break;
            }
        }
    }
    samples[2 * i] = ns.x;
    samples[2 * i + 1] = ns.y;
}
)";

constexpr uint32_t kResolveThreads = 64;

// SwitchCounterResolve
struct ResolveParams {
    uint32_t count;
};

static_assert(sizeof(ResolveParams) == 4, "SwitchCounterResolve");
static_assert(TegraGM20BClockAnchorSlots == 63, "SwitchClockAnchors");

inline uint64_t sinceInWords(const TegraGM20BAbiClockAnchor &anchor, uint32_t unitsLo, uint32_t unitsHi)
{
    const uint32_t a[4] = {uint32_t(anchor.units), uint32_t(anchor.units >> 32), uint32_t(anchor.ns),
                           uint32_t(anchor.ns >> 32)};
    uint32_t dLo = unitsLo - a[0];
    uint32_t dHi = unitsHi - a[1] - (unitsLo < a[0] ? 1u : 0u);
    const bool negative = (dHi >> 31) != 0;
    if (negative) {
        dLo = ~dLo + 1u;
        dHi = ~dHi + (dLo == 0u ? 1u : 0u);
    }
    const uint32_t digits[4] = {dHi >> 16, dHi & 0xffffu, dLo >> 16, dLo & 0xffffu};
    uint32_t q[4];
    uint32_t rem = 0;
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t cur = (rem << 16) | digits[k];
        q[k] = cur / 384u;
        rem = cur % 384u;
    }
    uint32_t m[4];
    uint32_t carry = 0;
    for (uint32_t k = 4; k-- > 0;) {
        const uint32_t v = q[k] * 625u + carry;
        m[k] = v & 0xffffu;
        carry = v >> 16;
    }
    uint32_t mHi = (m[0] << 16) | m[1], mLo = (m[2] << 16) | m[3];
    const uint32_t frac = (625u * rem + (negative ? 383u : 0u)) / 384u;
    const uint32_t t = mLo + frac;
    mHi += t < mLo ? 1u : 0u;
    mLo = t;
    uint32_t lo, hi;
    if (!negative) {
        lo = a[2] + mLo;
        hi = a[3] + mHi + (lo < a[2] ? 1u : 0u);
    } else {
        lo = a[2] - mLo;
        hi = a[3] - mHi - (a[2] < mLo ? 1u : 0u);
    }
    return uint64_t(hi) << 32 | lo;
}

// The kernel's search and arithmetic on the host, word for word
inline uint64_t resolvedInWords(const Record &r, const TegraGM20BAbiClockAnchors &t)
{
    if (!r.payload)
        return 0;
    const uint32_t unitsLo = uint32_t(r.units), unitsHi = uint32_t(r.units >> 32);
    const uint32_t kept = t.published < 62u ? t.published : 62u;
    for (uint32_t k = 0; k < kept; k++) {
        const TegraGM20BAbiClockAnchor &a = t.slot[(t.published - 1u - k) % 63u];
        const uint32_t aLo = uint32_t(a.units), aHi = uint32_t(a.units >> 32);
        if (aHi < unitsHi || (aHi == unitsHi && aLo <= unitsLo))
            return sinceInWords(a, unitsLo, unitsHi);
    }
    return kErrorValue;
}

}  // namespace raw::counters
