// SPDX-License-Identifier: MIT
#include "transfer_fake.h"
#include "../zeta_cpu.h"
#include <thread>

namespace {

TEST_F(GM20BTransfer, UploadSnapshotsCallerBytesAndKeepsOwnerWithoutWaiting)
{
    std::vector<uint8_t> target(128, 0), bytes(128, 0x35);
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 32}, 8, 4, 1, 4};
    auto owner = std::make_shared<int>(19);
    std::weak_ptr<int> alive = owner;
    ASSERT_TRUE(copy(region, bytes.data(), 32, true, owner));
    owner.reset();
    EXPECT_FALSE(alive.expired());
    EXPECT_EQ(gpu.waits, 0u);
    EXPECT_EQ(gpu.visibility, gm20b::JobVisibility::Internal);
    EXPECT_EQ(target[0], 0);
    std::fill(bytes.begin(), bytes.end(), 0xe8);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, std::vector<uint8_t>(128, 0x35));
    ASSERT_EQ(gpu.uses.size(), 2u);
    EXPECT_EQ(gpu.uses[1].handle, region.texture.va);
    EXPECT_EQ(gpu.uses[1].access, uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite));
}

// A texture in a set member goes with the set: the copy job names it and no use of the texture; a write to a member the set
// only read makes it a written one first
TEST_F(GM20BTransfer, AMemberTexturesCopyNamesTheSet)
{
    std::vector<uint8_t> target(128, 0), bytes(128, 0x35);
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 32}, 8, 4, 1, 4};
    gm20b::Bo chunk;
    chunk.handle = 77;
    chunk.va = chunk.vaBase = reinterpret_cast<uint64_t>(target.data());
    chunk.bytes = target.size();
    ASSERT_TRUE(set.add(kernel, chunk, TegraGM20BSurfaceRead));
    ASSERT_TRUE(copy(region, bytes.data(), 32, true));
    EXPECT_EQ(gpu.set, 1u);
    EXPECT_EQ(gpu.registrations, 2u) << "the written member was not made a written one";
    ASSERT_EQ(gpu.uses.size(), 1u) << "the member texture was named as a use";
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, std::vector<uint8_t>(128, 0x35));
}

TEST_F(GM20BTransfer, WarmSlotsReuseOnlyAfterTheirFenceAndHandleSequenceZero)
{
    gpu.sequence = UINT32_MAX - 2;
    gpu.fence.completed = gpu.sequence;
    std::array<uint8_t, 4> target{}, bytes{1, 2, 3, 4};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    for (uint32_t i = 0; i < 12; ++i) {
        bytes[0] = uint8_t(i);
        ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    }
    EXPECT_EQ(gpu.allocations, 4u);
    EXPECT_EQ(gpu.submissions, 12u);
    EXPECT_EQ(gpu.waits, 8u);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target[0], 11);
}

TEST_F(GM20BTransfer, ReadbackWaitsAndPreservesCallerRowPadding)
{
    std::vector<uint8_t> target(128, 0x49), bytes(160, 0xab);
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 32}, 7, 4, 1, 4};
    ASSERT_TRUE(copy(region, bytes.data(), 40, false));
    EXPECT_EQ(gpu.visibility, gm20b::JobVisibility::External);
    EXPECT_EQ(gpu.waits, 1u);
    for (size_t i = 0; i < bytes.size(); ++i)
        EXPECT_EQ(bytes[i], i % 40 < 28 ? 0x49 : 0xab);
}

TEST_F(GM20BTransfer, TiledSubregionRoundTripPreservesOtherTexels)
{
    for (uint32_t bpp : {1u, 2u, 4u, 8u, 16u}) {
        SCOPED_TRACE(bpp);
        tgm_layout layout;
        ASSERT_TRUE(tgm_layout_tiled(43, 29, bpp, &layout));
        std::vector<uint8_t> tiled(layout.bytes, 0x91);
        std::vector<uint8_t> input(23 * 11 * bpp), output(input.size(), 0);
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = uint8_t(i * 23 ^ i >> 4);
        gm20b::TextureTransfer region;
        region.texture = {reinterpret_cast<uint64_t>(tiled.data()), layout.row_bytes, 9, 13, 29, true, layout.y_log2};
        region.width = 23;
        region.height = 11;
        region.planes = 1;
        region.bpp = bpp;
        ASSERT_TRUE(copy(region, input.data(), 23 * bpp, true));
        ASSERT_TRUE(copy(region, output.data(), 23 * bpp, false));
        EXPECT_EQ(input, output);
        for (uint32_t y = 0; y < 29; ++y)
            for (uint32_t x = 0; x < 43; ++x)
                if (x < 9 || x >= 32 || y < 13 || y >= 24)
                    for (uint32_t byte = 0; byte < bpp; ++byte)
                        ASSERT_EQ(tiled[tgm_layout_offset_of(layout.row_bytes, layout.y_log2, x * bpp + byte, y)],
                                  0x91);
    }
}

// A shared Depth32Float_Stencil8 texture's CPU copies move its depth alone, a float a texel (Metal's getBytes and
// replaceRegion on an M4 Max), with NVK's buffer-copy remaps of the depth aspect: into the texel's first word, the
// stencil word NO_WRITE, and out of it; Depth16Unorm, Depth32Float and Stencil8 move their texels as they are
TEST_F(GM20BTransfer, ZetaCopiesAreNvksAspectCopies)
{
    gm20b::ZetaCpuCopy copy;
    ASSERT_TRUE(gm20b::zetaCpuCopy(260, copy));
    EXPECT_EQ(copy.bytes, 4u);
    // NV90B5_SET_REMAP_COMPONENTS: nvk_remap_insert_aspect's and extract_aspect's DST_X SRC_X, DST_Y..W NO_WRITE,
    // four-byte components, the buffer's one a texel and the image's two
    EXPECT_EQ(copy.remapIn, 0x0103'6660u);
    EXPECT_EQ(copy.remapOut, 0x0013'6660u);
    for (const auto &[format, bytes] : {std::pair{250u, 2u}, {252u, 4u}, {253u, 1u}}) {
        ASSERT_TRUE(gm20b::zetaCpuCopy(format, copy)) << format;
        EXPECT_EQ(copy.bytes, bytes) << format;
        EXPECT_EQ(copy.remapIn, gm20b::copyTexelRemap(bytes)) << format;
        EXPECT_EQ(copy.remapOut, gm20b::copyTexelRemap(bytes)) << format;
    }
    EXPECT_FALSE(gm20b::zetaCpuCopy(70, copy));

    tgm_layout layout;
    ASSERT_TRUE(tgm_layout_tiled(21, 13, 8, &layout));
    std::vector<uint8_t> zeta(layout.bytes, 0x5a);
    std::vector<float> depths(9 * 5), back(depths.size(), -1.0f);
    for (size_t i = 0; i < depths.size(); ++i)
        depths[i] = float(i) / 64.0f;
    ASSERT_TRUE(gm20b::zetaCpuCopy(260, copy));
    gm20b::TextureTransfer region;
    region.texture = {reinterpret_cast<uint64_t>(zeta.data()), layout.row_bytes, 3, 4, 13, true, layout.y_log2};
    region.width = 9;
    region.height = 5;
    region.planes = 1;
    region.bpp = copy.bytes;
    region.remapIn = copy.remapIn;
    region.remapOut = copy.remapOut;
    ASSERT_TRUE(this->copy(region, depths.data(), 9 * 4, true));
    ASSERT_TRUE(this->copy(region, back.data(), 9 * 4, false));
    EXPECT_EQ(back, depths);
    for (uint32_t y = 0; y < 5; ++y)
        for (uint32_t x = 0; x < 9; ++x) {
            float word;
            const uint64_t at = tgm_layout_offset_of(layout.row_bytes, layout.y_log2, (3 + x) * 8, 4 + y);
            memcpy(&word, &zeta[at], 4);
            EXPECT_EQ(word, depths[y * 9 + x]);
            for (uint32_t b = 4; b < 8; ++b)
                EXPECT_EQ(zeta[tgm_layout_offset_of(layout.row_bytes, layout.y_log2, (3 + x) * 8 + b, 4 + y)], 0x5a)
                    << "the stencil word of texel (" << x << ", " << y << ")";
        }
}

// A shared multisampled zeta's CPU copies (Metal's rows: pixel p's sample s at texel p * samples + s, an M4 Max's 4x
// Depth32Float, Depth16Unorm and Depth32Float_Stencil8: probe/hwref/depthms.swift) move each sample to and from its
// cell of the sample grid NIL lays out (fx::sampleCell), the copy engine copying the grid's rectangle
TEST_F(GM20BTransfer, MultisampledZetaSamplesMoveThroughTheirCells)
{
    for (uint32_t samples : {2u, 4u}) {
        SCOPED_TRACE(samples);
        gm20b::fx::MipChain grid;
        ASSERT_TRUE(gm20b::fx::msLayout(10, 6, samples, 4, grid));
        std::vector<uint8_t> image(grid.bytes, 0x33);
        std::vector<uint32_t> metal(3 * 2 * samples), back(metal.size(), 0);
        for (size_t i = 0; i < metal.size(); ++i)
            metal[i] = 0x3e000000u + uint32_t(i);
        gm20b::TextureTransfer region;
        region.texture = {reinterpret_cast<uint64_t>(image.data()),
                          grid.level[0].rowBytes,
                          4u << grid.sxLog2,
                          1u << grid.syLog2,
                          6u << grid.syLog2,
                          true,
                          grid.yLog2};
        region.width = 3;
        region.height = 2;
        region.planes = 1;
        region.bpp = 4;
        region.samples = uint8_t(samples);
        region.sxLog2 = grid.sxLog2;
        region.syLog2 = grid.syLog2;
        ASSERT_TRUE(copy(region, metal.data(), 3 * samples * 4, true));
        ASSERT_TRUE(copy(region, back.data(), 3 * samples * 4, false));
        EXPECT_EQ(back, metal);
        for (uint32_t y = 0; y < 2; ++y)
            for (uint32_t p = 0; p < 3; ++p)
                for (uint32_t k = 0; k < samples; ++k) {
                    const uint8_t cell = gm20b::fx::sampleCell(samples, k);
                    const uint32_t gx = ((4 + p) << grid.sxLog2) + (cell & 3),
                                   gy = ((1 + y) << grid.syLog2) + (cell >> 2);
                    uint32_t word;
                    memcpy(&word, &image[tgm_layout_offset_of(grid.level[0].rowBytes, grid.yLog2, gx * 4, gy)], 4);
                    EXPECT_EQ(word, metal[(y * 3 + p) * samples + k]) << "pixel " << p << ", " << y << " sample " << k;
                }
    }
}

TEST_F(GM20BTransfer, FailedSubmissionIsNotPublishedOrAllowedToAlterOutput)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    gpu.reject = true;
    EXPECT_FALSE(copy(region, bytes.data(), 4, false));
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{}));
    EXPECT_EQ(gpu.submissions, 0u);
    EXPECT_EQ(gpu.waits, 0u);
}

// The kernel refuses the copy channel (the connection's channel slots all held): the copies fail, and the kernel is asked
// again only once the channel epoch moved (a channel of the connection destroyed), not at every copy
TEST_F(GM20BTransfer, ARefusedCopyChannelIsAskedForAgainOnlyOnceTheEpochMoves)
{
    std::array<uint8_t, 4> target{}, bytes{1, 2, 3, 4};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    gpu.refuseChannel = true;
    EXPECT_FALSE(copy(region, bytes.data(), 4, true));
    EXPECT_FALSE(copy(region, bytes.data(), 4, false));
    gpu.refuseChannel = false;
    EXPECT_FALSE(copy(region, bytes.data(), 4, true));
    EXPECT_EQ(gpu.channelAsks, 1u) << "a refused copy channel was asked for again with no channel destroyed since";
    __atomic_fetch_add(&gpu.channelEpoch, 1u, __ATOMIC_RELEASE);
    ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    EXPECT_EQ(gpu.channelAsks, 2u);
    EXPECT_EQ(gpu.submissions, 1u);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
}

// HW Test's failed CPU copy (failNextTransfer): the armed thread's next copy fails as a refused submission does, the
// caller's bytes and the texture untouched, nothing submitted; the arm fires once and says so
TEST_F(GM20BTransfer, AnArmedThreadsNextCopyFailsAsARefusedSubmissionOnce)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{9, 9, 9, 9};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    EXPECT_FALSE(gm20b::failNextTransfer(true));
    EXPECT_FALSE(copy(region, bytes.data(), 4, false));
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{9, 9, 9, 9}));
    EXPECT_EQ(gpu.submissions, 0u);
    ASSERT_TRUE(copy(region, bytes.data(), 4, false)) << "the arm fired more than once";
    EXPECT_EQ(bytes, target);
    EXPECT_TRUE(gm20b::failNextTransfer(true));
    EXPECT_FALSE(copy(region, bytes.data(), 4, true));
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, (std::array<uint8_t, 4>{1, 2, 3, 4}));
    EXPECT_EQ(gpu.submissions, 1u);
    EXPECT_TRUE(gm20b::failNextTransfer(false));
    std::thread([&] {
        EXPECT_FALSE(gm20b::failNextTransfer(true)) << "another thread's arm";
        gm20b::failNextTransfer(false);
    }).join();
    ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
}

TEST_F(GM20BTransfer, VolumePlanesPreserveTheirPhysicalBlockDepthAndCallerImageStride)
{
    for (bool view2d : {false, true}) {
        gm20b::fx::MipChain layout;
        ASSERT_TRUE(gm20b::fx::volumeLayout(23, 13, 9, 1, 4, view2d, layout));
        const auto &level = layout.level[0];
        std::vector<uint8_t> tiled(layout.bytes, 0xb3), input(128 * 7 * 4, 0xad), output(input.size(), 0x19);
        gm20b::TextureTransfer region;
        region.texture = {
            reinterpret_cast<uint64_t>(tiled.data()), level.rowBytes, 5, 4, 13, true, level.yLog2, 9, 3, level.zLog2};
        region.width = 17;
        region.height = 5;
        region.planes = 4;
        region.bpp = 4;
        ASSERT_TRUE(transfers.copy(kernel, set, allocationLock, region, input.data(), 128, 128 * 7, true, {}));
        ASSERT_TRUE(transfers.copy(kernel, set, allocationLock, region, output.data(), 128, 128 * 7, false, {}));
        for (uint32_t z = 0; z < 9; ++z)
            for (uint32_t y = 0; y < 13; ++y)
                for (uint32_t x = 0; x < 23; ++x)
                    for (uint32_t byte = 0; byte < 4; ++byte) {
                        const bool copied = z >= 3 && z < 7 && y >= 4 && y < 9 && x >= 5 && x < 22;
                        ASSERT_EQ(tiled[gm20b::fx::volumeOffset(layout, 0, x * 4 + byte, y, z)], copied ? 0xad : 0xb3);
                    }
        for (size_t i = 0; i < output.size(); ++i)
            EXPECT_EQ(output[i], i % (128 * 7) / 128 < 5 && i % 128 < 17 * 4 ? 0xad : 0x19);
    }
}

TEST_F(GM20BTransfer, FailedFenceKeepsQueuedStorageAndOwnerUntilQuiescentClose)
{
    std::array<uint8_t, 4> target{}, bytes{1, 2, 3, 4};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(9);
    std::weak_ptr<int> held = owner;
    for (uint32_t i = 0; i < 4; ++i)
        ASSERT_TRUE(copy(region, bytes.data(), 4, true, owner));
    owner.reset();
    gpu.fence = {4, 1, 4, 1};
    EXPECT_FALSE(held.expired());
    transfers.close(kernel, allocationLock);
    EXPECT_TRUE(held.expired());
    EXPECT_TRUE(gpu.memory.empty());
}

TEST_F(GM20BTransfer, FailedChannelCanServeAnIndependentTextureAfterQuiescence)
{
    std::array<uint8_t, 4> abandoned{}, target{}, bytes{1, 2, 3, 4};
    gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(abandoned.data()), 4}, 1, 1, 1, 4};
    ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    gpu.fence = {1, 1, 1, 1};
    region.texture.va = reinterpret_cast<uint64_t>(target.data());
    ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    gpu.complete(gpu.sequence);
    EXPECT_EQ(target, bytes);
    EXPECT_EQ(abandoned, (std::array<uint8_t, 4>{}));
}

TEST_F(GM20BTransfer, PhysicallyCompleteReadbackWaitsForSemanticSuccess)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.completeOnSubmit = true;
    gpu.semanticOnSubmit = TegraGM20BFenceSemanticPending;
    gpu.onWait = [&] {
        EXPECT_EQ(gpu.fence.completed, gpu.sequence);
        EXPECT_FALSE(held.expired());
        EXPECT_EQ(bytes, (std::array<uint8_t, 4>{}));
    };
    EXPECT_TRUE(copy(region, bytes.data(), 4, false, std::move(owner)));
    EXPECT_EQ(gpu.waits, 1u);
    EXPECT_EQ(bytes, target);
    EXPECT_TRUE(held.expired());
}

TEST_F(GM20BTransfer, PhysicallyCompleteFailedReadbackPreservesOutputAndOwner)
{
    for (uint32_t previous : {0u, UINT32_MAX}) {
        SCOPED_TRACE(previous);
        std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{9, 9, 9, 9};
        const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
        auto owner = std::make_shared<int>(7);
        std::weak_ptr<int> held = owner;
        gpu.sequence = gpu.fence.completed = previous;
        gpu.completeOnSubmit = true;
        gpu.semanticOnSubmit = TegraGM20BFenceSemanticFailed;
        EXPECT_FALSE(copy(region, bytes.data(), 4, false, std::move(owner)));
        EXPECT_EQ(gpu.waits, 0u);
        EXPECT_EQ(bytes, (std::array<uint8_t, 4>{9, 9, 9, 9}));
        EXPECT_FALSE(held.expired());
        gpu.onClose = [&] {
            EXPECT_FALSE(held.expired());
        };
        transfers.close(kernel, allocationLock);
        gpu.onClose = {};
        EXPECT_TRUE(held.expired());
    }
}

TEST_F(GM20BTransfer, FailedSemanticsCannotRetireBeforePhysicalCompletion)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{9, 9, 9, 9};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.semanticOnSubmit = TegraGM20BFenceSemanticFailed;
    gpu.onWait = [&] {
        EXPECT_LT(int32_t(gpu.fence.completed - gpu.sequence), 0);
        EXPECT_FALSE(held.expired());
        EXPECT_EQ(gpu.jobs.size(), 1u);
    };
    EXPECT_FALSE(copy(region, bytes.data(), 4, false, std::move(owner)));
    EXPECT_EQ(gpu.waits, 1u);
    EXPECT_TRUE(gpu.jobs.empty());
    EXPECT_EQ(gpu.fence.completed, gpu.sequence);
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{9, 9, 9, 9}));
    EXPECT_FALSE(held.expired());
    transfers.close(kernel, allocationLock);
    EXPECT_TRUE(held.expired());
}

TEST_F(GM20BTransfer, RefusedWaitPinsStorageUntilChannelDrain)
{
    std::array<uint8_t, 4> target{1, 2, 3, 4}, bytes{};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.refuseWait = true;
    gpu.semanticOnSubmit = TegraGM20BFenceSemanticPending;
    EXPECT_FALSE(copy(region, bytes.data(), 4, false, std::move(owner)));
    EXPECT_EQ(gpu.waits, 1u);
    EXPECT_FALSE(held.expired());
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{}));
    gpu.onClose = [&] {
        EXPECT_FALSE(held.expired());
        EXPECT_EQ(gpu.jobs.size(), 1u);
        EXPECT_FALSE(gpu.memory.empty());
    };
    transfers.close(kernel, allocationLock);
    EXPECT_TRUE(gpu.jobs.empty());
    EXPECT_TRUE(gpu.memory.empty());
    EXPECT_TRUE(held.expired());
}

TEST_F(GM20BTransfer, ByteCopyMovesExactlyItsBytesWithoutWaitingAndNamesBothRanges)
{
    const uint64_t line = gm20b::kCopyLineBytes;
    for (uint64_t bytes : {uint64_t(1), uint64_t(16384), line, line + 16, 3 * line + 512, uint64_t(1400832)}) {
        SCOPED_TRACE(bytes);
        std::vector<uint8_t> source(bytes), target(bytes + 512, 0xcc);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = uint8_t(i * 131 + (i >> 9));
        const uint64_t from = reinterpret_cast<uint64_t>(source.data()), to = reinterpret_cast<uint64_t>(target.data());
        auto owner = std::make_shared<int>(3);
        std::weak_ptr<int> held = owner;
        const uint32_t waits = gpu.waits;
        ASSERT_TRUE(transfers.copyBytes(kernel, set, allocationLock, from, to, bytes, std::move(owner)));
        EXPECT_EQ(gpu.waits, waits);
        EXPECT_EQ(gpu.visibility, gm20b::JobVisibility::Internal);
        EXPECT_FALSE(held.expired());
        EXPECT_LE(gpu.jobs.back().words, gm20b::kCopyBytesWords + SWITCH_GM20B_KIND_SWITCH_WORDS);
        ASSERT_EQ(gpu.uses.size(), 3u);
        EXPECT_EQ(gpu.uses[1].handle, from);
        EXPECT_EQ(gpu.uses[1].access, uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead |
                                               TegraGM20BSurfaceRange | TegraGM20BSurfaceHostWritten));
        EXPECT_EQ(gpu.uses[1].reserved, bytes);
        EXPECT_EQ(gpu.uses[2].handle, to);
        EXPECT_EQ(gpu.uses[2].access,
                  uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite | TegraGM20BSurfaceRange));
        EXPECT_EQ(gpu.uses[2].reserved, bytes);
        gpu.launches.clear();
        const uint32_t switches = gpu.kindSwitches;
        gpu.complete(gpu.sequence);
        EXPECT_EQ(gpu.kindSwitches, switches + 1) << "the pool image's kind-0 fill ends with a kind switch";
        ASSERT_EQ(gpu.launches.size(), size_t(bytes / line > 0) + size_t(bytes % line > 0));
        EXPECT_EQ(gpu.launches[0] & 3, uint32_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED));
        if (gpu.launches.size() > 1)
            EXPECT_EQ(gpu.launches[1] & 3, uint32_t(NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED));
        EXPECT_TRUE(std::equal(source.begin(), source.end(), target.begin()));
        EXPECT_TRUE(std::all_of(target.begin() + bytes, target.end(), [](uint8_t b) {
            return b == 0xcc;
        }));
    }
}

TEST_F(GM20BTransfer, ByteCopyRefusesWhatItCannotNameAndHoldsNothing)
{
    std::array<uint8_t, 4> source{1, 2, 3, 4}, target{};
    const uint64_t from = reinterpret_cast<uint64_t>(source.data()), to = reinterpret_cast<uint64_t>(target.data());
    auto owner = std::make_shared<int>(5);
    std::weak_ptr<int> held = owner;
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, from, to, 0, owner));
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, 0, to, 4, owner));
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, from, to, uint64_t(UINT32_MAX) + 1, owner));
    gpu.reject = true;
    EXPECT_FALSE(transfers.copyBytes(kernel, set, allocationLock, from, to, 4, owner));
    owner.reset();
    EXPECT_TRUE(held.expired());
    EXPECT_EQ(gpu.submissions, 0u);
    EXPECT_EQ(target, (std::array<uint8_t, 4>{}));
}

// A pool image's chunk is a set member: the copy into it goes with the set, naming no use of it, and makes the member a
// written one; the caller's pages, outside the set, stay a use of their own
TEST_F(GM20BTransfer, AByteCopyIntoAMemberNamesTheSet)
{
    std::vector<uint8_t> source(4096, 0x5a), target(4096, 0);
    gm20b::Bo chunk;
    chunk.handle = 78;
    chunk.va = chunk.vaBase = reinterpret_cast<uint64_t>(target.data());
    chunk.bytes = target.size();
    ASSERT_TRUE(set.add(kernel, chunk, TegraGM20BSurfaceRead));
    ASSERT_TRUE(transfers.copyBytes(kernel, set, allocationLock, reinterpret_cast<uint64_t>(source.data()),
                                    chunk.va + 512, 2048, {}));
    EXPECT_EQ(gpu.set, 1u);
    EXPECT_EQ(gpu.registrations, 2u) << "the written member was not made a written one";
    ASSERT_EQ(gpu.uses.size(), 2u) << "the member was named as a use";
    EXPECT_EQ(gpu.uses[1].handle, reinterpret_cast<uint64_t>(source.data()));
    gpu.complete(gpu.sequence);
    EXPECT_TRUE(std::all_of(target.begin() + 512, target.begin() + 2560, [](uint8_t b) {
        return b == 0x5a;
    }));
    EXPECT_EQ(target[511], 0);
    EXPECT_EQ(target[2560], 0);
}

TEST_F(GM20BTransfer, ByteCopiesAndUploadsShareTheSlotsInOrder)
{
    std::array<uint8_t, 4> source{9, 8, 7, 6}, copied{}, uploaded{}, bytes{1, 2, 3, 4};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(uploaded.data()), 4}, 1, 1, 1, 4};
    for (uint32_t i = 0; i < 6; ++i) {
        ASSERT_TRUE(transfers.copyBytes(kernel, set, allocationLock, reinterpret_cast<uint64_t>(source.data()),
                                        reinterpret_cast<uint64_t>(copied.data()), 4, {}));
        ASSERT_TRUE(copy(region, bytes.data(), 4, true));
    }
    EXPECT_EQ(gpu.allocations, 4u);
    EXPECT_EQ(gpu.submissions, 12u);
    gpu.complete(gpu.sequence);
    EXPECT_EQ(copied, source);
    EXPECT_EQ(uploaded, bytes);
}

TEST_F(GM20BTransfer, FailedUploadSlotDrainsChannelBeforeReuse)
{
    std::array<uint8_t, 4> target{}, bytes{1, 2, 3, 4};
    const gm20b::TextureTransfer region{{reinterpret_cast<uint64_t>(target.data()), 4}, 1, 1, 1, 4};
    auto owner = std::make_shared<int>(7);
    std::weak_ptr<int> held = owner;
    gpu.completeOnSubmit = true;
    for (uint32_t i = 0; i < 4; ++i)
        ASSERT_TRUE(copy(region, bytes.data(), 4, true, owner));
    owner.reset();
    gpu.semantic = {2, 0, 1, TegraGM20BFenceSemanticFailed};
    unsigned closes = 0;
    gpu.onClose = [&] {
        ++closes;
        EXPECT_FALSE(held.expired());
    };
    EXPECT_TRUE(copy(region, bytes.data(), 4, true));
    gpu.onClose = {};
    EXPECT_EQ(closes, 1u);
    EXPECT_TRUE(held.expired());
}
}  // namespace
