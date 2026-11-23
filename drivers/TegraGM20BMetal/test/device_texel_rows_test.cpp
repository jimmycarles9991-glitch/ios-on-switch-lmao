// SPDX-License-Identifier: MIT
// A texel buffer over a surface the kernel laid out block-linear (texel_rows.h): a texel buffer header over a pitch
// pool of its bytes, which the copy engine fills from the surface's rows and copies back into them, a part of a row at each
// end and each row's texels alone; its CPU copies move its bytes between the caller's and the rows.
#include "device_harness.h"
#include "../push.h"
#include "../surface_refusal.h"
#include "../texel_rows.h"
#include <cstdlib>
#include <set>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint64_t kSurface = 7;
// a 64x16 RGBA8 surface of rows 320 bytes apart: 256 bytes of texels a row, 64 of padding
constexpr uint32_t kWidth = 64, kHeight = 16, kRows = 320, kTexels = kWidth * 4;

// A copy-engine launch: the methods it was given, as the engine holds them when LAUNCH_DMA comes
struct Launch {
    uint64_t in = 0, out = 0;
    uint32_t length = 0, lines = 0, pitchIn = 0, pitchOut = 0, srcOrigin = 0, dstOrigin = 0, remap = 0, launch = 0;

    bool fromBlockLinear() const
    {
        return !(launch >> 7 & 1);
    }

    bool intoBlockLinear() const
    {
        return !(launch >> 8 & 1);
    }

    bool copy() const
    {
        return length && (remap & 7) != NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A;
    }

    // its texels' element: SET_REMAP_COMPONENTS COMPONENT_SIZE x NUM_SRC_COMPONENTS
    uint32_t unit() const
    {
        return ((remap >> 16 & 3) + 1) * ((remap >> 20 & 3) + 1);
    }
};

std::vector<Launch> launches(size_t first)
{
    std::vector<Launch> out;
    Launch at;
    const auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc != gm20b::SUBC_COPY)
            return;
        switch (m) {
        case NVB0B5_OFFSET_IN_UPPER:
            at.in = (at.in & 0xffffffffull) | uint64_t(v) << 32;
            break;
        case NVB0B5_OFFSET_IN_LOWER:
            at.in = (at.in & ~0xffffffffull) | v;
            break;
        case NVB0B5_OFFSET_OUT_UPPER:
            at.out = (at.out & 0xffffffffull) | uint64_t(v) << 32;
            break;
        case NVB0B5_OFFSET_OUT_LOWER:
            at.out = (at.out & ~0xffffffffull) | v;
            break;
        case NVB0B5_PITCH_IN:
            at.pitchIn = v;
            break;
        case NVB0B5_PITCH_OUT:
            at.pitchOut = v;
            break;
        case NVB0B5_LINE_LENGTH_IN:
            at.length = v;
            break;
        case NVB0B5_LINE_COUNT:
            at.lines = v;
            break;
        case NVB0B5_SET_SRC_ORIGIN:
            at.srcOrigin = v;
            break;
        case NVB0B5_SET_DST_ORIGIN:
            at.dstOrigin = v;
            break;
        case NVB0B5_SET_REMAP_COMPONENTS:
            at.remap = v;
            break;
        case NVB0B5_LAUNCH_DMA:
            at.launch = v;
            out.push_back(at);
            break;
        default:
            break;
        }
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            if (op == 4) {
                method(subc, m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(subc, op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i]);
        }
    }
    return out;
}

// The surface bytes (row, byte of it) the copies into the block-linear image wrote
std::set<std::pair<uint32_t, uint32_t>> written(const std::vector<Launch> &all)
{
    std::set<std::pair<uint32_t, uint32_t>> out;
    for (const Launch &l : all)
        if (l.copy() && l.intoBlockLinear())
            for (uint32_t y = 0; y < l.lines; y++)
                for (uint32_t b = 0; b < l.length * l.unit(); b++)
                    out.insert({(l.dstOrigin >> 16) + y, (l.dstOrigin & 0xffff) * l.unit() + b});
    return out;
}

class DeviceTexelRows : public harness::DeviceTest {
protected:
    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.infoFlags = TegraGM20BInfoSurfaceLayout;
        kernel_model::model.surfaceLayout = TegraGM20BLayoutTiled;
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
        ASSERT_NE(base, nullptr);
        ASSERT_TRUE(
            dev.createSurfaceTexture(kSurface, base, bytes, nullptr, 0, kWidth, kHeight, kRows, kRGBA8, image, true));
        ASSERT_TRUE(image.blockLinear && image.import->kernelTiled()) << "the kernel tiled it for the target";
    }

    void TearDown() override
    {
        free(base);
        DeviceTest::TearDown();
    }

    bool texelBuffer(uint64_t offset, uint32_t elements, gm20b::Texture &t, uint32_t rows = kRows)
    {
        gm20b::surfaceRefusal = nullptr;
        return dev.createSurfaceTexelBuffer(kSurface, base, bytes, nullptr, offset, rows, elements, kRGBA8, t);
    }

    const size_t bytes = 2 * size_t(getpagesize());
    void *base = aligned_alloc(size_t(getpagesize()), bytes);
    gm20b::Texture image;
};

TEST_F(DeviceTexelRows, ItIsATexelBufferHeaderOverAPoolOfItsBytes)
{
    gm20b::Texture t;
    ASSERT_TRUE(texelBuffer(160, 300, t)) << (gm20b::surfaceRefusal ? gm20b::surfaceRefusal : "");
    ASSERT_NE(t.texelRows, nullptr);
    EXPECT_EQ(t.texelRows->import, image.import);
    EXPECT_EQ(t.texelRows->offset, 160u);
    EXPECT_EQ(t.texelRows->bytesPerRow, kRows);
    const std::array<uint32_t, 8> h = gm20b::DeviceHarness::header(dev, t.tic);
    EXPECT_EQ(h[4] >> 23 & 0xf, 6u) << "TEXTURE_TYPE ONE_D_BUFFER";
    EXPECT_EQ(h[4] & 0xffff, 299u) << "WIDTH_MINUS_ONE_BITS15TO0";
    EXPECT_EQ(h[1] | uint64_t(h[2] & 0xffff) << 32, t.va) << "ADDRESS: its pool";
    const gm20b::Bo &surface = image.import->bo;
    EXPECT_TRUE(t.va + 1200 <= surface.va || t.va >= surface.va + surface.bytes)
        << "memory of its own: a ONE_D_BUFFER header reads no GOBs";
    EXPECT_EQ(t.cpu, nullptr) << "the CPU reaches its bytes through the copy engine alone";
    dev.destroyTexture(t);
    dev.destroyTexture(image);
}

TEST_F(DeviceTexelRows, ItHoldsTheSurfacesImportUntilItGoes)
{
    gm20b::Texture t;
    ASSERT_TRUE(texelBuffer(0, 64, t));
    const uint64_t handle = image.import->bo.handle;
    dev.destroyTexture(image);
    EXPECT_FALSE(freed(handle)) << "its rows are still the texel buffer's";
    dev.destroyTexture(t);
    EXPECT_TRUE(freed(handle));
}

// Bytes [160, 1360): the last 96 bytes of row 0's texels, rows 1 to 3's and the first 80 of row 4's, each way in 16-byte
// elements; the pool zeroed before the fill, as the rows' padding (64 bytes of rows 0 to 3) has no storage; the copies back
// write the range's texels and no other byte of the surface
TEST_F(DeviceTexelRows, ItsPoolIsFilledAndCopiedBackAPartOfARowAtEachEnd)
{
    gm20b::Texture t;
    ASSERT_TRUE(texelBuffer(160, 300, t));
    const size_t first = kernel_model::model.jobs.size();
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    for (const bool fill : {true, false}) {
        gm20b::BlitEncoder *b = cb->blit();
        gm20b::moveTexelRows(*b, t, fill);
        b->end();
    }
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const std::vector<Launch> all = launches(first);
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all[0].remap & 7, uint32_t(NVB0B5_SET_REMAP_COMPONENTS_DST_X_CONST_A)) << "the pool zeroed first";
    EXPECT_EQ(all[0].out, t.va);
    EXPECT_EQ(uint64_t(all[0].length) * all[0].lines * 4, 1200u);
    const uint64_t surface = image.import->bo.va;

    struct Part {
        uint32_t x, y, length, lines;
        uint64_t at;
    };

    const Part parts[3] = {{10, 0, 6, 1, 0}, {0, 1, 16, 3, 160}, {0, 4, 5, 1, 1120}};
    std::vector<const Launch *> fills, backs;
    for (const Launch &l : all)
        if (l.copy())
            (l.fromBlockLinear() ? fills : backs).push_back(&l);
    ASSERT_EQ(fills.size(), 3u);
    ASSERT_EQ(backs.size(), 3u);
    for (uint32_t k = 0; k < 3; k++) {
        const Launch &f = *fills[k], &b = *backs[k];
        const Part &p = parts[k];
        EXPECT_FALSE(f.intoBlockLinear()) << k;
        EXPECT_EQ(f.in, surface) << k;
        EXPECT_EQ(f.srcOrigin, p.x | p.y << 16) << k;
        EXPECT_EQ(f.out, t.va + p.at) << k;
        EXPECT_EQ(f.pitchOut, kRows) << k;
        EXPECT_TRUE(b.intoBlockLinear()) << k;
        EXPECT_EQ(b.in, t.va + p.at) << k;
        EXPECT_EQ(b.pitchIn, kRows) << k;
        EXPECT_EQ(b.out, surface) << k;
        EXPECT_EQ(b.dstOrigin, p.x | p.y << 16) << k;
        for (const Launch *l : {&f, &b}) {
            EXPECT_EQ(l->length, p.length) << k;
            EXPECT_EQ(l->lines, p.lines) << k;
            EXPECT_EQ(l->unit(), 16u) << k;
        }
    }
    std::set<std::pair<uint32_t, uint32_t>> range;
    for (uint64_t linear = 160; linear < 1360; linear++)
        if (linear % kRows < kTexels)
            range.insert({uint32_t(linear / kRows), uint32_t(linear % kRows)});
    EXPECT_EQ(written(all), range);
    dev.destroyTexture(t);
    dev.destroyTexture(image);
}

// getBytes and replaceRegion of texels [10, 60) (bytes [200, 400) of the surface): the copy engine moves the 56 bytes of row
// 0's texels from byte 200 in 8-byte elements and the first 80 of row 1 in 16-byte ones, through the transfer channel; the
// 64 bytes of row 0's padding between them read 0
TEST_F(DeviceTexelRows, ItsCpuCopiesMoveItsBytesBetweenTheCallerAndTheRows)
{
    gm20b::Texture t;
    ASSERT_TRUE(texelBuffer(160, 300, t));
    const uint64_t surface = image.import->bo.va;
    const uint32_t origin[3] = {10, 0, 0}, size[3] = {50, 1, 1};
    for (const bool write : {false, true}) {
        std::vector<uint8_t> bytes(200, 0xab);
        const size_t first = kernel_model::model.jobs.size();
        EXPECT_EQ(dev.transferTexture(t, 0, 0, origin, size, bytes.data(), 0, 0, write, nullptr),
                  gm20b::TextureTransferResult::Complete)
            << write;
        std::vector<Launch> moved;
        for (const Launch &l : launches(first))
            if (l.copy())
                moved.push_back(l);
        ASSERT_EQ(moved.size(), 2u) << write;
        for (uint32_t k = 0; k < 2; k++) {
            const Launch &l = moved[k];
            EXPECT_EQ(write ? l.intoBlockLinear() : l.fromBlockLinear(), true) << write << k;
            EXPECT_EQ(write ? l.out : l.in, surface) << write << k;
            EXPECT_EQ(write ? l.dstOrigin : l.srcOrigin, k ? 1u << 16 : 25u) << write << k;
            EXPECT_EQ(l.length, k ? 5u : 7u) << write << k;
            EXPECT_EQ(l.unit(), k ? 16u : 8u) << write << k;
        }
        if (!write)
            for (size_t b = 56; b < 120; b++)
                EXPECT_EQ(bytes[b], 0) << "padding byte " << b;
    }
    dev.destroyTexture(t);
    dev.destroyTexture(image);
}

}  // namespace
