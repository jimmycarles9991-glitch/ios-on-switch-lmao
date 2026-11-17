// SPDX-License-Identifier: MIT
// Stored one-sample depth/stencil textures on the raw facade (raw_facade_harness.h): made in the raw path's own zeta memory
// with no m2v twin, and, at the first replay that asks for one, moved by the copy engine into the image m2v's twin has
// (the stand-in's NVK image: memory of the raw device's own, as NVK's would be bound in the same VM).
#include "raw_facade_harness.h"
#include "../raw/runtime/RawMetalInternal.h"
#include "../raw/runtime/RawMetalPassCache.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_turn_copy.h"
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
    for (const bool sampled : {true, false}) {
        SCOPED_TRACE(sampled);
        auto d = stored(MTLPixelFormatDepth32Float_Stencil8);
        if (!sampled)
            d.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [gDevice newTextureWithDescriptor:d];
        ASSERT_TRUE([(id)texture isKindOfClass:[RMTexture class]]);
        RMTexture *t = (RMTexture *)texture;
        ASSERT_TRUE(t->_rawOk);
        const uint64_t owned = t->_raw.va;
        const uint32_t ownedTic = t->_raw.tic;
        EXPECT_EQ(ownedTic != 0, sampled);
        const size_t first = kernel_model::model.jobs.size();
        id twin = ((id (*)(id, SEL))objc_msgSend)(texture, sel_registerName("rmTwin"));
        ASSERT_TRUE([twin isKindOfClass:[HarnessZetaImage class]]);
        switch_nvk_image_memory m;
        memcpy(&m, ((HarnessZetaImage *)twin)->_memory.bytes, sizeof m);
        EXPECT_EQ(t->_raw.va, m.addr);
        EXPECT_EQ(t->_raw.pitch, m.row_stride_B);
        EXPECT_NE(t->_raw.tic, 0u);
        EXPECT_NE(t->_raw.tic, ownedTic) << "a header of its own over NVK's image";
        const auto launched = copies(first);
        ASSERT_FALSE(launched.empty());
        EXPECT_EQ(launched.front().first, owned);
        EXPECT_EQ(launched.front().second, m.addr);
        const size_t after = kernel_model::model.jobs.size();
        EXPECT_TRUE(((id (*)(id, SEL))objc_msgSend)(texture, sel_registerName("rmTwin")) == twin);
        EXPECT_EQ(kernel_model::model.jobs.size(), after);
    }
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

// A 2D array (3 layers) and a cube (6 faces) of each format: held in their own zeta memory, layerBytes apart, with the
// array or cube header their ShaderRead usage samples them through, no twin; a replay's twin is NVK's image with every
// layer copied into it, layer by layer at its stride
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
            EXPECT_NE(t->_raw.tic, 0u) << "a ShaderRead layered zeta has its header";
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

struct Sample {
    uint32_t tic;
    uint64_t address, target;
};

std::vector<Sample> conversionSamples(size_t first)
{
    std::vector<Sample> out;
    std::map<uint32_t, uint32_t> registers;
    const auto write = [&](uint32_t subchannel, uint32_t method, uint32_t value, size_t job) {
        if (subchannel != raw::SUBC_3D)
            return;
        registers[method] = value;
        if (job < first || method != NVB197_BIND_GROUP_CONSTANT_BUFFER(4) || !(value & 1) || (value >> 4 & 0x1f) != 1)
            return;
        const uint64_t set = uint64_t(registers[NVB197_SET_CONSTANT_BUFFER_SELECTOR_B]) << 32 |
                             registers[NVB197_SET_CONSTANT_BUFFER_SELECTOR_C];
        const uint8_t *descriptor = kernel_model::model.host(set, 4);
        EXPECT_NE(descriptor, nullptr);
        if (!descriptor)
            return;
        uint32_t tic = 0;
        memcpy(&tic, descriptor, sizeof tic);
        tic &= 0xfffff;
        const uint32_t *w = gRM->raw->headerWords(tic);
        out.push_back({tic, uint64_t(w[1] >> 9) << 9 | uint64_t(w[2] & 0xffff) << 32,
                       uint64_t(registers[NVB197_SET_ZT_A]) << 32 | registers[NVB197_SET_ZT_B]});
    };
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = 0; j < jobs.size(); ++j) {
        const auto &words = jobs[j].words;
        for (size_t i = 0; i < words.size();) {
            const uint32_t h = words[i++], op = h >> 29, count = h >> 16 & 0x1fff, subchannel = h >> 13 & 7,
                           method = (h & 0xfff) << 2;
            if (op == 4) {
                write(subchannel, method, count, j);
                continue;
            }
            if (op != 1 && op != 3 && op != 5)
                continue;
            for (uint32_t k = 0; k < count && i < words.size(); ++k, ++i)
                write(subchannel, method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i], j);
        }
    }
    return out;
}

TEST_F(FacadeStoredZeta, ARecordedDepth16PassAndItsCachedCopyUseTheMovedImage)
{
    for (const uint32_t format : {250u, 260u}) {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 ds=" + std::to_string(format) + (format == 260 ? "/260 " : "/0 ");
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
            p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
        }
        const uint64_t readSet = 0x4c98ull << 48 | 1ull << 34;
        memcpy(r.programs[1].image.data() + 88, &readSet, sizeof readSet);
        raw::setEntryCount(r.pipeline, 1);
        r.pipeline.entry[0] = {0, raw::EntryTexture, 2, 0, raw::SamplerNone, 0, 0};
        r.pipeline.setBytes = 32;
        r.pipeline.memoryEntries = 1;
        const char *name = raw::turnDepthState(format);
        r.pipeline.keyHash = raw::Pack::fnv(name, strlen(name));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(gRM->raw->addRecord(r, false, why), nullptr) << why;
    }
    auto d = stored(MTLPixelFormatDepth16Unorm);
    d.usage = MTLTextureUsageRenderTarget;
    RMTexture *depth = (RMTexture *)[gDevice newTextureWithDescriptor:d];
    id<MTLTexture> stencil = [gDevice newTextureWithDescriptor:stored(MTLPixelFormatStencil8)];
    ASSERT_NE(depth, nil);
    ASSERT_NE(stencil, nil);
    ASSERT_EQ(depth->_raw.tic, 0u);
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = (id<MTLTexture>)depth;
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.texture = stencil;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    auto queue = [gDevice newCommandQueue];
    auto cb = [queue commandBuffer];
    auto firstEncoder = [cb renderCommandEncoderWithDescriptor:pass];
    [firstEncoder endEncoding];
    RMCommandBuffer *recorded = (RMCommandBuffer *)cb;
    ASSERT_EQ(recorded->_passes.size(), 1u);
    EXPECT_EQ(recorded->_passes[0].zeta[0].sourceTic, 0u);
    const uint32_t oldTic = depth->_raw.tic;
    ASSERT_NE(oldTic, 0u);
    const uint64_t oldAddress = depth->_raw.va;
    const uint64_t epoch = rmPassCacheReturned(recorded->_passCache.get());
    ASSERT_NE([depth rmTwin], nil);
    ASSERT_NE(depth->_raw.tic, oldTic);
    ASSERT_NE(depth->_raw.va, oldAddress);
    EXPECT_FALSE(rmPassCachePublished(epoch));
    auto seam = [cb blitCommandEncoder];
    [seam endEncoding];
    auto cachedEncoder = [cb renderCommandEncoderWithDescriptor:pass];
    EXPECT_TRUE(((RMRenderEncoder *)cachedEncoder)->_passShared);
    [cachedEncoder endEncoding];
    ASSERT_EQ(recorded->_passes.size(), 2u);
    EXPECT_EQ(recorded->_passes[0].zeta[0].sourceTic, 0u);
    EXPECT_EQ(recorded->_passes[1].zeta[0].sourceTic, 0u);
    const size_t first = kernel_model::model.jobs.size();
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error;
    ASSERT_EQ(recorded->_preds, 0u);
    const auto samples = conversionSamples(first);
    ASSERT_EQ(samples.size(), 4u);
    for (size_t i = 0; i < samples.size(); i += 2) {
        EXPECT_EQ(samples[i].tic, depth->_raw.tic);
        EXPECT_EQ(samples[i].address, depth->_raw.va);
        EXPECT_NE(samples[i].address, oldAddress);
        EXPECT_EQ(samples[i + 1].address, samples[i].target);
        EXPECT_EQ(samples[i + 1].target, depth->_raw.va);
    }
}

TEST_F(FacadeStoredZeta, AMipChainCannotMoveOutFromUnderItsPreparedHeaders)
{
    auto d = stored(MTLPixelFormatDepth16Unorm);
    d.mipmapLevelCount = 3;
    RMTexture *depth = (RMTexture *)[gDevice newTextureWithDescriptor:d];
    ASSERT_NE(depth, nil);
    ASSERT_NE(depth->_raw.chain, nullptr);
    const uint64_t address = depth->_raw.va;
    const uint32_t tic = depth->_raw.tic;
    const size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ([depth rmTwin], nil);
    EXPECT_EQ(depth->_raw.va, address);
    EXPECT_EQ(depth->_raw.tic, tic);
    EXPECT_EQ(kernel_model::model.jobs.size(), first);
}

}  // namespace
