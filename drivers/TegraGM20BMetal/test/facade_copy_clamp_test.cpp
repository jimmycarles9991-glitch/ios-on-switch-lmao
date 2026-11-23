// SPDX-License-Identifier: MIT
// A GPU copy into or out of a BGRA10_XR texture Metal keeps compressed leaves its destination clamped as an M4 Max does
// (TegraGM20BMetalCopyClamp.h): after the copy engine's copy, the destination's region goes into a scratch and one pass of the pack's
// clamp state draws it back. A copy of a texture Metal does not compress is the copy engine's one copy alone.
#include "facade_pass_scene.h"
#include "../push.h"

namespace {

using namespace pass_scene;

constexpr NSUInteger kSide = 64;

id<MTLDevice> clampDevice()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                          {{"raw.xr.xr_bgra10_frag", MTLPixelFormatRGBA16Float},
                           {"raw.xr.xr_copy_clamp_frag", MTLPixelFormatRGBA16Uint}}));
}

id<MTLTexture> xrTexture(id<MTLDevice> device, bool optimized, uint32_t format = 552,
                         MTLStorageMode storage = MTLStorageModePrivate, NSUInteger slices = 1)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    if (slices > 1) {
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = slices;
    }
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = storage;
    d.allowGPUOptimizedContents = optimized;
    return [device newTextureWithDescriptor:d];
}

// The copy engine's launches that move bytes and the draws (calls of a draw macro, mme.h) of the jobs since `first`. A
// launch of DATA_TRANSFER_TYPE (1:0) NONE or of an empty line (BlitEncoder::barrier's) moves none.
struct Work {
    unsigned launches = 0, draws = 0;
};

size_t jobCount()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return kernel_model::model.jobs.size();
}

Work workSince(size_t first)
{
    constexpr uint32_t kLaunchDma = 0x0300, kLineLengthIn = 0x0418;  // NVB0B5_LAUNCH_DMA, NVB0B5_LINE_LENGTH_IN
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    Work out;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(w.size());
        const long count = switch_gm20b_push_decode(w.data(), w.size(), methods.data(), methods.size());
        EXPECT_GE(count, 0);
        uint32_t lineLength = 0;
        for (long i = 0; i < count; i++) {
            const switch_gm20b_method &m = methods[i];
            if (m.subchannel == gm20b::SUBC_COPY && m.method == kLineLengthIn)
                lineLength = m.value;
            else if (m.subchannel == gm20b::SUBC_COPY && m.method == kLaunchDma)
                out.launches += lineLength && (m.value & 3);
            else if (m.subchannel == gm20b::SUBC_3D && m.method >= gm20b::mme::kMacroCall &&
                     (m.method - gm20b::mme::kMacroCall) % 8 == 0)
                out.draws += gm20b::mme::countedAsDraw((m.method - gm20b::mme::kMacroCall) / 8);
        }
    }
    return out;
}

// The work of the second of two command buffers of `encode`'s blit encoder: the first one to name a new Private texture
// also writes its first contents ahead of its commands (TegraGM20BMetalFresh.h), and draws as the second one does
Work secondRun(id<MTLDevice> device, void (^encode)(id<MTLBlitCommandEncoder>))
{
    size_t first = 0;
    unsigned firstDraws = 0;
    for (int n = 0; n < 2; n++) {
        first = jobCount();
        id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        encode(e);
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        expectRaw(device, cb);
        if (!n)
            firstDraws = workSince(first).draws;
    }
    const Work second = workSince(first);
    EXPECT_EQ(firstDraws, second.draws) << "the command buffer that wrote a new texture's first contents";
    return second;
}

// One texture's texels copied into another's first slice
Work copiedBetween(id<MTLDevice> device, id<MTLTexture> from, id<MTLTexture> to)
{
    return secondRun(device, ^(id<MTLBlitCommandEncoder> e) {
        [e copyFromTexture:from
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(kSide, kSide, 1)
                    toTexture:to
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    });
}

// A buffer's texels copied into the texture, or the texture's into a buffer
Work copied(id<MTLDevice> device, id<MTLTexture> t, bool intoTexture)
{
    id<MTLBuffer> b = [device newBufferWithLength:kSide * kSide * 8 options:MTLResourceStorageModeShared];
    return secondRun(device, ^(id<MTLBlitCommandEncoder> e) {
        if (intoTexture)
            [e copyFromBuffer:b
                       sourceOffset:0
                  sourceBytesPerRow:kSide * 8
                sourceBytesPerImage:kSide * kSide * 8
                         sourceSize:MTLSizeMake(kSide, kSide, 1)
                          toTexture:t
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        else
            [e copyFromTexture:t
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(kSide, kSide, 1)
                                toBuffer:b
                       destinationOffset:0
                  destinationBytesPerRow:kSide * 8
                destinationBytesPerImage:kSide * kSide * 8];
    });
}

void copiesIntoACompressedTextureAreClamped()
{
    id<MTLDevice> device = clampDevice();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> compressed = xrTexture(device, true), plain = xrTexture(device, false);
    ASSERT_TRUE(compressed != nil && plain != nil);
    const Work into = copied(device, compressed, true), plainInto = copied(device, plain, true);
    EXPECT_EQ(into.launches, 2u) << "the copy, then the texture's region into the clamp's scratch";
    EXPECT_GE(into.draws, 1u) << "the clamp pass";
    EXPECT_EQ(plainInto.launches, 1u) << "a texture Metal does not compress: the copy alone";
    EXPECT_EQ(plainInto.draws, 0u);
    for (id<MTLTexture> t : {xrTexture(device, true, 553), xrTexture(device, true, 552, MTLStorageModeShared)}) {
        ASSERT_TRUE(t != nil);
        const Work other = copied(device, t, true);
        EXPECT_EQ(other.launches, 2u) << "BGRA10_XR_sRGB, and shared storage, alike";
        EXPECT_GE(other.draws, 1u);
    }
}

// A compressed texture copied into a plain one leaves the plain one clamped (m2v-textures5 probes/where.mm); into an array's
// slice, which was not measured, the copy alone
void copiesBetweenTexturesAreClampedWhereMeasured()
{
    id<MTLDevice> device = clampDevice();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> compressed = xrTexture(device, true), plain = xrTexture(device, false),
                   array = xrTexture(device, false, 552, MTLStorageModePrivate, 2);
    ASSERT_TRUE(compressed != nil && plain != nil && array != nil);
    const Work intoPlain = copiedBetween(device, compressed, plain);
    const Work intoArray = copiedBetween(device, compressed, array);
    EXPECT_EQ(intoPlain.launches, 2u) << "the copy, then the plain texture's region into the clamp's scratch";
    EXPECT_GE(intoPlain.draws, 1u);
    EXPECT_EQ(intoArray.launches, 1u) << "an array's slice: the copy alone";
    EXPECT_EQ(intoArray.draws, 0u);
}

void copiesOutOfACompressedTextureAreClamped()
{
    id<MTLDevice> device = clampDevice();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> compressed = xrTexture(device, true), plain = xrTexture(device, false);
    ASSERT_TRUE(compressed != nil && plain != nil);
    const Work out = copied(device, compressed, false), plainOut = copied(device, plain, false);
    EXPECT_GE(out.launches, 2u)
        << "the copy, then the buffer's rows into the clamp's scratch (and a backing's fill and drain where the rows "
           "are no pitch target)";
    EXPECT_GE(out.draws, 1u) << "the clamp pass into the buffer's rows";
    EXPECT_EQ(plainOut.launches, 1u);
    EXPECT_EQ(plainOut.draws, 0u);
}

}  // namespace

TEST(GM20BFacadeCopyClamp, CopiesIntoACompressedExtendedRangeTextureAreClamped)
{
    switch_ios::test::ExpectPassesInFreshProcess(copiesIntoACompressedTextureAreClamped);
}

TEST(GM20BFacadeCopyClamp, CopiesBetweenTexturesAreClampedWhereMeasured)
{
    switch_ios::test::ExpectPassesInFreshProcess(copiesBetweenTexturesAreClampedWhereMeasured);
}

TEST(GM20BFacadeCopyClamp, CopiesOutOfACompressedExtendedRangeTextureAreClamped)
{
    switch_ios::test::ExpectPassesInFreshProcess(copiesOutOfACompressedTextureAreClamped);
}
