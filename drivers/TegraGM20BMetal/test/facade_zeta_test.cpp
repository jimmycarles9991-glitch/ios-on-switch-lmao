// SPDX-License-Identifier: MIT
// Stored depth/stencil textures on the facade (facade_harness.h): made in the driver's own zeta memory.
#include "facade_harness.h"
#include "../TegraGM20BMetalInternal.h"
#include "../record.h"
#include "../samples.h"
#include "../turn_copy.h"

#include <gtest/gtest.h>
#include <vector>

namespace {

using namespace harness;

id<MTLDevice> gDevice;

struct ZetaFormat {
    MTLPixelFormat format;
    uint32_t bpp;
};

constexpr ZetaFormat kFormats[] = {{MTLPixelFormatDepth16Unorm, 2},
                                   {MTLPixelFormatDepth32Float, 4},
                                   {MTLPixelFormatStencil8, 1},
                                   {MTLPixelFormatDepth32Float_Stencil8, 8}};

uint32_t layersOf(MTLTextureDescriptor *d)
{
    return d.textureType == MTLTextureTypeCube      ? 6
           : d.textureType == MTLTextureType2DArray ? uint32_t(d.arrayLength)
                                                    : 1;
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
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_TRUE(gDevice != nil);
    }
};

TEST_F(FacadeStoredZeta, EachFormatIsHeldInItsOwnPlainZetaMemory)
{
    for (const ZetaFormat &f : kFormats) {
        id<MTLTexture> texture = [gDevice newTextureWithDescriptor:stored(f.format)];
        ASSERT_TRUE([(id)texture isKindOfClass:[SwitchMetalTexture class]]) << f.format;
        SwitchMetalTexture *t = (SwitchMetalTexture *)texture;
        EXPECT_TRUE(t->_gm20bOk) << f.format;
        EXPECT_TRUE(t->_raw.zeta) << f.format;
        EXPECT_EQ(int(t->_raw.samples), 1) << f.format;
        EXPECT_EQ(t->_raw.bpp, f.bpp) << f.format;
        EXPECT_FALSE(t->_raw.comptags) << f.format;
        EXPECT_NE(t->_raw.va, 0u) << f.format;
    }
}

// A 2D array (3 layers) and a cube (6 faces) of each format: held in their own zeta memory, layerBytes apart, with the
// array or cube header their ShaderRead usage samples them through
TEST_F(FacadeStoredZeta, ArraysAndCubesAreHeldInTheirOwnZetaMemory)
{
    for (const MTLTextureType type : {MTLTextureType2DArray, MTLTextureTypeCube})
        for (const ZetaFormat &f : kFormats) {
            MTLTextureDescriptor *d = stored(f.format, type);
            id<MTLTexture> texture = [gDevice newTextureWithDescriptor:d];
            ASSERT_TRUE([(id)texture isKindOfClass:[SwitchMetalTexture class]]) << f.format << " " << type;
            SwitchMetalTexture *t = (SwitchMetalTexture *)texture;
            ASSERT_TRUE(t->_gm20bOk) << f.format << " " << type;
            const uint32_t layers = layersOf(d);
            EXPECT_EQ(t->_raw.layers, layers);
            EXPECT_NE(t->_raw.tic, 0u) << "a ShaderRead layered zeta has its header";
            EXPECT_GE(t->_raw.storage.bytes, t->_raw.layerBytes * layers);
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
        if (subchannel != gm20b::SUBC_3D)
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

// The pack's state `name` (key fields `fields`) as a record whose fragment program reads set 0, which holds its texture
// 0: each of its draws is a conversionSamples sample
void addSamplingRecord(const char *name, const std::string &fields)
{
    gm20b::RuntimeRecord r;
    r.keyString = fields;
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    const uint64_t readSet = 0x4c98ull << 48 | 1ull << 34;
    memcpy(r.programs[1].image.data() + 88, &readSet, sizeof readSet);
    gm20b::setEntryCount(r.pipeline, 1);
    r.pipeline.entry[0] = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerNone, 0, 0};
    r.pipeline.setBytes = 32;
    r.pipeline.memoryEntries = 1;
    r.pipeline.keyHash = gm20b::Pack::fnv(name, strlen(name));
    r.blob = {0x1a00, 0x1111};
    std::string why;
    ASSERT_NE(gRM->raw->addRecord(r, false, why), nullptr) << why;
}

std::string depthFields(uint32_t format, uint32_t samples)
{
    return "samples=" + std::to_string(samples) + " ds=" + std::to_string(format) + (format == 260 ? "/260 " : "/0 ");
}

MTLTextureDescriptor *multisampled(MTLPixelFormat format)
{
    MTLTextureDescriptor *d = stored(format);
    d.textureType = MTLTextureType2DMultisample;
    d.sampleCount = 4;
    d.usage = MTLTextureUsageRenderTarget;
    return d;
}

// A multisampled Depth16Unorm beside a Stencil8 renders a Depth32Float_Stencil8 of the command buffer's, its depth
// converted in from the Depth16Unorm: the depth's resolve reads that zeta, whether the pass discards its depth
// (MultisampleResolve: nothing converts it back) or keeps it (StoreAndMultisampleResolve: converted back after the resolve)
TEST_F(FacadeStoredZeta, AMultisampledDepth16ResolvesTheDepthItsPassRendered)
{
    for (const uint32_t format : {250u, 260u})
        ASSERT_NO_FATAL_FAILURE(addSamplingRecord(gm20b::turnDepthState(format, 4), depthFields(format, 4)));
    ASSERT_NO_FATAL_FAILURE(addSamplingRecord(gm20b::depthResolveState(250, 0), depthFields(250, 1)));
    SwitchMetalTexture *depth =
        (SwitchMetalTexture *)[gDevice newTextureWithDescriptor:multisampled(MTLPixelFormatDepth16Unorm)];
    id<MTLTexture> stencil = [gDevice newTextureWithDescriptor:multisampled(MTLPixelFormatStencil8)];
    SwitchMetalTexture *resolve =
        (SwitchMetalTexture *)[gDevice newTextureWithDescriptor:stored(MTLPixelFormatDepth16Unorm)];
    ASSERT_TRUE(depth != nil && stencil != nil && resolve != nil);
    ASSERT_TRUE(depth->_gm20bOk && resolve->_gm20bOk);
    for (const MTLStoreAction store : {MTLStoreActionMultisampleResolve, MTLStoreActionStoreAndMultisampleResolve}) {
        SCOPED_TRACE(store);
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = (id<MTLTexture>)depth;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = store;
        pass.depthAttachment.resolveTexture = (id<MTLTexture>)resolve;
        pass.stencilAttachment.texture = stencil;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
        auto cb = [[gDevice newCommandQueue] commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
        ASSERT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
        const size_t first = kernel_model::model.jobs.size();
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error;
        const bool kept = store == MTLStoreActionStoreAndMultisampleResolve;
        const auto samples = conversionSamples(first);
        ASSERT_EQ(samples.size(), kept ? 3u : 2u);
        const Sample &in = samples[0], &resolved = samples[1];
        EXPECT_EQ(in.address, depth->_raw.va);
        EXPECT_NE(in.target, depth->_raw.va);
        EXPECT_EQ(resolved.target, resolve->_raw.va);
        EXPECT_EQ(resolved.address, in.target) << "a resolve of the Depth16Unorm, which holds none of the pass's depth";
        if (kept) {
            EXPECT_EQ(samples[2].address, in.target);
            EXPECT_EQ(samples[2].target, depth->_raw.va);
        }
    }
}

}  // namespace
