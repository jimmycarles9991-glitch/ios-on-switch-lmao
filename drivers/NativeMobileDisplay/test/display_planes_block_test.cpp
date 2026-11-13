// Window B on a surface the GPU renders block-linear (NativePlanes.h blockReads), on the host against TegraGM20BLayout.h.
// DC A fetches byte b of window line j from START + the block-linear offset of byte H_OFFSET + b in line V_OFFSET + j,
// LINE_STRIDE bytes a line and 2^BLOCK_HEIGHT GOBs a block: mainline dc.c programs a block-linear plane that way with START
// the buffer's base. A START on a block row is the same layout's origin for every row below it, so each byte the window
// fetches has to be the byte of the surface pixel the pitch window fetches for it, inside the bytes mapped for the window.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../NativePlanes.h"

using namespace native_planes;

namespace {

uint64_t rng = 0x2545f4914f6cdd1dull;

uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return n ? static_cast<uint32_t>(rng % n) : 0;
}

constexpr uint32_t W = 720, H = 1280;
constexpr uint64_t kSlotSpan = switch_window_caps::kPlaneSlotSpan;

uint8_t blockHeight(const Image &m)
{
    return static_cast<uint8_t>((m.surfaceKind >> kSurfaceKindBlockHeightShift) & 7);
}

// The surface offset DC A reads byte b of window line j from.
uint64_t fetched(const Image &m, uint32_t b, uint32_t j)
{
    return m.start[0] + tgm_layout_offset_of(m.lineStride, blockHeight(m), m.hOffset + b, m.vOffset + j);
}

// A BGRA surface the GPU tiled, its rows as IOSurface describes them (CA's 256-byte rows) and its memory holding the tiled
// image, and a detached layer of it within the published limits: a source anywhere in it, scaled 1/4..4x, on the screen.
Layer tiledLayer(uint32_t sw, uint32_t sh, tgm_layout &layout)
{
    const Limits limits;
    Layer l;
    l.kind = kKindBgra;
    l.surface.format = fourcc('B', 'G', 'R', 'A');
    l.surface.width = sw;
    l.surface.height = sh;
    l.surface.stride[0] = (sw * 4 + 255) / 256 * 256;
    l.surface.planeHeight[0] = sh;
    l.surface.allocation = static_cast<uint64_t>(l.surface.stride[0]) * sh;
    EXPECT_TRUE(tgm_layout_tiled(sw, sh, 4, &layout));
    const uint64_t bytes = layout.bytes > l.surface.allocation ? layout.bytes : l.surface.allocation;
    l.surface.descriptorBytes = (bytes + 16383) & ~16383ULL;
    const int32_t w = 1 + static_cast<int32_t>(rnd(limits.maxSrcRectWidth < sw ? limits.maxSrcRectWidth : sw));
    int32_t h = 1 + static_cast<int32_t>(rnd(sh));
    while (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > limits.maxSrcRectTotal)
        h = h / 2 + 1;
    l.src = {static_cast<int32_t>(rnd(sw - static_cast<uint32_t>(w) + 1)),
             static_cast<int32_t>(rnd(sh - static_cast<uint32_t>(h) + 1)), w, h};
    const auto pick = [](int32_t s) {
        const int32_t low = (s + 3) / 4;
        const int32_t high = s * 4 > 4000 ? 4000 : s * 4;
        return low >= high ? low : low + static_cast<int32_t>(rnd(static_cast<uint32_t>(high - low + 1)));
    };
    l.dst = {static_cast<int32_t>(rnd(W + 200)) - 100, static_cast<int32_t>(rnd(H + 200)) - 100, pick(w), pick(h)};
    return l;
}

// Bytes b and lines j of the window to check: its edges and a few inside.
std::vector<uint32_t> samples(uint32_t n, uint32_t step)
{
    std::vector<uint32_t> v = {0, (n - 1) / step * step};
    for (int k = 0; k < 6; ++k)
        v.push_back(rnd(n) / step * step);
    return v;
}

}  // namespace

TEST(DisplayPlanesBlock, WindowFetchesTheSurfacesOwnBytes)
{
    unsigned shown = 0;
    for (int n = 0; n < 20000; ++n) {
        tgm_layout layout = {};
        const Layer l = tiledLayer(16 + rnd(2048 - 16 + 1), 16 + rnd(2048 - 16 + 1), layout);
        ASSERT_EQ(checkDetached(l, W, H), static_cast<unsigned>(kAccepted));
        Image pitch;
        Image block;
        ASSERT_TRUE(imageOf(l, W, H, pitch));
        ASSERT_TRUE(imageOf(l, W, H, block, &layout));
        ASSERT_EQ(block.visible, pitch.visible);
        if (!block.visible)
            continue;
        ++shown;
        // The window itself is the pitch window's; only where its bytes come from differs.
        EXPECT_EQ(block.position, pitch.position);
        EXPECT_EQ(block.size, pitch.size);
        EXPECT_EQ(block.prescaled, pitch.prescaled);
        EXPECT_EQ(block.dda, pitch.dda);
        EXPECT_EQ(block.hInitial, pitch.hInitial);
        EXPECT_EQ(block.vInitial, pitch.vInitial);
        EXPECT_EQ(block.hOffset, pitch.hOffset);
        EXPECT_EQ(block.options, pitch.options);
        const uint32_t kind = kSurfaceKindBlock | static_cast<uint32_t>(layout.y_log2) << 4;
        EXPECT_EQ(static_cast<uint32_t>(block.surfaceKind), kind);
        EXPECT_EQ(static_cast<uint32_t>(pitch.surfaceKind), 0u);
        EXPECT_EQ(static_cast<uint32_t>(pitch.vOffset), 0u);
        EXPECT_EQ(block.lineStride, layout.row_bytes);
        const uint64_t blockRows = 8ULL << layout.y_log2;
        EXPECT_EQ(block.start[0] % (layout.row_bytes * blockRows), 0u);
        EXPECT_LT(static_cast<uint64_t>(block.vOffset), blockRows);
        EXPECT_LE(block.readHigh - block.readLow, kSlotSpan);
        EXPECT_LE(block.readHigh, l.surface.descriptorBytes);
        const uint32_t xFirst = pitch.hOffset / 4;
        const auto yFirst = static_cast<uint32_t>(pitch.start[0] / l.surface.stride[0]);
        const uint32_t bytes = block.prescaled & 0xffff;
        const uint32_t lines = block.prescaled >> 16;
        for (const uint32_t j : samples(lines, 1)) {
            for (const uint32_t b : samples(bytes, 4)) {
                const uint64_t got = fetched(block, b, j);
                ASSERT_EQ(got, tgm_layout_offset(&layout, xFirst * 4 + b, yFirst + j))
                    << l.surface.width << "x" << l.surface.height << " src " << l.src.x << "," << l.src.y << " byte "
                    << b << " line " << j;
                ASSERT_GE(got, block.readLow);
                ASSERT_LT(got + 3, block.readHigh);
            }
        }
    }
    std::printf("block-linear: %u windows shown\n", shown);
    EXPECT_GT(shown, 10000u);
}

// The pixels themselves: an image tiled as the GPU tiles it (tgm_layout_write) and read back the way DC A fetches the window
// gives the source rectangle's own pixels, row for row.
TEST(DisplayPlanesBlock, WindowShowsTheTiledImage)
{
    for (int n = 0; n < 60; ++n) {
        tgm_layout layout = {};
        const Layer l = tiledLayer(16 + rnd(400), 16 + rnd(400), layout);
        Image pitch;
        Image block;
        ASSERT_TRUE(imageOf(l, W, H, pitch));
        ASSERT_TRUE(imageOf(l, W, H, block, &layout));
        if (!block.visible)
            continue;
        const uint32_t sw = l.surface.width;
        const uint32_t sh = l.surface.height;
        std::vector<uint32_t> rows(static_cast<size_t>(sw) * sh);
        for (uint32_t y = 0; y < sh; ++y) {
            for (uint32_t x = 0; x < sw; ++x)
                rows[static_cast<size_t>(y) * sw + x] = x | y << 16;
        }
        std::vector<uint8_t> tiled(layout.bytes);
        tgm_layout_write(&layout, tiled.data(), 0, 0, sw, sh, reinterpret_cast<const uint8_t *>(rows.data()),
                         static_cast<uint64_t>(sw) * 4);
        const uint32_t xFirst = pitch.hOffset / 4;
        const auto yFirst = static_cast<uint32_t>(pitch.start[0] / l.surface.stride[0]);
        for (uint32_t j = 0; j < (block.prescaled >> 16); ++j) {
            for (uint32_t i = 0; i < (block.prescaled & 0xffff) / 4; ++i) {
                uint32_t pixel = 0;
                memcpy(&pixel, tiled.data() + fetched(block, i * 4, j), 4);
                ASSERT_EQ(pixel, (xFirst + i) | (yFirst + j) << 16) << "window pixel " << i << "," << j;
            }
        }
    }
}

// What window B cannot scan block-linear is not given to it: a 4:2:0 plane pair, and a layout past the surface's memory.
TEST(DisplayPlanesBlock, RefusesWhatItCannotScan)
{
    tgm_layout layout = {};
    Layer l = tiledLayer(640, 480, layout);
    l.src = {0, 0, 640, 480};
    l.dst = {0, 0, 640, 480};
    Image m;
    EXPECT_TRUE(imageOf(l, W, H, m, &layout));
    Layer short_ = l;
    short_.surface.descriptorBytes = layout.bytes - 4096 * 4;
    EXPECT_FALSE(imageOf(short_, W, H, m, &layout));
    Layer yuv = l;
    yuv.kind = kKindNv12Video;
    EXPECT_FALSE(imageOf(yuv, W, H, m, &layout));
}
