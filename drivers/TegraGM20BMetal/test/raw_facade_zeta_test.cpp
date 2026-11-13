// SPDX-License-Identifier: MIT
// Stored one-sample depth/stencil textures on the raw facade (raw_facade_harness.h): made in the raw path's own zeta memory
// with no m2v twin, and, at the first replay that asks for one, moved by the copy engine into the image m2v's twin has
// (the stand-in's NVK image: memory of the raw device's own, as NVK's would be bound in the same VM).
#include "raw_facade_harness.h"
#include "../raw/runtime/RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"

#include <gtest/gtest.h>
#include <functional>
#include <vector>

@interface HarnessZetaImage : NSObject {
@public
    NSData *_memory;
}
@end
@implementation HarnessZetaImage

- (NSData *)harnessImageMemory
{
    return _memory;
}

@end

namespace {

std::function<NSData *(MTLTextureDescriptor *)> gImageMemory;

}  // namespace

// m2v with an NVK image of its own for each texture it makes
@interface HarnessZetaM2V : HarnessM2V
@end
@implementation HarnessZetaM2V

- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    HarnessZetaImage *image = [HarnessZetaImage new];
    image->_memory = gImageMemory ? gImageMemory(d) : nil;
    return image;
}

@end

namespace {

using namespace harness;

id<MTLDevice> gDevice;
std::vector<id<MTLBuffer>> gImages;

struct ZetaFormat {
    MTLPixelFormat format;
    uint32_t bpp;
};

constexpr ZetaFormat kFormats[] = {{MTLPixelFormatDepth16Unorm, 2},
                                   {MTLPixelFormatDepth32Float, 4},
                                   {MTLPixelFormatStencil8, 1},
                                   {MTLPixelFormatDepth32Float_Stencil8, 8}};

uint32_t texelBytes(MTLPixelFormat format)
{
    for (const ZetaFormat &f : kFormats)
        if (f.format == format)
            return f.bpp;
    return 0;
}

uint32_t layersOf(MTLTextureDescriptor *d)
{
    return d.textureType == MTLTextureTypeCube      ? 6
           : d.textureType == MTLTextureType2DArray ? uint32_t(d.arrayLength)
                                                    : 1;
}

// An NVK image of `d` in a raw buffer: one-GOB-high blocks, rows of whole GOBs, its layers one after another
NSData *imageMemory(MTLTextureDescriptor *d)
{
    const uint32_t bpp = texelBytes(d.pixelFormat);
    switch_nvk_image_memory m = {};
    m.row_stride_B = uint32_t((d.width * bpp + 63) / 64 * 64);
    m.array_stride_B = uint64_t(m.row_stride_B) * ((d.height + 7) / 8 * 8);
    m.size_B = m.array_stride_B * layersOf(d);
    id<MTLBuffer> image = [gDevice newBufferWithLength:m.size_B options:MTLResourceStorageModePrivate];
    if (![(id)image isKindOfClass:[RMBuffer class]])
        return nil;
    gImages.push_back(image);
    m.addr = ((RMBuffer *)image)->_raw.va;
    m.texel_B = bpp;
    m.tiled = 1;
    return [NSData dataWithBytes:&m length:sizeof m];
}

MTLTextureDescriptor *stored(MTLPixelFormat format, MTLTextureType type = MTLTextureType2D)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
    d.textureType = type;
    if (type == MTLTextureType2DArray)
        d.arrayLength = 3;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return d;
}

class FacadeStoredZeta : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)), [HarnessM2VMethods class], [HarnessZetaM2V class]);
        ASSERT_TRUE(gDevice != nil);
        gImageMemory = imageMemory;
    }
};

TEST_F(FacadeStoredZeta, EachFormatIsHeldInItsOwnPlainZetaMemoryWithNoTwin)
{
    for (const ZetaFormat &f : kFormats) {
        id<MTLTexture> texture = [gDevice newTextureWithDescriptor:stored(f.format)];
        ASSERT_TRUE([(id)texture isKindOfClass:[RMTexture class]]) << f.format;
        RMTexture *t = (RMTexture *)texture;
        EXPECT_TRUE(t->_rawOk) << f.format;
        EXPECT_TRUE(t->_raw.zeta) << f.format;
        EXPECT_EQ(int(t->_raw.samples), 1) << f.format;
        EXPECT_EQ(t->_raw.bpp, f.bpp) << f.format;
        EXPECT_FALSE(t->_raw.comptags) << f.format;
        EXPECT_NE(t->_raw.va, 0u) << f.format;
        @synchronized(t) {
            EXPECT_TRUE(t->_twin == nil) << f.format;
        }
    }
}

// The copy engine's source and destination of each launch the jobs since `first` submitted
std::vector<std::pair<uint64_t, uint64_t>> copies(size_t first)
{
    std::vector<std::pair<uint64_t, uint64_t>> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++)
        for (const TegraGM20BAbiPush &push : jobs[j].pushes) {
            const auto *w = reinterpret_cast<const uint32_t *>(kernel_model::model.host(push.va));
            uint32_t in[2] = {}, outs[2] = {};
            for (uint32_t i = 0; w && i < push.dwords;) {
                const uint32_t hdr = w[i++], op = hdr >> 29, subc = hdr >> 13 & 7, m = (hdr & 0xfff) << 2,
                               n = hdr >> 16 & 0x1fff;
                if (!hdr)
                    continue;
                if (op == 7)
                    break;
                if (op == 4) {
                    if (subc == 4 && m == NVB0B5_LAUNCH_DMA)
                        out.push_back({uint64_t(in[0]) << 32 | in[1], uint64_t(outs[0]) << 32 | outs[1]});
                    continue;
                }
                for (uint32_t k = 0; k < n && i < push.dwords; k++, i++) {
                    const uint32_t method = m + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0);
                    if (subc != 4)
                        continue;
                    if (method == NVB0B5_OFFSET_IN_UPPER)
                        in[0] = w[i];
                    else if (method == NVB0B5_OFFSET_IN_LOWER)
                        in[1] = w[i];
                    else if (method == NVB0B5_OFFSET_OUT_UPPER)
                        outs[0] = w[i];
                    else if (method == NVB0B5_OFFSET_OUT_LOWER)
                        outs[1] = w[i];
                    else if (method == NVB0B5_LAUNCH_DMA)
                        out.push_back({uint64_t(in[0]) << 32 | in[1], uint64_t(outs[0]) << 32 | outs[1]});
                }
            }
        }
    return out;
}

// m2v's twin, asked for at a replay: NVK's image, the texels copied into it from the raw path's own memory, and the
// texture over it from then on; a second ask is the same twin
TEST_F(FacadeStoredZeta, AReplaysTwinIsNvksImageWithTheTexelsMovedIntoIt)
{
    id<MTLTexture> texture = [gDevice newTextureWithDescriptor:stored(MTLPixelFormatDepth32Float_Stencil8)];
    ASSERT_TRUE([(id)texture isKindOfClass:[RMTexture class]]);
    RMTexture *t = (RMTexture *)texture;
    ASSERT_TRUE(t->_rawOk);
    const uint64_t owned = t->_raw.va;
    const uint32_t ownedTic = t->_raw.tic;
    const size_t first = kernel_model::model.jobs.size();
    id twin = ((id (*)(id, SEL))objc_msgSend)(texture, sel_registerName("rmTwin"));
    ASSERT_TRUE([twin isKindOfClass:[HarnessZetaImage class]]);
    switch_nvk_image_memory m;
    memcpy(&m, ((HarnessZetaImage *)twin)->_memory.bytes, sizeof m);
    EXPECT_EQ(t->_raw.va, m.addr);
    EXPECT_EQ(t->_raw.pitch, m.row_stride_B);
    EXPECT_NE(t->_raw.tic, ownedTic) << "a header of its own over NVK's image";
    const auto launched = copies(first);
    ASSERT_FALSE(launched.empty());
    EXPECT_EQ(launched.front().first, owned);
    EXPECT_EQ(launched.front().second, m.addr);
    const size_t after = kernel_model::model.jobs.size();
    EXPECT_TRUE(((id (*)(id, SEL))objc_msgSend)(texture, sel_registerName("rmTwin")) == twin);
    EXPECT_EQ(kernel_model::model.jobs.size(), after);
}

// Two replays asking at once: one move, one twin for both
TEST_F(FacadeStoredZeta, TwoAsksAtOnceMoveItOnce)
{
    for (int round = 0; round < 8; round++) {
        id<MTLTexture> texture = [gDevice newTextureWithDescriptor:stored(MTLPixelFormatDepth32Float)];
        ASSERT_TRUE([(id)texture isKindOfClass:[RMTexture class]]);
        const uint64_t owned = ((RMTexture *)texture)->_raw.va;
        const size_t first = kernel_model::model.jobs.size();
        __block id one = nil, other = nil;
        dispatch_group_t both = dispatch_group_create();
        const auto ask = ^id {
            return ((id (*)(id, SEL))objc_msgSend)(texture, sel_registerName("rmTwin"));
        };
        dispatch_group_async(both, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            one = ask();
        });
        dispatch_group_async(both, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            other = ask();
        });
        dispatch_group_wait(both, DISPATCH_TIME_FOREVER);
        ASSERT_TRUE(one != nil) << round;
        EXPECT_TRUE(one == other) << round;
        EXPECT_EQ(kernel_model::model.jobs.size() - first, 1u) << round << ": one move, one copy job";
        const auto launched = copies(first);
        ASSERT_FALSE(launched.empty()) << round;
        EXPECT_EQ(launched.front().first, owned) << round;
    }
}

// A 2D array (3 layers) and a cube (6 faces) of each format: held in their own zeta memory, layerBytes apart and no
// header, no twin; a replay's twin is NVK's image with every layer copied into it, layer by layer at its stride
TEST_F(FacadeStoredZeta, ArraysAndCubesAreHeldAndMoveEveryLayer)
{
    for (const MTLTextureType type : {MTLTextureType2DArray, MTLTextureTypeCube})
        for (const ZetaFormat &f : kFormats) {
            MTLTextureDescriptor *d = stored(f.format, type);
            id<MTLTexture> texture = [gDevice newTextureWithDescriptor:d];
            ASSERT_TRUE([(id)texture isKindOfClass:[RMTexture class]]) << f.format << " " << type;
            RMTexture *t = (RMTexture *)texture;
            ASSERT_TRUE(t->_rawOk) << f.format << " " << type;
            const uint32_t layers = layersOf(d);
            EXPECT_EQ(t->_raw.layers, layers);
            EXPECT_EQ(t->_raw.tic, 0u) << "a layered zeta has no header";
            EXPECT_GE(t->_raw.storage.bytes, t->_raw.layerBytes * layers);
            @synchronized(t) {
                EXPECT_TRUE(t->_twin == nil);
            }
            const uint64_t owned = t->_raw.va, ownedStride = t->_raw.layerBytes;
            const size_t first = kernel_model::model.jobs.size();
            id twin = ((id (*)(id, SEL))objc_msgSend)(texture, sel_registerName("rmTwin"));
            ASSERT_TRUE([twin isKindOfClass:[HarnessZetaImage class]]) << f.format << " " << type;
            switch_nvk_image_memory m;
            memcpy(&m, ((HarnessZetaImage *)twin)->_memory.bytes, sizeof m);
            EXPECT_EQ(t->_raw.va, m.addr);
            EXPECT_EQ(t->_raw.layerBytes, m.array_stride_B);
            EXPECT_EQ(t->_raw.layers, layers);
            std::vector<std::pair<uint64_t, uint64_t>> layerCopies;
            for (const auto &copy : copies(first))
                if (copy.first >= owned && copy.first < owned + ownedStride * layers)
                    layerCopies.push_back(copy);
            ASSERT_EQ(layerCopies.size(), layers) << f.format << " " << type;
            for (uint32_t layer = 0; layer < layers; layer++) {
                EXPECT_EQ(layerCopies[layer].first, owned + layer * ownedStride);
                EXPECT_EQ(layerCopies[layer].second, m.addr + layer * m.array_stride_B);
            }
        }
}

}  // namespace
