// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_plane_conversion_harness.h"

namespace {

TEST_F(PlaneConversion, ConvertsOnceAndReusesTheSameContentWithoutAllocatingOrSubmitting)
{
    const int slot = convert();
    ASSERT_GE(slot, 0);
    display->busy[slot] = true;
    EXPECT_FALSE(image.yuv);
    EXPECT_EQ(image.position, (200u << 16) | 100u);
    EXPECT_EQ(image.size, (128u << 16) | 256u);
    EXPECT_EQ(lastInput.standard, TEGRA_VIC_YCC_709);
    EXPECT_EQ(lastOperation.srcW, 128u << 16);
    EXPECT_EQ(convert(), slot);
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(conversions, 1u);
    EXPECT_EQ(display->mappings, 1u);
    EXPECT_GT(display->snapshots[slot].view.producer.value, UINT32_MAX);
}

// A turned layer (a landscape video Core Animation hands the display once rotation is published) is turned by the job:
// VIC gets the word and turns the source at its own size, and the window shows the turned plane untransformed where the
// layer goes, scaled up to the layer's twice-the-source size.
TEST_F(PlaneConversion, ATurnedVideoBecomesAnUntransformedTurnedPlane)
{
    layer.transform = 4;
    layer.dst = {100, 200, 128, 256};
    const int slot = convert();
    ASSERT_GE(slot, 0);
    EXPECT_EQ(lastOperation.transform, 4u);
    EXPECT_EQ(lastOperation.srcW, 128u << 16);
    EXPECT_EQ(lastOperation.srcH, 64u << 16);
    EXPECT_EQ(lastOperation.dstW, 64u);
    EXPECT_EQ(lastOperation.dstH, 128u);
    EXPECT_EQ(image.dda, 0x08000800u);
    EXPECT_FALSE(image.convert);
    EXPECT_EQ(image.options & 0x15u, 0u);  // no direction bits: the plane is already turned
    EXPECT_EQ(image.position, (200u << 16) | 100u);
    EXPECT_EQ(image.size, (256u << 16) | 128u);
    // The same content under another word is another plane.
    layer.transform = 7;
    advance();
    EXPECT_GE(convert(), 0);
    EXPECT_EQ(lastOperation.transform, 7u);
    EXPECT_EQ(conversions, 2u);
}

// A turned decoder picture is turned straight from its blocks from the decoder's crop on: one VIC job, no pitch copy.
TEST_F(PlaneConversion, ATurnedDecoderPictureIsReadFromItsBlocks)
{
    auto &view = display->snapshots[0].view;
    view.layout.format = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    view.layout.plane_count = 2;
    view.layout.color_matrix = 2;
    view.layout.planes[0] = {0, 256 * 64, 144, 64, 256, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    view.layout.planes[1] = {256 * 64, 256 * 32, 72, 32, 256, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    view.crop = {16, 0, 128, 64};
    layer.transform = 7;
    layer.dst = {100, 200, 128, 256};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 1u);
    EXPECT_TRUE(lastInput.blockLinear);
    EXPECT_EQ(lastInput.pitch[0], 256u);
    EXPECT_EQ(lastOperation.srcX, uint64_t(16) << 16);
    EXPECT_EQ(lastOperation.srcW, uint64_t(128) << 16);
    EXPECT_EQ(lastOperation.transform, 7u);
    EXPECT_EQ(lastOperation.dstW, 64u);
    EXPECT_EQ(lastOperation.dstH, 128u);
}

// A turned video shown small (a picture in picture at 1/5.8 across) is scaled by VIC: an RGB window stops at 1/4.
TEST_F(PlaneConversion, ASmallTurnedVideoIsScaledByVICAndShownOneToOne)
{
    layer.transform = 4;
    layer.dst = {100, 200, 11, 22};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(lastOperation.dstW, 11u);
    EXPECT_EQ(lastOperation.dstH, 22u);
    EXPECT_EQ(image.size, (22u << 16) | 11u);
    EXPECT_EQ(image.dda, 0x10001000u);
}

// A turned layer one pixel on the screen (sliding in) is a plane of VIC's smallest side, 2, which the window halves.
TEST_F(PlaneConversion, AOnePixelSliverIsAPlaneVICTakes)
{
    layer.transform = 4;
    layer.dst = {719, 200, 128, 256};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(lastOperation.dstW, 2u);
    EXPECT_EQ(lastOperation.dstH, 128u);
    EXPECT_GE(lastOperation.srcH, 1u << 16);
    EXPECT_EQ(image.position, (200u << 16) | 719u);
    EXPECT_EQ(image.size, (256u << 16) | 1u);
}

// Only the part on the screen is converted: for word 4 (transpose, flip Y) the columns cut on the left are the source's
// bottom rows.
TEST_F(PlaneConversion, AClippedTurnedLayerConvertsOnlyItsPartOnTheScreen)
{
    layer.transform = 4;
    layer.dst = {-50, 200, 128, 256};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(lastOperation.srcX, 0u);
    EXPECT_EQ(lastOperation.srcY, 0u);
    EXPECT_EQ(lastOperation.srcW, 128u << 16);
    EXPECT_EQ(lastOperation.srcH, 39u << 16);
    EXPECT_EQ(lastOperation.dstW, 39u);
    EXPECT_EQ(lastOperation.dstH, 128u);
    EXPECT_EQ(image.position, 200u << 16);
    EXPECT_EQ(image.size, (256u << 16) | 78u);
}

// A video the job converts unturned, shown small (1/5 across), is scaled by VIC too: an RGB window stops at 1/4.
TEST_F(PlaneConversion, ASmallVideoIsScaledByVICAndShownOneToOne)
{
    layer.dst = {100, 200, 25, 12};
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(lastOperation.transform, 0u);
    EXPECT_EQ(lastOperation.srcW, 128u << 16);
    EXPECT_EQ(lastOperation.dstW, 25u);
    EXPECT_EQ(lastOperation.dstH, 12u);
    EXPECT_EQ(image.size, (12u << 16) | 25u);
    EXPECT_EQ(image.dda, 0x10001000u);
}

// A GPU-tiled packed source is read in place in its blocks; one placed at an offset inside its backing is refused, as
// the layout describes the backing's first page.
TEST_F(PlaneConversion, ATiledPackedSourceIsReadInItsBlocksUnlessItIsAtAnOffset)
{
    layer = {};
    layer.kind = native_planes::kKindBgra;
    layer.surface.format = native_planes::fourcc('B', 'G', 'R', 'A');
    layer.surface.width = 64;
    layer.surface.height = 32;
    layer.surface.stride[0] = 256;
    layer.surface.planeHeight[0] = 32;
    layer.surface.allocation = layer.surface.descriptorBytes = 256 * 32;
    layer.src = {0, 0, 64, 32};
    layer.dst = {0, 0, 32, 64};
    layer.transform = 7;
    layoutAnswer = 1;
    tiledLayout = {};
    tiledLayout.width = 64;
    tiledLayout.height = 32;
    tiledLayout.bpp = 4;
    tiledLayout.row_bytes = 256;
    tiledLayout.rows = 32;
    tiledLayout.y_log2 = 2;
    tiledLayout.bytes = 256 * 32;
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(lastInput.blockLinear, 1u);
    EXPECT_EQ(lastInput.blockHeightLog2, 2u);
    display->snapshots[0].pitchOffset = 4096;
    advance();
    EXPECT_EQ(convert(), -95);
    layoutAnswer = 0;
}

// A packed RGB layer the windows cannot scan turned (scaled, or pitch-linear with a quarter turn) converts the same way.
TEST_F(PlaneConversion, APackedLayerTheWindowCannotTurnConvertsToo)
{
    layer = {};
    layer.kind = native_planes::kKindBgra;
    layer.surface.format = native_planes::fourcc('B', 'G', 'R', 'A');
    layer.surface.width = 64;
    layer.surface.height = 32;
    layer.surface.stride[0] = 256;
    layer.surface.planeHeight[0] = 32;
    layer.surface.allocation = layer.surface.descriptorBytes = 256 * 32;
    layer.src = {0, 0, 64, 32};
    layer.dst = {0, 0, 64, 128};
    layer.transform = 7;
    const int slot = convert();
    ASSERT_GE(slot, 0);
    EXPECT_EQ(lastInput.blockLinear, 0u);
    EXPECT_EQ(lastOperation.transform, 7u);
    EXPECT_EQ(lastOperation.dstW, 32u);
    EXPECT_EQ(lastOperation.dstH, 64u);
    EXPECT_EQ(image.size, (128u << 16) | 64u);
}

TEST_F(PlaneConversion, NewContentCannotOverwriteLiveConvertedScanout)
{
    const int first = convert();
    ASSERT_GE(first, 0);
    display->busy[first] = true;
    const auto *old = display->snapshots[first].view.native;
    advance();
    const int next = convert();
    ASSERT_GE(next, 0);
    EXPECT_NE(next, first);
    EXPECT_NE(display->snapshots[next].view.native, old);
    EXPECT_EQ(allocations, 2u);
    EXPECT_EQ(old->content, 1u);
}

TEST_F(PlaneConversion, RetiredBuffersAreReusedWithoutNewAllocation)
{
    const int first = convert();
    ASSERT_GE(first, 0);
    const auto *backing = display->snapshots[first].view.native;
    advance();
    const int next = convert();
    ASSERT_GE(next, 0);
    EXPECT_EQ(display->snapshots[next].view.native, backing);
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(conversions, 2u);
}

TEST_F(PlaneConversion, PoolExhaustionPreservesEveryLiveImage)
{
    for (unsigned i = 0; i < 4; ++i) {
        const int slot = convert();
        ASSERT_GE(slot, 0);
        display->busy[slot] = true;
        advance();
    }
    EXPECT_EQ(convert(), -11);
    EXPECT_EQ(allocations, 4u);
    EXPECT_EQ(conversions, 4u);
}

TEST_F(PlaneConversion, AliasOffsetAndStrideArePartOfContentIdentity)
{
    ASSERT_GE(convert(), 0);
    display->snapshots[0].pitchOffset = 4096;
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 2u);
    EXPECT_EQ(lastInput.base[0], 4096u);
    layer.surface.stride[0] = 512;
    layer.surface.descriptorBytes *= 2;
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 3u);
    EXPECT_EQ(lastInput.pitch[0], 512u);
}

TEST_F(PlaneConversion, FailedSubmissionDoesNotCreateCachedImage)
{
    convertError = -12;
    EXPECT_EQ(convert(), -12);
    EXPECT_EQ(display->mappings, 0u);
    convertError = 0;
    EXPECT_GE(convert(), 0);
    EXPECT_EQ(conversions, 2u);
    EXPECT_EQ(allocations, 1u);
}

TEST_F(PlaneConversion, AcceptedConversionSurvivesTemporaryDisplayMappingPressure)
{
    display->failMap = true;
    EXPECT_EQ(convert(), -11);
    display->failMap = false;
    EXPECT_GE(convert(), 0);
    EXPECT_EQ(conversions, 1u);
    EXPECT_EQ(allocations, 1u);
}

TEST_F(PlaneConversion, AMatrixVICLacksConvertsAsWindowBShowsIt)
{
    layer.surface.colorMatrix = SWITCH_VIC_MATRIX_UNSUPPORTED;
    layer.surface.chromaTop = layer.surface.chromaBottom = 2;
    ASSERT_EQ(offer(), -115);
    EXPECT_EQ(display->planeConversion->input.standard, uint32_t(TEGRA_VIC_YCC_601));
    work();
    EXPECT_GE(offer(), 0);
}

TEST_F(PlaneConversion, NewContentQueuesWithoutAllocationOrEngineAdmissionUnderTheDisplayGate)
{
    EXPECT_EQ(offer(), -115);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(conversions, 0u);
    EXPECT_TRUE(display->transferred);
    EXPECT_TRUE(display->planeConversionUses(0));
    EXPECT_EQ(display->event.fires, 1u);
    work();
    EXPECT_FALSE(display->transferred);
    EXPECT_FALSE(display->planeConversionUses(0));
    EXPECT_EQ(display->kicks, 1u);
    EXPECT_GE(offer(), 0);
    EXPECT_EQ(display->event.fires, 1u);
}

TEST_F(PlaneConversion, SourceCannotBeEvictedWhileRoomAdmissionRunsOutsideTheGate)
{
    display->busy[0] = false;
    ASSERT_EQ(offer(), -115);
    duringAdmission = [&] {
        EXPECT_TRUE(display->planeSlotBusy(0));
        EXPECT_TRUE(display->transferred);
        EXPECT_EQ(display->kicks, 0u);
        EXPECT_EQ(offer(), -11);
    };
    work();
    EXPECT_FALSE(display->planeSlotBusy(0));
}

TEST_F(PlaneConversion, StopBeforeDispatchReleasesTheQueuedReservationWithoutTouchingHardware)
{
    ASSERT_EQ(offer(), -115);
    display->copyStopping = true;
    work();
    EXPECT_EQ(conversions, 0u);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(display->kicks, 0u);
    EXPECT_FALSE(display->planeConversionQueued);
}

// A source whose writer's client is gone retires its swap unshown, never tried again; a lost one is the kernel's own
// failure.
TEST_F(PlaneConversion, AGoneSourceRetiresItsSwapUnshownAndALostOnePanics)
{
    convertError = TEGRA_SCANOUT_SOURCE_GONE;
    ASSERT_EQ(offer(), -115);
    work();
    EXPECT_EQ(display->kicks, 0u);
    EXPECT_EQ(display->unshown, 1u);
    EXPECT_EQ(display->taken, 1u);
    EXPECT_FALSE(display->planeNextSet);
    EXPECT_FALSE(display->convertedPlanes[0].valid);
    convertError = TEGRA_SCANOUT_SOURCE_LOST;
    ASSERT_EQ(offer(), -115);
    offGate = true;
    EXPECT_THROW(display->planeConversionWork(), std::runtime_error);
    EXPECT_EQ(display->unshown, 1u);
}

TEST_F(PlaneConversion, UnavailableWorkerDoesNotAllocateOrAdmit)
{
    display->copySource = nullptr;
    EXPECT_EQ(offer(), -19);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(conversions, 0u);
}
}  // namespace
