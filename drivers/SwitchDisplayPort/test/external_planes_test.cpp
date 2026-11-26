// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchExternalPlanes.h on the host: the clone swaps Core Animation builds, accepted and planned onto DC B's windows; a
// sweep holding validation to the published limits (every swap inside them accepted, every accepted one planned); the
// memory the published scaling can ask for at the top pixel clock; the published dictionary as this IPSW kind builds it.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "../SwitchExternalPlanes.h"
#include "../../SwitchPower/SwitchDisplayMemoryMath.h"

using namespace switch_external;
using native_planes::fourcc;
using native_planes::kBackgroundBit;
using native_planes::Kind;
using native_planes::kOpaqueBlack;
using native_planes::Rect;
using native_planes::Surface;

namespace {
constexpr uint32_t kUltraW = 3440, kUltraH = 1440;
constexpr uint32_t kUiIova = 0x10000000, kVideoIova = 0x14400000;
constexpr uint32_t kMatrix709 = 2;  // SWITCH_VIC_MATRIX_709, as switch_vic_surface_color reports it
constexpr WindowState kIdle[kWindows] = {};

uint64_t roundUp(uint64_t v, uint64_t a)
{
    return (v + a - 1) / a * a;
}

Surface makeSurface(Kind kind, uint32_t width, uint32_t height, uint32_t rowAlign = 64, uint32_t baseAlign = 4096)
{
    Surface s;
    s.width = width;
    s.height = height;
    if (native_planes::isYuv(kind)) {
        const bool nv12 = native_planes::isNv12(kind);
        const bool full = kind == native_planes::kKindNv12Full || kind == native_planes::kKindNv16Full;
        s.format = fourcc('4', '2', nv12 ? '0' : '2', full ? 'f' : 'v');
        s.planes = 2;
        s.stride[0] = s.stride[1] = uint32_t(roundUp((width + 1) / 2 * 2, rowAlign));
        s.planeHeight[0] = height;
        s.planeHeight[1] = nv12 ? (height + 1) / 2 : height;
        s.offset[1] = roundUp(uint64_t(s.stride[0]) * height, baseAlign);
        s.allocation = s.offset[1] + uint64_t(s.stride[1]) * s.planeHeight[1];
        s.colorMatrix = kMatrix709;
    } else {
        s.format = kind == native_planes::kKindRgba ? fourcc('R', 'G', 'B', 'A') : fourcc('B', 'G', 'R', 'A');
        s.stride[0] = uint32_t(roundUp(uint64_t(width) * 4, rowAlign));
        s.planeHeight[0] = height;
        s.allocation = uint64_t(s.stride[0]) * height;
    }
    s.descriptorBytes = roundUp(s.allocation, 16384);
    return s;
}

Layer makeLayer(Kind kind, uint32_t width, uint32_t height, Rect src, Rect dst)
{
    Layer l;
    l.kind = kind;
    l.surface = makeSurface(kind, width, height);
    l.src = src;
    l.dst = dst;
    return l;
}

// A layer whose surface is its destination's size, scanned whole (the clone's video and a 1:1 UI).
Layer wholeLayer(Kind kind, Rect dst)
{
    return makeLayer(kind, uint32_t(dst.w), uint32_t(dst.h), {0, 0, dst.w, dst.h}, dst);
}

struct Swap {
    uint32_t updated = kBackgroundBit;
    bool present[kLayers] = {};
    Layer layers[kLayers];
    Image images[kLayers];

    void put(unsigned i, const Layer &l)
    {
        updated |= 1u << i;
        present[i] = true;
        layers[i] = l;
    }

    unsigned check(uint32_t width, uint32_t height) const
    {
        return checkSwap(updated, kOpaqueBlack, present, layers, width, height);
    }

    bool image(uint32_t width, uint32_t height)
    {
        for (unsigned i = 0; i < kLayers; ++i)
            if (present[i] && !imageFor(layers[i], nullptr, width, height, images[i]))
                return false;
        return true;
    }
};

const Kind kUi = native_planes::kKindBgra;
const Kind kVideo = native_planes::kKindNv12Video;

// The clone of a 1280x720 UI with a 768x480 video, on an ultrawide: the video 1:1 in the middle, the UI pillarboxed.
Swap cloneSwap()
{
    Swap s;
    s.put(1, wholeLayer(kUi, {440, 0, 2560, 1440}));
    s.put(2, wholeLayer(kVideo, {1336, 480, 768, 480}));
    return s;
}

uint32_t iovas[kLayers] = {0, kUiIova, kVideoIova, 0};
}  // namespace

TEST(ExternalPlanes, CloneVideoUnderPillarboxedUi)
{
    Swap s = cloneSwap();
    ASSERT_EQ(s.check(kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    const Plan plan = planWindows(s.present, s.images, kIdle);
    EXPECT_EQ(plan.unshown, 0u);
    EXPECT_TRUE(plan.window[kA].on);
    EXPECT_EQ(plan.window[kA].layer, 1);
    EXPECT_EQ(plan.window[kA].depth, 0u);
    EXPECT_TRUE(plan.window[kB].on);
    EXPECT_EQ(plan.window[kB].layer, 2);
    EXPECT_EQ(plan.window[kB].depth, 1u);
    EXPECT_TRUE(plan.window[kB].csc);
    EXPECT_FALSE(plan.window[kC].on);

    const Frame frame = frameOf(plan, s.images, iovas, 319750000);
    const WindowRegisters &ui = frame.window[kA];
    EXPECT_EQ(ui.options, native_planes::kWinEnable);
    EXPECT_EQ(ui.colorDepth, native_planes::kDepthB8G8R8A8);
    EXPECT_EQ(ui.position, 440u);
    EXPECT_EQ(ui.size, (1440u << 16) | 2560u);
    EXPECT_EQ(ui.prescaled, (1440u << 16) | (2560u * 4u));
    EXPECT_EQ(ui.blend, native_planes::kBlendPremultiplied | 0u);
    EXPECT_EQ(ui.blendSelect, native_planes::kBlendOver);
    EXPECT_EQ(ui.start, kUiIova);
    EXPECT_EQ(ui.lineStride, 2560u * 4u);
    EXPECT_FALSE(ui.csc);

    const WindowRegisters &video = frame.window[kB];
    const Surface &v = s.layers[2].surface;
    EXPECT_EQ(video.options, native_planes::kWinEnable | native_planes::kCscEnable);
    EXPECT_EQ(video.colorDepth, native_planes::kDepthYCbCr420SP);
    EXPECT_EQ(video.position, (480u << 16) | 1336u);
    EXPECT_EQ(video.size, (480u << 16) | 768u);
    EXPECT_EQ(video.prescaled, (480u << 16) | 768u);
    EXPECT_EQ(video.blend, native_planes::kBlendBypass | 1u);
    EXPECT_EQ(video.blendSelect, 0u);
    EXPECT_EQ(video.start, kVideoIova);
    EXPECT_EQ(video.startU, kVideoIova + uint32_t(v.offset[1]));
    EXPECT_EQ(video.lineStride, (v.stride[1] << 16) | v.stride[0]);
    EXPECT_TRUE(video.csc);
    EXPECT_EQ(video.matrix, int(native_planes::kBt709));
    EXPECT_EQ(video.fullRange, 0);
    for (unsigned i = 0; i < 8; ++i)
        EXPECT_EQ(video.cscWords[i], native_planes::kCsc[native_planes::kBt709][0][i]);
    EXPECT_FALSE(frame.window[kC].on);

    EXPECT_TRUE(frame.memory[kA].active);
    EXPECT_TRUE(frame.memory[kB].active);
    EXPECT_FALSE(frame.memory[kC].active);
    EXPECT_EQ(frame.peakBytes, 319750000ULL * 4 + 319750000ULL * 2);
}

TEST(ExternalPlanes, UiOnly)
{
    Swap s;
    s.put(0, makeLayer(kUi, 1280, 720, {0, 0, 1280, 720}, {440, 0, 2560, 1440}));
    s.updated |= 1u << 1;
    ASSERT_EQ(s.check(kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    const Plan plan = planWindows(s.present, s.images, kIdle);
    EXPECT_EQ(plan.window[kA].layer, 0);
    EXPECT_FALSE(plan.window[kB].on);
    EXPECT_FALSE(plan.window[kC].on);
    const Frame frame = frameOf(plan, s.images, iovas, 319750000);
    EXPECT_EQ(frame.window[kA].dda, (0x0800u << 16) | 0x0800u);
    EXPECT_EQ(frame.window[kA].options,
              native_planes::kWinEnable | native_planes::kHFilterOn | native_planes::kVFilterOn);
    EXPECT_EQ(frame.peakBytes, 319750000ULL * 4);
}

// A video frame alone: the family fills the UI in from the screen. The video stays on its window, whose CSC holds its
// matrix already, and nothing else moves.
TEST(ExternalPlanes, VideoOnlyUpdateWithInheritedSurfaces)
{
    Swap s = cloneSwap();
    const bool updatedOnly[kLayers] = {false, false, true, false};
    EXPECT_EQ(checkSwap(1u << 2, 0, updatedOnly, s.layers, kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    const WindowState scanning[kWindows] = {{true, -1, -1}, {true, int(native_planes::kBt709), 0}, {}};
    const Plan plan = planWindows(s.present, s.images, scanning);
    EXPECT_EQ(plan.window[kA].layer, 1);
    EXPECT_EQ(plan.window[kB].layer, 2);
    EXPECT_FALSE(plan.window[kB].csc);
    EXPECT_FALSE(plan.window[kC].on);
}

TEST(ExternalPlanes, LoneYuvLayer)
{
    Swap s;
    s.put(2, wholeLayer(kVideo, {0, 0, 1920, 1080}));
    ASSERT_EQ(s.check(1920, 1080), native_planes::kAccepted);
    ASSERT_TRUE(s.image(1920, 1080));
    const Plan plan = planWindows(s.present, s.images, kIdle);
    EXPECT_FALSE(plan.window[kA].on);
    EXPECT_EQ(plan.window[kB].layer, 2);
    EXPECT_EQ(plan.window[kB].depth, 0u);
    EXPECT_TRUE(plan.window[kB].csc);
    EXPECT_FALSE(plan.window[kC].on);
}

TEST(ExternalPlanes, RgbUnderUi)
{
    Swap s = cloneSwap();
    s.layers[2] = wholeLayer(kUi, {1336, 480, 768, 480});
    ASSERT_EQ(s.check(kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    const Plan plan = planWindows(s.present, s.images, kIdle);
    EXPECT_EQ(plan.window[kA].layer, 1);
    EXPECT_EQ(plan.window[kB].layer, 2);
    EXPECT_FALSE(plan.window[kB].csc);
    const Frame frame = frameOf(plan, s.images, iovas, 319750000);
    EXPECT_EQ(frame.window[kB].blend, native_planes::kBlendPremultiplied | 1u);
    EXPECT_EQ(frame.window[kB].blendSelect, native_planes::kBlendOver);
    EXPECT_EQ(frame.window[kB].matrix, -1);
}

// The CSC is not shadowed: a video whose matrix changes moves to a window that is not scanning and is written there; with
// none free VIC converts it, and it shows as BGRA.
TEST(ExternalPlanes, CscChangeMovesWindows)
{
    Swap s = cloneSwap();
    s.layers[2].surface.colorMatrix = 1;  // BT.601
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    const WindowState scanning[kWindows] = {{true, -1, -1}, {true, int(native_planes::kBt709), 0}, {}};
    Plan plan = planWindows(s.present, s.images, scanning);
    EXPECT_EQ(plan.window[kA].layer, 1);
    EXPECT_FALSE(plan.window[kB].on);
    EXPECT_EQ(plan.window[kC].layer, 2);
    EXPECT_TRUE(plan.window[kC].csc);
    EXPECT_EQ(frameOf(plan, s.images, iovas, 148500000).window[kC].cscWords[3],
              native_planes::kCsc[native_planes::kBt601][0][3]);

    Swap three = cloneSwap();
    three.put(0, wholeLayer(kVideo, {0, 0, 640, 360}));
    three.layers[2].surface.colorMatrix = 1;
    ASSERT_TRUE(three.image(kUltraW, kUltraH));
    const WindowState busy[kWindows] = {
        {true, -1, -1}, {true, int(native_planes::kBt709), 0}, {true, int(native_planes::kBt2020), 0}};
    plan = planWindows(three.present, three.images, busy);
    EXPECT_EQ(plan.window[kA].layer, 1);
    EXPECT_EQ(plan.window[kB].layer, 0);
    EXPECT_FALSE(plan.window[kB].csc);
    EXPECT_FALSE(plan.window[kC].on);
    EXPECT_EQ(plan.convert, 1u << 2);
    EXPECT_EQ(plan.unshown, 0u);

    ASSERT_TRUE(planSwap(three.present, three.layers, three.images, busy, kUltraW, kUltraH, plan));
    EXPECT_EQ(plan.convert, 1u << 2);
    EXPECT_EQ(plan.window[kC].layer, 2);
    EXPECT_FALSE(plan.window[kC].csc);
    EXPECT_FALSE(three.images[2].yuv);
    const Frame frame = frameOf(plan, three.images, iovas, 148500000);
    EXPECT_EQ(frame.window[kC].colorDepth, native_planes::kDepthB8G8R8A8);
    EXPECT_EQ(frame.window[kC].matrix, -1);
    EXPECT_EQ(frame.window[kC].lineStride, conversionOf(three.layers[2]).pitch);

    const WindowState settled[kWindows] = {busy[kA], busy[kB], {}};
    ASSERT_TRUE(three.image(kUltraW, kUltraH));
    plan = planWindows(three.present, three.images, settled);
    EXPECT_EQ(plan.window[kC].layer, 2);
    EXPECT_TRUE(plan.window[kC].csc);
    EXPECT_EQ(plan.convert, 0u);
}

// Chroma sited where no window samples it (Center, code 2) is resampled by VIC, as the panel's windows have it; Left
// (code 1) and unspecified are scanned as they are.
TEST(ExternalPlanes, MisSitedChromaIsConverted)
{
    Swap s = cloneSwap();
    s.layers[2].surface.chromaTop = s.layers[2].surface.chromaBottom = 2;
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    EXPECT_TRUE(s.images[2].convert);
    Plan plan;
    ASSERT_TRUE(planSwap(s.present, s.layers, s.images, kIdle, kUltraW, kUltraH, plan));
    EXPECT_EQ(plan.convert, 1u << 2);
    EXPECT_EQ(plan.window[kA].layer, 1);
    EXPECT_EQ(plan.window[kB].layer, 2);
    EXPECT_EQ(plan.window[kB].depth, 1u);
    EXPECT_EQ(frameOf(plan, s.images, iovas, 148500000).window[kB].blend, native_planes::kBlendPremultiplied | 1u);

    s.layers[2].surface.chromaTop = s.layers[2].surface.chromaBottom = 1;
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    ASSERT_TRUE(planSwap(s.present, s.layers, s.images, kIdle, kUltraW, kUltraH, plan));
    EXPECT_EQ(plan.convert, 0u);
}

// Packed 4:2:2 that VIC reads ('yuvs') is converted when its chroma is mis-sited; 'yuvf', which VIC has no format for, is
// scanned co-sited instead, and with no window for its matrix it is left for requestFrame's panic, never sent to VIC.
TEST(ExternalPlanes, PackedYuvVicCannotReadIsScannedAsItIs)
{
    Swap s = cloneSwap();
    Layer &video = s.layers[2];
    video.kind = native_planes::kKindExtraYuv;
    video.surface.planes = 1;
    video.surface.offset[1] = video.surface.stride[1] = video.surface.planeHeight[1] = 0;
    video.surface.stride[0] = uint32_t(roundUp(uint64_t(video.surface.width) * 2, 64));
    video.surface.allocation = uint64_t(video.surface.stride[0]) * video.surface.height;
    video.surface.descriptorBytes = roundUp(video.surface.allocation, 16384);
    video.surface.chromaTop = video.surface.chromaBottom = 2;
    Plan plan;

    video.surface.format = fourcc('y', 'u', 'v', 's');
    ASSERT_EQ(s.check(kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    ASSERT_TRUE(planSwap(s.present, s.layers, s.images, kIdle, kUltraW, kUltraH, plan));
    EXPECT_EQ(plan.convert, 1u << 2);

    video.surface.format = fourcc('y', 'u', 'v', 'f');
    ASSERT_EQ(s.check(kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    EXPECT_FALSE(s.images[2].convert);
    ASSERT_TRUE(planSwap(s.present, s.layers, s.images, kIdle, kUltraW, kUltraH, plan));
    EXPECT_EQ(plan.convert, 0u);
    EXPECT_EQ(plan.unshown, 0u);
    EXPECT_EQ(plan.window[kB].layer, 2);
    EXPECT_TRUE(s.images[2].yuv);

    const WindowState busy[kWindows] = {{true, -1, -1}, {true, 0, 0}, {true, 0, 0}};
    ASSERT_TRUE(planSwap(s.present, s.layers, s.images, busy, kUltraW, kUltraH, plan));
    EXPECT_EQ(plan.convert, 0u);
    EXPECT_EQ(plan.unshown, 1u << 2);
}

// A YUV layer narrowed between 4x and 8x (a YUV window's H DDA takes it, an RGB one's does not) converts to a quarter of
// its width, so its BGRA still has a window.
TEST(ExternalPlanes, NarrowedConversionKeepsAWindow)
{
    Swap s;
    s.put(2, makeLayer(kVideo, 3840, 1080, {0, 0, 3840, 1080}, {0, 0, 500, 1080}));
    s.layers[2].surface.chromaTop = 2;
    ASSERT_EQ(s.check(1920, 1080), native_planes::kAccepted);
    ASSERT_TRUE(s.image(1920, 1080));
    const Conversion c = conversionOf(s.layers[2]);
    EXPECT_EQ(c.divisor, 4u);
    EXPECT_EQ(c.width, 960u);
    Plan plan;
    ASSERT_TRUE(planSwap(s.present, s.layers, s.images, kIdle, 1920, 1080, plan));
    EXPECT_EQ(plan.convert, 1u << 2);
    EXPECT_EQ(s.images[2].dda & 0xffffu, (960u << 12) / 500u);
}

TEST(ExternalPlanes, ThreeSurfacesPlannedFourRefused)
{
    Swap s = cloneSwap();
    s.put(0, wholeLayer(kUi, {0, 0, 256, 256}));
    ASSERT_EQ(s.check(kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    Plan plan = planWindows(s.present, s.images, kIdle);
    EXPECT_EQ(plan.unshown, 0u);
    EXPECT_EQ(plan.window[kA].layer, 1);
    EXPECT_EQ(plan.window[kB].layer, 2);
    EXPECT_EQ(plan.window[kC].layer, 0);
    EXPECT_EQ(plan.window[kC].depth, 1u);
    EXPECT_EQ(plan.window[kB].depth, 2u);

    s.put(3, wholeLayer(kUi, {0, 0, 64, 64}));
    EXPECT_EQ(s.check(kUltraW, kUltraH), native_planes::kTwoUi);
    ASSERT_TRUE(s.image(kUltraW, kUltraH));
    plan = planWindows(s.present, s.images, kIdle);
    EXPECT_EQ(plan.unshown, 1u << 2);
    EXPECT_EQ(plan.window[kA].layer, 3);

    // checkSwap counts the layers a swap updates; a fourth that an earlier swap left on is the screen's to catch
    // (requestFrame panics on plan.unshown: clone_update never sends layer 3).
    s.updated = kBackgroundBit | 1u << 3;
    EXPECT_EQ(s.check(kUltraW, kUltraH), native_planes::kAccepted);
    ASSERT_TRUE(planSwap(s.present, s.layers, s.images, kIdle, kUltraW, kUltraH, plan));
    EXPECT_EQ(plan.unshown, 1u << 2);
}

TEST(ExternalPlanes, TransformedLayerRefused)
{
    Swap s = cloneSwap();
    s.layers[2].transform = 4;
    EXPECT_EQ(s.check(kUltraW, kUltraH), native_planes::kTransformed);
}

TEST(ExternalPlanes, BackgroundAndUnknownBitsRefused)
{
    Swap s = cloneSwap();
    EXPECT_EQ(checkSwap(s.updated, 0xff112233u, s.present, s.layers, kUltraW, kUltraH), native_planes::kLayerState);
    EXPECT_EQ(checkSwap(s.updated | 0x10u, kOpaqueBlack, s.present, s.layers, kUltraW, kUltraH),
              native_planes::kLayerState);
    const bool none[kLayers] = {};
    EXPECT_EQ(checkSwap(0x7u | kBackgroundBit, kOpaqueBlack, none, s.layers, kUltraW, kUltraH),
              native_planes::kAccepted);
}

namespace {
uint64_t rng = 0x9e3779b97f4a7c15ull;

uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return n ? uint32_t(rng % n) : 0;
}

struct Mode {
    uint32_t width, height;
};

constexpr Mode kModes[] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}, {4096, 2160}};

// Whether Core Animation may send the layer under what the display publishes: 8-bit YUV or 32-bit RGB (the clone
// carries no other; VIC copies none), no transform, buffers within IOMFBMaxSrcPixels, scale within IOMFBScalingLimits,
// rows and plane bases on the published alignments.
bool published(const Layer &l)
{
    const Surface &s = l.surface;
    if (l.kind == native_planes::kKindNone || l.transform || s.width > SWITCH_DP_MAX_WIDTH ||
        s.height > SWITCH_DP_MAX_HEIGHT)
        return false;
    const uint64_t sw = uint64_t(l.src.w), sh = uint64_t(l.src.h), dw = uint64_t(l.dst.w), dh = uint64_t(l.dst.h);
    if (dw * kMinScaleFraction < sw || dh * kMinScaleFraction < sh || dw > sw * kMaxScale || dh > sh * kMaxScale)
        return false;
    for (unsigned p = 0; p < (native_planes::isYuv(l.kind) ? 2u : 1u); ++p)
        if (s.stride[p] % kStrideAlignment || s.offset[p] % kBaseAlignment)
            return false;
    return true;
}

Layer randomLayer(const Mode &mode)
{
    static const Kind kinds[] = {native_planes::kKindBgra,      native_planes::kKindRgba,
                                 native_planes::kKindNv12Video, native_planes::kKindNv12Full,
                                 native_planes::kKindNv16Video, native_planes::kKindNv16Full,
                                 native_planes::kKindNone};
    static const uint32_t rowAligns[] = {16, 64, 128, 256};
    static const uint32_t baseAligns[] = {16, 64, 4096};
    static const uint32_t scales[][2] = {{1, 8}, {1, 4}, {1, 3}, {1, 2}, {2, 3}, {1, 1},
                                         {1, 1}, {3, 2}, {2, 1}, {3, 1}, {4, 1}, {8, 1}};
    Layer l;
    l.kind = kinds[rnd(7)];
    const uint32_t width = 2 + rnd(rnd(4) ? 2047 : 4095), height = 2 + rnd(rnd(4) ? 2047 : 4095);
    const Kind shape = l.kind == native_planes::kKindNone ? native_planes::kKindNv12Video : l.kind;
    l.surface = makeSurface(shape, width, height, rowAligns[rnd(4)], baseAligns[rnd(3)]);
    if (l.kind == native_planes::kKindNone)
        l.surface.format = fourcc('x', '4', '2', '0');
    l.surface.chromaTop = rnd(8);
    l.surface.chromaBottom = rnd(4) ? l.surface.chromaTop : rnd(8);
    l.src.x = int32_t(rnd(width / 4));
    l.src.y = int32_t(rnd(height / 4));
    l.src.w = int32_t(1 + rnd(width - uint32_t(l.src.x)));
    l.src.h = int32_t(1 + rnd(height - uint32_t(l.src.y)));
    const uint32_t *sx = scales[rnd(12)];
    const uint32_t *sy = rnd(2) ? sx : scales[rnd(12)];
    l.dst.w = std::max<int32_t>(1, int32_t(uint64_t(l.src.w) * sx[0] / sx[1]));
    l.dst.h = std::max<int32_t>(1, int32_t(uint64_t(l.src.h) * sy[0] / sy[1]));
    // Destinations no larger than a display and at most half off it, inside DC B's 13-bit window coordinates.
    if (l.dst.w > int32_t(SWITCH_DP_MAX_WIDTH) || l.dst.h > int32_t(SWITCH_DP_MAX_HEIGHT)) {
        l.dst.w = l.src.w;
        l.dst.h = l.src.h;
    }
    l.dst.x = int32_t(rnd(mode.width)) - l.dst.w / 2;
    l.dst.y = int32_t(rnd(mode.height)) - l.dst.h / 2;
    l.transform = rnd(16) ? 0 : 4;
    return l;
}
}  // namespace

// Every swap the published limits allow is accepted, and every accepted one, whatever DC B's windows hold, has a window
// for each layer on the screen, directly or through VIC's BGRA, with registers that read only that buffer's own bytes.
TEST(ExternalPlanes, SweepAcceptsThePublishedAndPlansTheAccepted)
{
    unsigned accepted = 0, inside = 0, converted = 0;
    for (unsigned n = 0; n < 200000; ++n) {
        const Mode &mode = kModes[rnd(6)];
        Swap s;
        const unsigned count = 1 + rnd(4);
        bool allPublished = count <= kMaxLayers;
        for (unsigned k = 0; k < count; ++k) {
            unsigned i = rnd(kLayers);
            while (s.present[i])
                i = (i + 1) % kLayers;
            s.put(i, randomLayer(mode));
            allPublished = allPublished && published(s.layers[i]);
        }
        const unsigned reason = s.check(mode.width, mode.height);
        inside += allPublished;
        if (allPublished)
            ASSERT_EQ(reason, native_planes::kAccepted) << "swap " << n;
        if (reason != native_planes::kAccepted)
            continue;
        ++accepted;
        ASSERT_TRUE(s.image(mode.width, mode.height)) << "swap " << n;
        for (unsigned i = 0; i < kLayers; ++i) {
            if (!s.present[i] || native_planes::isYuv(s.layers[i].kind))
                continue;
            Layer tiled = s.layers[i];
            tgm_layout block = {};
            ASSERT_EQ(tgm_layout_tiled(tiled.surface.width, tiled.surface.height, 4, &block), 1);
            tiled.surface.allocation = std::max<uint64_t>(tiled.surface.allocation, block.bytes);
            tiled.surface.descriptorBytes = roundUp(tiled.surface.allocation, 16384);
            Image m;
            ASSERT_TRUE(imageFor(tiled, &block, mode.width, mode.height, m)) << "swap " << n << " layer " << i;
        }
        WindowState state[kWindows];
        for (auto &w : state)
            w = {rnd(2) == 0, int(rnd(4)) - 1, int(rnd(3)) - 1};
        Plan plan;
        ASSERT_TRUE(planSwap(s.present, s.layers, s.images, state, mode.width, mode.height, plan)) << "swap " << n;
        ASSERT_EQ(plan.unshown, 0u) << "swap " << n;
        unsigned visible = 0, placed = 0;
        for (unsigned i = 0; i < kLayers; ++i)
            visible += s.present[i] && s.images[i].visible;
        for (const Placement &p : plan.window) {
            placed += p.on;
            if (p.on && s.images[p.layer].yuv && p.csc)
                ASSERT_FALSE(state[&p - plan.window].live) << "swap " << n;
            if (p.on && s.images[p.layer].yuv && !p.csc)
                ASSERT_EQ(state[&p - plan.window].matrix, int(s.images[p.layer].matrix)) << "swap " << n;
        }
        ASSERT_EQ(placed, visible) << "swap " << n;
        converted += __builtin_popcount(plan.convert);
        const uint32_t iova[kLayers] = {0x10000000, 0x14400000, 0x18800000, 0x1cc00000};
        const Frame frame = frameOf(plan, s.images, iova, 148500000);
        for (unsigned w = 0; w < kWindows; ++w) {
            const Placement &p = plan.window[w];
            if (!p.on)
                continue;
            const Image &m = s.images[p.layer];
            const bool viaVic = plan.convert & (1u << p.layer);
            const uint64_t bytes =
                viaVic ? conversionOf(s.layers[p.layer]).bytes : s.layers[p.layer].surface.allocation;
            ASSERT_LE(m.readHigh, roundUp(bytes, 4096)) << "swap " << n;
            ASSERT_EQ(m.yuv, !viaVic && native_planes::isYuv(s.layers[p.layer].kind)) << "swap " << n;
            ASSERT_EQ(frame.window[w].options & kPaletteEnable, 0u);
            ASSERT_EQ(frame.window[w].start, iova[p.layer] + uint32_t(m.start[0]));
        }
    }
    EXPECT_GT(inside, 10000u);
    EXPECT_GT(accepted, inside);
    EXPECT_GT(converted, 1000u);
}

namespace {
// Windows on every line of `mode`: the UI 1:1 on top, then two more whose sources are `fraction` times their
// destination on both axes, as far as 4096 allows, the RGB ones block-linear (the GPU's surfaces).
Frame fullScreen(const switch_dp_mode &mode, uint32_t fraction, Kind lower)
{
    Swap s;
    tgm_layout blocks[kLayers] = {};
    const tgm_layout *block[kLayers] = {};
    const Rect screen = {0, 0, mode.hactive, mode.vactive};
    const uint32_t sw = std::min<uint32_t>(mode.hactive * fraction, SWITCH_DP_MAX_WIDTH);
    const uint32_t sh = std::min<uint32_t>(mode.vactive * fraction, SWITCH_DP_MAX_HEIGHT);
    s.put(1, wholeLayer(kUi, screen));
    s.put(0, makeLayer(lower, sw, sh, {0, 0, int32_t(sw), int32_t(sh)}, screen));
    s.put(2, makeLayer(lower, sw, sh, {0, 0, int32_t(sw), int32_t(sh)}, screen));
    for (unsigned i = 0; i < kLayers; ++i) {
        if (!s.present[i] || native_planes::isYuv(s.layers[i].kind))
            continue;
        Surface &f = s.layers[i].surface;
        EXPECT_EQ(tgm_layout_tiled(f.width, f.height, 4, &blocks[i]), 1);
        block[i] = &blocks[i];
        f.allocation = std::max<uint64_t>(f.allocation, blocks[i].bytes);
        f.descriptorBytes = roundUp(f.allocation, 16384);
    }
    for (unsigned i = 0; i < kLayers; ++i)
        if (s.present[i])
            EXPECT_TRUE(imageFor(s.layers[i], block[i], mode.hactive, mode.vactive, s.images[i]));
    const Plan plan = planWindows(s.present, s.images, kIdle);
    EXPECT_EQ(plan.unshown, 0u);
    const uint32_t iova[kLayers] = {0x10000000, 0x14400000, 0x18800000, 0};
    return frameOf(plan, s.images, iova, mode.pixel_khz * 1000u);
}

// What the panel's own UI fetches meanwhile (78 MHz, 4 bytes a pixel).
constexpr uint64_t kPanelUi = 78000000ULL * 4;

bool fits(const switch_dp_mode &mode, const Frame &frame)
{
    const switch_display_memory_head timing = {1,
                                               mode.pixel_khz * 1000u,
                                               mode.hactive,
                                               mode.vactive,
                                               switch_dp_mode_htotal(&mode),
                                               switch_dp_mode_vtotal(&mode),
                                               0,
                                               0,
                                               {}};
    const switch_display_memory_head head = memoryOf(timing, frame);
    switch_display::MemoryPlan plan;
    return switch_display::memoryPlan(head, 1600000, plan) &&
           switch_display::floorKhz(head.peak_bytes_per_second + kPanelUi) <= 1600000u;
}
}  // namespace

// At the top pixel clock the DP path admits, the UI and two windows at the published scaling fit the EMC's isochronous
// ceiling (1600 MHz) and the latency allowance; at 1/2 they do not fit even at 4K60.
TEST(ExternalPlanes, WorstCaseMemoryFits)
{
    const switch_dp_mode top = {SWITCH_DP_MAX_PIXEL_KHZ, 4096, 88, 44, 172, 2880, 4, 5, 31, true, true, false};
    ASSERT_TRUE(switch_dp_source_timing(&top));
    EXPECT_TRUE(fits(top, fullScreen(top, kMinScaleFraction, kUi)));
    EXPECT_TRUE(fits(top, fullScreen(top, kMinScaleFraction, kVideo)));

    const switch_dp_mode uhd = {594000, 3840, 176, 88, 296, 2160, 8, 10, 72, true, true, false};
    ASSERT_TRUE(switch_dp_source_timing(&uhd));
    EXPECT_TRUE(fits(uhd, fullScreen(uhd, 1, kUi)));
    EXPECT_FALSE(fits(uhd, fullScreen(uhd, 2, kUi)));
}

TEST(ExternalPlanes, PublishedDictionary)
{
    struct Expected {
        const char *dictionary, *key;
        uint64_t value;
        unsigned bits;
    };

    const Expected expected[] = {
        {"DisplayPipeStrideRequirements", "StrideLinearHorizontal", 64, 32},
        {"DisplayPipePlaneBaseAlignment", "PlaneBaseAlignmentLinear", 4096, 64},
        {"IOMFBNumLayers", "MaxNumLayers", 3, 32},
        {"IOMFBScalingLimits", "RGBLayer_MinScaleFraction", 1, 32},
        {"IOMFBScalingLimits", "RGBLayer_MaxScale", 4, 32},
        {"IOMFBScalingLimits", "YUVLayer_MinScaleFraction", 1, 32},
        {"IOMFBScalingLimits", "YUVLayer_MaxScale", 4, 32},
    };
    constexpr unsigned count = sizeof(expected) / sizeof(expected[0]);
    ASSERT_EQ(sizeof(kPublished) / sizeof(kPublished[0]), count);
    for (unsigned i = 0; i < count; ++i) {
        EXPECT_STREQ(kPublished[i].dictionary, expected[i].dictionary);
        EXPECT_STREQ(kPublished[i].key, expected[i].key);
        EXPECT_EQ(kPublished[i].value, expected[i].value) << expected[i].key;
        EXPECT_EQ(kPublished[i].bits, expected[i].bits) << expected[i].key;
        for (unsigned j = 0; j + 1 < i; ++j)
            if (!strcmp(kPublished[j].dictionary, kPublished[i].dictionary))
                EXPECT_STREQ(kPublished[i - 1].dictionary, kPublished[i].dictionary);
    }
#if SWITCH_IPSW_KIND_24B119
    EXPECT_TRUE(kPackedYuvUnsupported);
#elif SWITCH_IPSW_KIND_21H461
    EXPECT_FALSE(kPackedYuvUnsupported);
#else
#error "external_planes_test is built for one IPSW kind"
#endif
}

// The GPU's block-linear BGRA, at widths whose rows are not whole GOBs: START on the first block row, the rows' bytes
// as the stride, the block height in the kind.
TEST(ExternalPlanes, BlockLinearRgb)
{
    const struct {
        uint32_t w, h, modeW, modeH;
    } cases[] = {{405, 720, 1280, 720}, {506, 900, 1600, 900}, {605, 1080, 1920, 1080}, {2560, 1440, 3440, 1440}};

    for (const auto &c : cases) {
        Layer l = wholeLayer(kUi, {int32_t((c.modeW - c.w) / 2), 0, int32_t(c.w), int32_t(c.h)});
        tgm_layout block = {};
        ASSERT_EQ(tgm_layout_tiled(c.w, c.h, 4, &block), 1);
        l.surface.allocation = std::max<uint64_t>(l.surface.allocation, block.bytes);
        l.surface.descriptorBytes = roundUp(l.surface.allocation, 16384);
        Image m;
        ASSERT_TRUE(imageFor(l, &block, c.modeW, c.modeH, m)) << c.w;
        EXPECT_EQ(m.surfaceKind, native_planes::kSurfaceKindBlock | (uint32_t(block.y_log2) << 4));
        EXPECT_EQ(m.lineStride, block.row_bytes);
        EXPECT_EQ(m.start[0], 0u);
        EXPECT_EQ(m.size, (c.h << 16) | c.w);
    }
}

TEST(ExternalPlanes, PanelPaletteNeverReachesDisplayB)
{
    switch_window_palette::setOptions(kPaletteEnable);
    Image m;
    EXPECT_TRUE(imageFor(wholeLayer(kUi, {0, 0, 1920, 1080}), nullptr, 1920, 1080, m));
    switch_window_palette::setOptions(0);
    EXPECT_EQ(m.options, native_planes::kWinEnable);
}

TEST(ExternalPlanes, LargestBuffers)
{
    Swap s;
    s.put(1, makeLayer(kUi, 4096, 4096, {0, 0, 4096, 4096}, {0, 0, 2160, 2160}));
    EXPECT_EQ(s.check(4096, 2160), native_planes::kAccepted);
    s.layers[1] = makeLayer(kUi, 4097, 2160, {0, 0, 4096, 2160}, {0, 0, 4096, 2160});
    EXPECT_EQ(s.check(4096, 2160), native_planes::kGeometry);
}
