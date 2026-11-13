// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchWindowCaps.h through NativePlanes.h: NV24 and packed 4:2:2 layers get the window registers L4T's window.c and
// mainline's plane.c program for them, and a model of window B's fetch reads every pixel's Y', Cb and Cr from the bytes
// that hold them; every layer Core Animation may send within the limits fits one plane slot and its tables.
#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "../NativePlanes.h"

namespace {
using namespace native_planes;

constexpr uint32_t W = 720, H = 1280;
uint32_t rng = 0x2545f491;

uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return n ? rng % n : 0;
}

uint32_t roundUp(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

Surface surface(uint32_t format, uint32_t width, uint32_t height)
{
    const switch_window_caps::Format *f = switch_window_caps::find(
        format, format == fourcc('4', '4', '4', 'v') || format == fourcc('4', '4', '4', 'f') ? 2 : 0);
    Surface s;
    s.format = format;
    s.width = width;
    s.height = height;
    s.planes = f && f->planes == 2 ? 2 : 0;
    s.stride[0] = roundUp(width * (f ? f->lumaBytes : 4), 256);
    s.planeHeight[0] = height;
    if (s.planes == 2) {
        s.stride[1] = roundUp(width * f->chromaBytes, 256);
        s.planeHeight[1] = height;
        s.offset[1] = static_cast<uint64_t>(s.stride[0]) * height;
        s.allocation = s.offset[1] + static_cast<uint64_t>(s.stride[1]) * height;
    } else {
        s.allocation = static_cast<uint64_t>(s.stride[0]) * height;
    }
    s.descriptorBytes = (s.allocation + 16383) & ~16383ULL;
    return s;
}

Layer layer(uint32_t format, uint32_t width, uint32_t height, Rect src, Rect dst)
{
    Layer l;
    l.surface = surface(format, width, height);
    l.kind = kindOf(l.surface.format, l.surface.planes);
    l.src = src;
    l.dst = dst;
    return l;
}

// What window B reads for output pixel (i, j) at 1:1: the bytes of luma, Cb and Cr, as L4T window.c lays a window's
// planes out (START + V offset rows + H offset bytes; a packed pair's four bytes after BYTE_SWAP, UYVY order).
struct Fetched {
    uint64_t luma, cb, cr;
};

Fetched fetch(const Image &m, const switch_window_caps::Format &f, uint32_t i, uint32_t j)
{
    const uint32_t stride0 = m.lineStride & 0xffff, stride1 = m.lineStride >> 16;
    const uint64_t row0 = m.start[0] + static_cast<uint64_t>(j) * (f.planes > 1 ? stride0 : m.lineStride);
    const uint64_t x = m.hOffset / f.lumaBytes + i;
    if (f.planes > 1) {
        const uint64_t chroma = m.start[1] + static_cast<uint64_t>(j) * stride1 + 2 * x;
        return {row0 + x, chroma, chroma + 1};
    }
    const uint64_t pair = row0 + 4 * (x / 2);
    static const unsigned uyvy[4] = {1, 0, 3, 2};  // after SWAP2: byte k of the word comes from byte uyvy[k]
    const auto at = [&](unsigned k) {
        return pair + (f.byteSwap == switch_window_caps::kByteSwap2 ? uyvy[k] : k);
    };
    return {at(x % 2 ? 3 : 1), at(0), at(2)};
}

TEST(SwitchWindowCaps, KindsAndRegisters)
{
    struct Want {
        uint32_t format, depth, swap;
    };

    const Want want[] = {{fourcc('4', '4', '4', 'v'), 49, 0},
                         {fourcc('4', '4', '4', 'f'), 49, 0},
                         {fourcc('2', 'v', 'u', 'y'), 16, 0},
                         {fourcc('y', 'u', 'v', 's'), 16, 1},
                         {fourcc('y', 'u', 'v', 'f'), 16, 1}};
    for (const Want &w : want) {
        const Layer l = layer(w.format, 1280, 720, {0, 0, 1280, 720}, {0, 437, 720, 405});
        ASSERT_EQ(l.kind, kKindExtraYuv);
        EXPECT_TRUE(isYuv(l.kind));
        EXPECT_EQ(checkDetached(l, W, H), kAccepted);
        Image m;
        ASSERT_TRUE(imageOf(l, W, H, m));
        EXPECT_EQ(m.colorDepth, w.depth);
        EXPECT_EQ(m.byteSwap, w.swap);
        EXPECT_TRUE(m.options & kCscEnable);
        EXPECT_EQ(m.fullRange, (w.format & 0xff) == 'f');
        const uint32_t bpp = w.depth == 16 ? 2 : 1;
        EXPECT_EQ(m.prescaled, 720u << 16 | 1280 * bpp);
        EXPECT_EQ(prescaledBytesPerPixel(m), bpp);
        EXPECT_EQ(fetchBytesPerPixel(m), w.depth == 16 ? 2u : 3u);
    }
    EXPECT_EQ(kindOf(fourcc('4', '4', '4', 'v'), 0), kKindNone);
    EXPECT_EQ(kindOf(fourcc('2', 'v', 'u', 'y'), 2), kKindNone);
    EXPECT_EQ(kindOf(fourcc('x', '4', '2', '0'), 2), kKindNone);
}

// 1:1 crops of each format anywhere in the surface: every output pixel's luma, Cb and Cr come from the source pixel the
// rectangle names, inside the bytes the image says the window reads.
TEST(SwitchWindowCaps, TheFetchReadsEachPixelsOwnSamples)
{
    for (const switch_window_caps::Format &f : switch_window_caps::kFormats) {
        for (int n = 0; n < 200; ++n) {
            const uint32_t sw = 64 + rnd(1200), sh = 64 + rnd(1200);
            const uint32_t w = 2 + rnd(sw - 2 < 700 ? sw - 2 : 700) & ~1u, h = 1 + rnd(sh - 1 < 1200 ? sh - 1 : 1200);
            const uint32_t x = rnd(sw - w + 1) & (f.pairs ? ~1u : ~0u), y = rnd(sh - h + 1);
            const Layer l = layer(f.fourcc, sw, sh, {int32_t(x), int32_t(y), int32_t(w), int32_t(h)},
                                  {int32_t(rnd(W - w + 1)), int32_t(rnd(H - h + 1)), int32_t(w), int32_t(h)});
            ASSERT_EQ(checkDetached(l, W, H), kAccepted) << f.fourcc << " " << x << "," << y << " " << w << "x" << h;
            Image m;
            ASSERT_TRUE(imageOf(l, W, H, m));
            EXPECT_EQ(m.options & (kHFilterOn | kVFilterOn), 0u);
            for (const uint32_t j : {0u, h / 2, h - 1}) {
                for (const uint32_t i : {0u, 1u, w / 2, w - 2, w - 1}) {
                    const Fetched got = fetch(m, f, i, j);
                    const uint64_t sx = x + i, sy = y + j;
                    const Surface &s = l.surface;
                    Fetched want;
                    if (f.planes > 1) {
                        want = {sy * s.stride[0] + sx, s.offset[1] + sy * s.stride[1] + 2 * sx,
                                s.offset[1] + sy * s.stride[1] + 2 * sx + 1};
                    } else {
                        const uint64_t pair = sy * s.stride[0] + 4 * (sx / 2);
                        const bool uyvy = f.byteSwap == switch_window_caps::kByteSwapNone;
                        want = uyvy ? Fetched{pair + 1 + 2 * (sx % 2), pair, pair + 2}
                                    : Fetched{pair + 2 * (sx % 2), pair + 1, pair + 3};
                    }
                    EXPECT_EQ(got.luma, want.luma);
                    EXPECT_EQ(got.cb, want.cb);
                    EXPECT_EQ(got.cr, want.cr);
                    for (const uint64_t at : {got.luma, got.cb, got.cr}) {
                        EXPECT_GE(at, m.readLow);
                        EXPECT_LT(at, m.readHigh);
                    }
                }
            }
            EXPECT_LE(m.readHigh, (l.surface.allocation + 4095) & ~4095ULL);
        }
    }
}

TEST(SwitchWindowCaps, SurfaceRules)
{
    Layer l = layer(fourcc('4', '4', '4', 'v'), 1280, 720, {0, 0, 1280, 720}, {0, 0, 720, 405});
    EXPECT_EQ(checkDetached(l, W, H), kAccepted);
    l.surface.stride[1] = 2048;  // shorter than 1280 CbCr pairs
    EXPECT_EQ(checkDetached(l, W, H), kSurface);
    l.surface.stride[1] = 2560 + 64;
    EXPECT_EQ(checkDetached(l, W, H), kAlignment);
    l = layer(fourcc('2', 'v', 'u', 'y'), 1280, 720, {0, 0, 1280, 720}, {0, 0, 720, 405});
    l.surface.allocation -= 4096;
    EXPECT_EQ(checkDetached(l, W, H), kSurface);
    l = layer(fourcc('y', 'u', 'v', 's'), 3840, 2160, {0, 0, 3840, 2160}, {0, 437, 720, 405});
    EXPECT_EQ(checkDetached(l, W, H), kAccepted);
    l.dst = {0, 0, 600, 300};  // 6.4x across
    EXPECT_EQ(checkDetached(l, W, H), kScale);
}

// Every layer within the published limits, at the extremes of its source and buffer: its window reads fit one plane
// slot, and the plane slots plus the other mappings fit the domain's tables.
TEST(SwitchWindowCaps, EveryAcceptedLayerFitsOnePlaneSlot)
{
    const Limits limits;
    const uint32_t formats[] = {fourcc('B', 'G', 'R', 'A'), fourcc('4', '2', '0', 'v'), fourcc('4', '2', '2', 'v'),
                                fourcc('4', '4', '4', 'v'), fourcc('2', 'v', 'u', 'y')};
    uint64_t largest = 0;
    for (const uint32_t format : formats) {
        const bool rgb = format == fourcc('B', 'G', 'R', 'A');
        const bool nv = (format >> 8 & 0xff) == '2' || (format >> 8 & 0xff) == '0';
        Layer l;
        l.surface.format = format;
        l.surface.width = limits.maxSrcBuffer;
        l.surface.height = limits.maxSrcBufferHeight;
        if (rgb) {
            l.surface.stride[0] = limits.maxSrcBuffer * 4;
            l.surface.allocation = static_cast<uint64_t>(l.surface.stride[0]) * l.surface.height;
        } else if (nv) {
            l.surface = surface(fourcc('4', '4', '4', 'v'), limits.maxSrcBuffer, limits.maxSrcBufferHeight);
            l.surface.format = format;
            l.surface.stride[1] = l.surface.stride[0];
            l.surface.planeHeight[1] = (format >> 8 & 0xff) == '0' ? l.surface.height / 2 : l.surface.height;
            l.surface.allocation = l.surface.offset[1] + uint64_t(l.surface.stride[1]) * l.surface.planeHeight[1];
        } else {
            l.surface = surface(format, limits.maxSrcBuffer, limits.maxSrcBufferHeight);
        }
        l.surface.descriptorBytes = (l.surface.allocation + 16383) & ~16383ULL;
        l.kind = kindOf(l.surface.format, l.surface.planes);
        ASSERT_NE(l.kind, kKindNone) << format;
        const uint32_t down = rgb ? limits.minScaleFraction : limits.yuvMinScaleFraction;
        const int32_t sw = int32_t(limits.maxSrcBuffer), sh = int32_t(limits.maxSrcBufferHeight);
        const Rect sources[] = {{0, 0, sw, sh}, {0, 0, 1, sh}, {sw - 2, 0, 2, sh}};
        for (const Rect &src : sources) {
            l.src = src;
            l.dst = {0, 0, int32_t((src.w + down - 1) / down), int32_t((src.h + down - 1) / down)};
            if (static_cast<uint64_t>(src.w) * src.h > limits.maxSrcRectTotal)
                continue;
            ASSERT_EQ(checkDetached(l, W, H, limits), kAccepted) << format << " " << src.w;
            Image m;
            ASSERT_TRUE(imageOf(l, W, H, m));
            const uint64_t span = m.readHigh - m.readLow + 4096;  // a page either side of an unaligned start
            largest = span > largest ? span : largest;
            EXPECT_LE(span, switch_window_caps::kPlaneSlotSpan) << format << " " << src.w;
        }
    }
    std::printf("largest plane read: %llu MiB of a %llu MiB slot\n", static_cast<unsigned long long>(largest >> 20),
                static_cast<unsigned long long>(switch_window_caps::kPlaneSlotSpan >> 20));
    const uint64_t tablesPerSlot = switch_window_caps::kPlaneSlotSpan / (4ULL << 20);
    // 8 plane slots, 5 direct slots and the probe's, the directory, and identity maps of 3 scan buffers and the tape.
    EXPECT_LE(8 * tablesPerSlot + 6 + 1 + 8, switch_window_caps::kDomainTables);
    EXPECT_EQ(switch_window_caps::kPlaneSlotSpan % (4ULL << 20), 0u);
}

}  // namespace
