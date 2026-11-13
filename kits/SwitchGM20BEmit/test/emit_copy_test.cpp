// SPDX-License-Identifier: MIT
// switch_gm20b_copy_rect against the copy emitters the drivers had before they moved onto the kit, for the same copies:
// word for word against the raw path's (raw_copy.h's emitCopy, kept here as handCopy), with every remap form raw uses,
// and method for method against the kernel's hand-encoded row and relayout copies, whose launch took two words where
// the immediate form takes one. raw's emitCopy (raw_kit.h) is then the kit's, word for word.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BCopyPush.h"
#include "drivers/TegraGM20B/TegraGM20BLayout.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_kit.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitCopy.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Methods;
using switch_gm20b_test::Words;

struct Side {
    uint64_t va;
    uint32_t pitch;
    bool tiled;
    uint32_t rowTexels, height, depth, layer, x, y;
    uint8_t blockHeight, blockDepth;
};

Side pitch(uint64_t va, uint32_t pitch)
{
    return {va, pitch, false, 0, 0, 0, 0, 0, 0, 0, 0};
}

Side tiled(uint64_t va, uint32_t rowTexels, uint32_t height, uint32_t depth, uint32_t layer, uint32_t x, uint32_t y,
           uint8_t blockHeight, uint8_t blockDepth)
{
    return {va, 0, true, rowTexels, height, depth, layer, x, y, blockHeight, blockDepth};
}

// raw's sideOf: a block-linear side's pitch is its row stride
raw::CopySurface rawSide(const Side &s, uint32_t bpp)
{
    raw::CopySurface c;
    c.va = s.va;
    c.pitch = s.tiled ? s.rowTexels * bpp : s.pitch;
    c.blockLinear = s.tiled;
    c.height = s.height;
    c.depth = s.depth;
    c.layer = s.layer;
    c.x = s.x;
    c.y = s.y;
    c.blockHeight = s.blockHeight;
    c.blockDepth = s.blockDepth;
    return c;
}

switch_gm20b_copy_surface kitSide(const Side &s, uint32_t bpp)
{
    switch_gm20b_copy_surface c = {};
    c.base_addr = s.va;
    c.bpp = bpp;
    c.tiled = s.tiled;
    c.is_3d = true;
    c.row_stride = s.tiled ? s.rowTexels * bpp : s.pitch;
    c.extent_el.height = s.height;
    c.extent_el.depth = s.depth;
    c.offset_el.x = s.tiled ? s.x : 0;
    c.offset_el.y = s.tiled ? s.y : 0;
    c.offset_el.z = s.layer;
    c.y_log2 = s.blockHeight;
    c.z_log2 = s.blockDepth;
    return c;
}

// raw_copy.h's emitCopy, the raw path's copy before the kit's; a block-linear side's width is its row stride in texels
void handCopy(raw::Push &p, const raw::CopySurface &source, uint32_t sourceWidth, const raw::CopySurface &destination,
              uint32_t destinationWidth, uint32_t width, uint32_t height, uint32_t remap, bool ordered)
{
    p.m(0x0708, remap, raw::SUBC_COPY);
    p.inc(0x0400,
          {uint32_t(source.va >> 32), uint32_t(source.va), uint32_t(destination.va >> 32), uint32_t(destination.va),
           source.pitch, destination.pitch, width, height},
          raw::SUBC_COPY);
    uint32_t launch = (ordered ? 2u : 1u) | 1u << 2 | 1u << 9 | 1u << 10;
    const auto surface = [&](const raw::CopySurface &s, uint32_t sideWidth, uint32_t method, uint32_t pitchBit) {
        if (s.blockLinear)
            p.inc(method,
                  {uint32_t(s.blockHeight) << 4 | uint32_t(s.blockDepth) << 8 | 1u << 12, sideWidth, s.height, s.depth,
                   s.layer, s.y << 16 | s.x},
                  raw::SUBC_COPY);
        else
            launch |= pitchBit;
    };
    surface(source, sourceWidth, 0x0728, 1u << 7);
    surface(destination, destinationWidth, 0x070c, 1u << 8);
    p.m(0x0300, launch, raw::SUBC_COPY);
}

constexpr uint32_t remapOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t src, uint32_t dst)
{
    return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (src - 1) << 20 | (dst - 1) << 24;
}

struct Case {
    const char *name;
    Side src, dst;
    uint32_t bpp, width, height;
    bool ordered;
};

// The words the kernel hand-encoded for tgm_copy_rows and tgm_relayout_copy, for the inputs below
constexpr uint32_t kHandRows[] = {0x20088100, 0x00000000, 0x12340000, 0x00000000, 0x56780000, 0x00001000,
                                  0x00002000, 0x00000f00, 0x00000438, 0x200180c0, 0x00000386};
constexpr uint32_t kHandToRows[] = {0x200181c2, 0x00033210, 0x20088100, 0x00000000, 0x80000000, 0x00000000, 0x90000000,
                                    0x00001400, 0x00001400, 0x00000500, 0x000002d0, 0x200681ca, 0x00001040, 0x00000500,
                                    0x000002d0, 0x00000001, 0x00000000, 0x00000000, 0x200180c0, 0x00000706};
constexpr uint32_t kHandToTiled[] = {0x200181c2, 0x00033210, 0x20088100, 0x00000000, 0x90000000, 0x00000000, 0x80000000,
                                     0x00001400, 0x00001400, 0x00000500, 0x000002d0, 0x200681c3, 0x00001040, 0x00000500,
                                     0x000002d0, 0x00000001, 0x00000000, 0x00000000, 0x200180c0, 0x00000686};

template <size_t N> std::vector<uint32_t> hand(const uint32_t (&words)[N])
{
    return std::vector<uint32_t>(words, words + N);
}

const Case kCases[] = {
    {"pitch to pitch", pitch(0x1000000, 1024), pitch(0x2000040, 2048), 4, 256, 64, false},
    {"pitch to block-linear", pitch(0x1000000, 512), tiled(0x40000000, 256, 128, 1, 0, 8, 16, 4, 0), 4, 64, 32, true},
    {"block-linear to pitch, 8-byte texels", tiled(0x3400000000, 128, 64, 1, 0, 0, 8, 3, 0), pitch(0x5000, 1024), 8,
     128, 8, false},
    {"block-linear to block-linear, 3D", tiled(0x10000, 64, 64, 8, 3, 4, 4, 2, 1),
     tiled(0x20000, 64, 64, 8, 5, 0, 0, 2, 1), 16, 32, 32, true},
    {"one-byte texels", pitch(0x7000, 4096), tiled(0x8000, 4096, 16, 1, 0, 0, 0, 1, 0), 1, 4096, 16, false},
    {"two-byte texels", tiled(0x9000, 640, 480, 1, 0, 32, 32, 4, 0), pitch(0xa000, 1280), 2, 320, 240, true},
};

TEST(EmitCopy, WordForWordTheRawPathsCopy)
{
    for (const Case &c : kCases) {
        std::vector<uint32_t> ours(SWITCH_GM20B_COPY_RECT_WORDS(1)), theirs(64);
        switch_gm20b_copy copy = {};
        copy.src = kitSide(c.src, c.bpp);
        copy.dst = kitSide(c.dst, c.bpp);
        copy.remap.comp_size = uint8_t(c.bpp < 4 ? c.bpp : 4);
        for (uint8_t i = 0; i < 4; i++)
            copy.remap.dst[i] = i;
        copy.extent_el.width = c.width;
        copy.extent_el.height = c.height;
        copy.extent_el.depth = copy.extent_el.array_len = 1;
        nv_push p;
        nv_push_init(&p, ours.data(), ours.size(), SUBC_MASK_ALL);
        switch_gm20b_copy_rect(&p, &copy,
                               c.ordered ? NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED
                                         : NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED);
        ours.resize(nv_push_dw_count(&p));

        raw::Push r{theirs.data(), theirs.data() + theirs.size()};
        handCopy(r, rawSide(c.src, c.bpp), c.src.rowTexels, rawSide(c.dst, c.bpp), c.dst.rowTexels, c.width, c.height,
                 raw::copyTexelRemap(c.bpp), c.ordered);
        theirs.resize(size_t(r.at - theirs.data()));

        EXPECT_EQ(Words(ours), Words(theirs)) << c.name;
    }
}

// Every remap the raw path copies with (raw_gpu.cpp copyBufferTexels, raw_shader_tests.h): texels of size x count bytes
// on each side, components NO_WRITE (6) or moved between sides of different component counts
struct RemapCase {
    const char *name;
    uint32_t remap;
    bool tiledSource;
};

const RemapCase kRemaps[] = {
    {"depth word into a packed texel", remapOf(0, 6, 6, 6, 4, 1, 2), false},
    {"depth word out of a packed texel", remapOf(0, 6, 6, 6, 4, 2, 1), true},
    {"stencil byte into the temp", remapOf(0, 6, 6, 6, 1, 1, 2), false},
    {"temp into a packed texel's third two-byte component", remapOf(6, 6, 0, 6, 2, 1, 4), false},
    {"a packed texel's third two-byte component into the temp", remapOf(2, 6, 6, 6, 2, 4, 1), true},
    {"temp's first byte out", remapOf(0, 6, 6, 6, 1, 2, 1), true},
    {"depth to depth", remapOf(0, 6, 6, 6, 4, 1, 1), true},
    {"stencil to stencil", remapOf(0, 6, 6, 6, 1, 1, 1), false},
    {"XR color word out", 3u << 16 | 1u << 20, true},
    {"XR color word in, alpha word kept", 6u << 4 | 3u << 16 | 1u << 24, false},
};

TEST(EmitCopy, RawCopiesAreTheHandEncodedOnes)
{
    std::vector<Case> cases(std::begin(kCases), std::end(kCases));
    for (const Case &c : cases)
        for (const bool ordered : {false, true}) {
            std::vector<uint32_t> ours(SWITCH_GM20B_COPY_RECT_WORDS(1)), theirs(64);
            raw::Push o{ours.data(), ours.data() + ours.size()}, t{theirs.data(), theirs.data() + theirs.size()};
            const uint32_t remap = raw::copyTexelRemap(c.bpp);
            raw::emitCopy(o, rawSide(c.src, c.bpp), rawSide(c.dst, c.bpp), c.width, c.height, remap, ordered);
            handCopy(t, rawSide(c.src, c.bpp), c.src.rowTexels, rawSide(c.dst, c.bpp), c.dst.rowTexels, c.width,
                     c.height, remap, ordered);
            ours.resize(size_t(o.at - ours.data()));
            theirs.resize(size_t(t.at - theirs.data()));
            EXPECT_EQ(Words(ours), Words(theirs)) << c.name;
        }
    for (const RemapCase &c : kRemaps) {
        const uint32_t size = (c.remap >> 16 & 3) + 1, srcBpp = size * ((c.remap >> 20 & 3) + 1),
                       dstBpp = size * ((c.remap >> 24 & 3) + 1);
        const Side block = tiled(0x3400000000, 96, 40, 1, 2, 4, 8, 3, 0), rows = pitch(0x5000040, 4096);
        const Side &src = c.tiledSource ? block : rows, &dst = c.tiledSource ? rows : block;
        std::vector<uint32_t> ours(SWITCH_GM20B_COPY_RECT_WORDS(1)), theirs(64);
        raw::Push o{ours.data(), ours.data() + ours.size()}, t{theirs.data(), theirs.data() + theirs.size()};
        raw::emitCopy(o, rawSide(src, srcBpp), rawSide(dst, dstBpp), 33, 17, c.remap, false);
        handCopy(t, rawSide(src, srcBpp), src.rowTexels, rawSide(dst, dstBpp), dst.rowTexels, 33, 17, c.remap, false);
        ours.resize(size_t(o.at - ours.data()));
        theirs.resize(size_t(t.at - theirs.data()));
        EXPECT_EQ(Words(ours), Words(theirs)) << c.name;
    }
}

TEST(EmitCopy, MethodForMethodTheKernelsRowCopy)
{
    std::vector<uint32_t> kernel(TGM_COPY_DWORDS), ours(SWITCH_GM20B_COPY_RECT_WORDS(1));
    kernel.resize(tgm_copy_rows(kernel.data(), 0x12340000, 4096, 0x56780000, 8192, 3840, 1080));
    const std::vector<uint32_t> theirs = hand(kHandRows);

    switch_gm20b_copy copy = {};
    copy.src.base_addr = 0x12340000;
    copy.src.row_stride = 4096;
    copy.dst.base_addr = 0x56780000;
    copy.dst.row_stride = 8192;
    copy.src.bpp = copy.dst.bpp = 1;
    copy.extent_el.width = 3840;
    copy.extent_el.height = 1080;
    copy.extent_el.depth = copy.extent_el.array_len = 1;
    nv_push p;
    nv_push_init(&p, ours.data(), ours.size(), SUBC_MASK_ALL);
    switch_gm20b_copy_rect(&p, &copy, NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED);
    ours.resize(nv_push_dw_count(&p));

    EXPECT_EQ(Words(kernel), Words(ours));
    EXPECT_EQ(Methods(ours), Methods(theirs));
    EXPECT_EQ(ours.size() + 1, theirs.size()) << "the hand-encoded LAUNCH_DMA took a header and a word";
}

TEST(EmitCopy, MethodForMethodTheKernelsRelayout)
{
    tgm_layout l = {};
    ASSERT_TRUE(tgm_layout_tiled(1280, 720, 4, &l));
    for (int toTiled = 0; toTiled < 2; toTiled++) {
        const uint64_t tiled = 0x80000000, rows = 0x90000000;
        const uint32_t pitch = 1280 * 4;
        std::vector<uint32_t> kernel(TGM_RELAYOUT_DWORDS), ours(SWITCH_GM20B_COPY_RECT_WORDS(1));
        kernel.resize(toTiled ? tgm_relayout_copy(kernel.data(), &l, rows, 0, tiled, 1, pitch)
                              : tgm_relayout_copy(kernel.data(), &l, tiled, 1, rows, 0, pitch));
        const std::vector<uint32_t> theirs = toTiled ? hand(kHandToTiled) : hand(kHandToRows);

        switch_gm20b_copy_surface block = {};
        block.base_addr = tiled;
        block.bpp = 4;
        block.tiled = true;
        block.row_stride = l.row_bytes;
        block.extent_el.height = l.height;
        block.extent_el.depth = 1;
        block.y_log2 = l.y_log2;
        switch_gm20b_copy_surface linear = {};
        linear.base_addr = rows;
        linear.bpp = 4;
        linear.row_stride = pitch;
        switch_gm20b_copy copy = {};
        copy.src = toTiled ? linear : block;
        copy.dst = toTiled ? block : linear;
        copy.remap.comp_size = 4;
        for (uint8_t i = 0; i < 4; i++)
            copy.remap.dst[i] = i;
        copy.extent_el.width = l.width;
        copy.extent_el.height = l.height;
        copy.extent_el.depth = copy.extent_el.array_len = 1;
        nv_push p;
        nv_push_init(&p, ours.data(), ours.size(), SUBC_MASK_ALL);
        switch_gm20b_copy_rect(&p, &copy, NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED);
        ours.resize(nv_push_dw_count(&p));

        EXPECT_EQ(Words(kernel), Words(ours)) << (toTiled ? "to tiled" : "to rows");
        EXPECT_EQ(Methods(ours), Methods(theirs)) << (toTiled ? "to tiled" : "to rows");
        EXPECT_EQ(ours.size() + 1, theirs.size()) << (toTiled ? "to tiled" : "to rows");
    }
}

}  // namespace
