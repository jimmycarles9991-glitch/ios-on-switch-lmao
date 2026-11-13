// SPDX-License-Identifier: MIT
// Zero-bandwidth clears past the boot table: a full clear of a compressed target to a value the GPU's ZBC table lacks asks
// the kernel to add it (TegraGM20BABI.h method 19, nvkm gf100_gr_zbc_color_get / _depth_get as nvgpu's ZBC_SET_TABLE
// fills its table), off the render thread; that clear writes its texels, the next ones to the value write comptags only.
// The table is the GPU's, 15 colour and 15 depth entries shared by every process and never emptied: once it is full a
// value stays an ordinary clear. An entry is the clear's SET_COLOR_CLEAR_VALUE words (what the 3D engine matches) and the
// texel the L2 hands every reader of a tile cleared to it, so a value goes in only where that texel is exactly what the
// ROP writes for it: every channel an exact code of BGRA8Unorm or RGBA8Unorm (the ROP rounds a tie unlike Apple: HW Test
// gpu.x1.rop_conversion), any value of RGBA16Float. Those three packings HW Test gpu.raw.compressed_targets reads back;
// no other format goes in (sRGB, the 10-bit and 16-bit unorm formats, RGBA32Float beyond the boot table's 0 and 1).
#pragma once
#include "raw_lock.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include "TegraGM20BABI.h"

namespace raw {

enum : uint32_t {
    kZbcRf16x4 = 0x16,    // FERMI_A_ZBC_COLOR_V0_FMT_RF16_GF16_BF16_AF16
    kZbcA8R8G8B8 = 0x18,  // BGRA8Unorm's
    kZbcA8B8G8R8 = 0x28,  // RGBA8Unorm's
    kZbcDepthFp32 = 1,    // FERMI_A_ZBC_DEPTH_V0_FMT_FP32
};

inline float zbcFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// The code of `bits` as a float in a unorm channel of `max`, false where it is not exactly one
inline bool zbcUnorm(uint32_t bits, uint32_t max, uint32_t &code)
{
    const double v = double(zbcFloat(bits)) * max;
    if (!(v >= 0 && v <= max) || std::fabs(v - std::nearbyint(v)) > 1e-3)
        return false;
    code = uint32_t(std::nearbyint(v));
    return true;
}

// IEEE half of a float, rounded to nearest even as the ROP stores RGBA16Float (HW Test gpu.x1.rop_conversion: exact)
inline uint32_t zbcHalf(uint32_t bits)
{
    const uint32_t sign = bits >> 16 & 0x8000;
    const int32_t exponent = int32_t(bits >> 23 & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if ((bits & 0x7fffffff) > 0x7f800000)
        return sign | 0x7e00;
    if (exponent >= 31)
        return sign | 0x7c00;
    if (exponent <= 0) {
        if (exponent < -10)
            return sign;
        mantissa |= 0x800000;
        const uint32_t shift = uint32_t(14 - exponent);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rest > mid || (rest == mid && (half & 1)))
            half++;
        return sign | half;
    }
    uint32_t half = uint32_t(exponent) << 10 | mantissa >> 13;
    const uint32_t rest = mantissa & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1)))
        half++;
    return sign | half;
}

// The entry for a clear of Metal colour format `mtl` to SET_COLOR_CLEAR_VALUE words `ds`; false where none may be added
inline bool zbcColorEntry(uint32_t mtl, const uint32_t ds[4], TegraGM20BAbiZbc &out)
{
    out = {};
    out.type = TegraGM20BZbcColor;
    memcpy(out.ds, ds, sizeof out.ds);
    uint32_t c[4];
    const auto unorm = [&](uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
        return zbcUnorm(ds[0], r, c[0]) && zbcUnorm(ds[1], g, c[1]) && zbcUnorm(ds[2], b, c[2]) &&
               zbcUnorm(ds[3], a, c[3]);
    };
    switch (mtl) {
    case 70:  // RGBA8Unorm
        out.format = kZbcA8B8G8R8;
        if (!unorm(255, 255, 255, 255))
            return false;
        out.l2[0] = c[0] | c[1] << 8 | c[2] << 16 | c[3] << 24;
        return true;
    case 80:  // BGRA8Unorm
        out.format = kZbcA8R8G8B8;
        if (!unorm(255, 255, 255, 255))
            return false;
        out.l2[0] = c[2] | c[1] << 8 | c[0] << 16 | c[3] << 24;
        return true;
    case 115:  // RGBA16Float
        out.format = kZbcRf16x4;
        out.l2[0] = zbcHalf(ds[0]) | zbcHalf(ds[1]) << 16;
        out.l2[1] = zbcHalf(ds[2]) | zbcHalf(ds[3]) << 16;
        return true;
    default:
        return false;
    }
}

// The entry for a depth clear of a 32-bit float depth format (Depth32Float, Depth32Float_Stencil8) to `depth`
inline bool zbcDepthEntry(uint32_t mtl, float depth, TegraGM20BAbiZbc &out)
{
    out = {};
    if (mtl != 252 && mtl != 260)
        return false;
    out.type = TegraGM20BZbcDepth;
    out.format = kZbcDepthFp32;
    memcpy(&out.ds[0], &depth, 4);
    out.l2[0] = out.ds[0];
    return true;
}

// The values this process cleared compressed targets to, by state; its own lock, as clears are encoded on many threads. A
// value is asked for at its kZbcRepeats-th full clear: the GPU's few entries are every process's, and a clear colour an
// animation changes every frame would fill them.
class ZbcTable {
public:
    enum State : uint8_t {
        Seen,
        Wanted,
        Asking,
        Held,     // in the GPU's table
        Refused,  // the table was full, or the kernel took no such entry
    };

    static constexpr uint8_t kZbcRepeats = 3;

    // Whether a clear to `e` writes comptags only; true in `wanted` when it just became wanted (the caller wakes the
    // thread that asks)
    bool lookup(const TegraGM20BAbiZbc &e, bool &wanted)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        wanted = false;
        Entry *x = find(e);
        if (!x) {
            if (!(x = slot()))
                return false;
            *x = {e, Seen, 0};
        }
        if (x->state == Seen && ++x->seen >= kZbcRepeats) {
            x->state = Wanted;
            wanted = true;
        }
        return x->state == Held;
    }

    // The next entry to ask the kernel for, now Asking
    bool next(TegraGM20BAbiZbc &e)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        for (uint32_t i = 0; i < count_; i++)
            if (entries_[i].state == Wanted) {
                entries_[i].state = Asking;
                e = entries_[i].key;
                return true;
            }
        return false;
    }

    void answer(const TegraGM20BAbiZbc &e, bool held)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        if (Entry *x = find(e))
            x->state = held ? Held : Refused;
    }

    bool pending()
    {
        std::lock_guard<raw::Mutex> l(lock_);
        for (uint32_t i = 0; i < count_; i++)
            if (entries_[i].state == Wanted)
                return true;
        return false;
    }

private:
    // the GPU's 15 colour and 15 depth entries, and a few its full table refused
    static constexpr uint32_t kEntries = 40;

    struct Entry {
        TegraGM20BAbiZbc key;
        State state;
        uint8_t seen;
    };

    // A new entry's place: a free one, else the next value only seen so far in turn; null when every one is asked for
    Entry *slot()
    {
        if (count_ < kEntries)
            return &entries_[count_++];
        for (uint32_t i = 0; i < kEntries; i++) {
            Entry &x = entries_[(evict_ + i) % kEntries];
            if (x.state == Seen) {
                evict_ = (evict_ + i + 1) % kEntries;
                return &x;
            }
        }
        return nullptr;
    }

    Entry *find(const TegraGM20BAbiZbc &e)
    {
        for (uint32_t i = 0; i < count_; i++)
            if (!memcmp(&entries_[i].key, &e, sizeof e))
                return &entries_[i];
        return nullptr;
    }

    raw::Mutex lock_;
    Entry entries_[kEntries] = {};
    uint32_t count_ = 0, evict_ = 0;
};

}  // namespace raw
