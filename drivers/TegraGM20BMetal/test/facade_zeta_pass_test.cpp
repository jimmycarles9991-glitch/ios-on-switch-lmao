// SPDX-License-Identifier: MIT
// Render passes of depth and stencil attachments as the facade records them (facade_harness.h): a Depth32Float
// texture beside a Stencil8 texture (array slices past 0 too), and an aspect whose store action the encoder decides
// (MTLStoreActionUnknown, then setDepthStoreAction: / setStencilStoreAction:, a resolve there with no resolve texture
// keeping its store part), are the driver's (no pass.ds refusal), and their command buffers complete on it, as does a
// depth cleared to NaN. A one-sample Depth16Unorm beside a Stencil8 is converted through a Depth32Float_Stencil8; a
// multisampled pair of them stays refused without its conversion states.
#include "facade_harness.h"
#include "../TegraGM20BMetalInternal.h"
#include "../record.h"
#include "../turn_copy.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BCopy.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using namespace harness;

id<MTLDevice> gDevice;

id<MTLTexture> zeta(MTLPixelFormat format)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:32 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    return [gDevice newTextureWithDescriptor:d];
}

id<MTLTexture> zetaChain(MTLPixelFormat format, NSUInteger width = 100, NSUInteger height = 60, NSUInteger layers = 6,
                         NSUInteger levels = 4)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:YES];
    d.textureType = layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    d.arrayLength = layers;
    d.mipmapLevelCount = levels;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [gDevice newTextureWithDescriptor:d];
}

class FacadeZetaPass : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_TRUE(gDevice != nil);
        for (const uint32_t format : {250u, 260u}) {
            gm20b::RuntimeRecord r;
            r.keyString = "samples=1 ds=" + std::to_string(format) + (format == 260 ? "/260 " : "/0 ");
            for (gm20b::CapturedProgram &p : r.programs) {
                p.image.assign(80 + 64, 0);
                p.gprs = 8;
            }
            const char *name = gm20b::turnDepthState(format);
            r.pipeline.keyHash = gm20b::Pack::fnv(name, strlen(name));
            r.blob = {0x1a00, 0x1111};
            std::string why;
            ASSERT_NE(gRM->raw->addRecord(r, false, why), nullptr) << why;
        }
    }

    // One pass of `pass` and nothing drawn, its encoder deciding the stores `decide` names: whether the driver took it,
    // and the path its command buffer ran
    static std::pair<bool, std::string> run(MTLRenderPassDescriptor *pass, void (^decide)(id<MTLRenderCommandEncoder>),
                                            uint32_t *sourceTic = nullptr)
    {
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
        if (decide)
            decide(e);
        [e endEncoding];
        if (sourceTic)
            *sourceTic = ((SwitchMetalCommandBuffer *)cb)->_passes.back().zeta[0].sourceTic;
        const bool taken = (((SwitchMetalCommandBuffer *)cb)->_pred0 != PredPassDepthStencil);
        [cb commit];
        [cb waitUntilCompleted];
        NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
        return {taken, cb.status == MTLCommandBufferStatusCompleted && !cb.error ? (path.UTF8String ?: "?") : "error"};
    }

    static MTLRenderPassDescriptor *pass(id<MTLTexture> depth, id<MTLTexture> stencil, MTLLoadAction load,
                                         MTLStoreAction store)
    {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.depthAttachment.texture = depth;
        p.depthAttachment.loadAction = load;
        p.depthAttachment.storeAction = store;
        p.stencilAttachment.texture = stencil;
        p.stencilAttachment.loadAction = load;
        p.stencilAttachment.storeAction = store;
        return p;
    }

    static std::map<uint32_t, uint32_t> methods(size_t first,
                                                std::vector<std::pair<uint64_t, uint64_t>> *copies = nullptr)
    {
        std::map<uint32_t, uint32_t> out;
        uint64_t from = 0, to = 0;
        const auto write = [&](uint32_t subchannel, uint32_t method, uint32_t value) {
            if (subchannel == gm20b::SUBC_3D)
                out[method] = value;
            else if (copies && subchannel == gm20b::SUBC_COPY) {
                if (method == NVB0B5_OFFSET_IN_UPPER)
                    from = (from & 0xffffffffull) | uint64_t(value) << 32;
                else if (method == NVB0B5_OFFSET_IN_LOWER)
                    from = (from & ~0xffffffffull) | value;
                else if (method == NVB0B5_OFFSET_OUT_UPPER)
                    to = (to & 0xffffffffull) | uint64_t(value) << 32;
                else if (method == NVB0B5_OFFSET_OUT_LOWER)
                    to = (to & ~0xffffffffull) | value;
                else if (method == NVB0B5_LAUNCH_DMA && from && to)
                    copies->emplace_back(from, to);
            }
        };
        const auto &jobs = kernel_model::model.jobs;
        for (size_t j = first; j < jobs.size(); ++j) {
            const auto &words = jobs[j].words;
            for (size_t i = 0; i < words.size();) {
                const uint32_t header = words[i++], op = header >> 29, count = header >> 16 & 0x1fff,
                               subchannel = header >> 13 & 7, method = (header & 0xfff) << 2;
                if (op == 4) {
                    write(subchannel, method, count);
                    continue;
                }
                for (uint32_t k = 0; k < count && i < words.size(); ++k, ++i)
                    write(subchannel, method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i]);
            }
        }
        return out;
    }
};

TEST_F(FacadeZetaPass, MipAttachmentsBindTheirOwnZetaAddressAndExtent)
{
    const struct {
        MTLPixelFormat format;
        uint32_t zt, clear, bytes, stride, offset[4], pitch[4];
    } images[] = {
        {MTLPixelFormatDepth16Unorm, 0x13, 1, 2, 24576, {0, 16384, 20480, 21504}, {256, 128, 64, 64}},
        {MTLPixelFormatDepth32Float, 0x0a, 1, 4, 40960, {0, 28672, 36864, 38912}, {448, 256, 128, 64}},
        {MTLPixelFormatStencil8, 0x17, 2, 1, 12288, {0, 8192, 10240, 11264}, {128, 64, 64, 64}},
        {MTLPixelFormatDepth32Float_Stencil8, 0x19, 3, 8, 73728, {0, 53248, 67584, 71680}, {832, 448, 256, 128}},
    };

    const struct {
        uint32_t level, slice;
    } parts[] = {{0, 0}, {1, 2}, {2, 1}, {3, 5}};

    for (const auto &image : images) {
        auto texture = zetaChain(image.format);
        ASSERT_NE(texture, nil) << image.format;
        const auto &raw = ((SwitchMetalTexture *)texture)->_raw;
        ASSERT_NE(raw.chain, nullptr);
        for (const auto &part : parts) {
            SCOPED_TRACE(::testing::Message() << image.format << '/' << part.level << '/' << part.slice);
            auto p = pass(image.format != MTLPixelFormatStencil8 ? texture : nil,
                          image.format == MTLPixelFormatStencil8 || image.format == MTLPixelFormatDepth32Float_Stencil8
                              ? texture
                              : nil,
                          MTLLoadActionClear, MTLStoreActionStore);
            p.depthAttachment.level = p.stencilAttachment.level = part.level;
            p.depthAttachment.slice = p.stencilAttachment.slice = part.slice;
            p.depthAttachment.clearDepth = 0.25;
            p.stencilAttachment.clearStencil = 0x5a;
            const size_t first = kernel_model::model.jobs.size();
            const auto [taken, path] = run(p, nil);
            EXPECT_TRUE(taken);
            EXPECT_EQ(path, "gm20b");
            const auto emitted = methods(first);
            const uint32_t required[] = {NVB197_SET_ZT_A,
                                         NVB197_SET_ZT_B,
                                         NVB197_SET_ZT_FORMAT,
                                         NVB197_SET_ZT_ARRAY_PITCH,
                                         NVB197_SET_ZT_SIZE_A,
                                         NVB197_SET_ZT_SIZE_B,
                                         NVB197_SET_ZT_SIZE_C,
                                         NVB197_SET_CLEAR_RECT_HORIZONTAL,
                                         NVB197_SET_CLEAR_RECT_VERTICAL,
                                         NVB197_SET_CLEAR_SURFACE_CONTROL,
                                         NVB197_CLEAR_SURFACE};
            for (const uint32_t method : required)
                ASSERT_NE(emitted.find(method), emitted.end()) << std::hex << method;
            const uint64_t address = uint64_t(emitted.at(NVB197_SET_ZT_A)) << 32 | emitted.at(NVB197_SET_ZT_B);
            EXPECT_EQ(address, raw.chain->va + image.offset[part.level] + uint64_t(part.slice) * image.stride);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_FORMAT), image.zt);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_ARRAY_PITCH), image.stride >> 2);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_A), image.pitch[part.level] / image.bytes);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_B), 60u >> part.level);
            EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_C), 1u);
            EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_RECT_HORIZONTAL), (100u >> part.level) << 16);
            EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_RECT_VERTICAL), (60u >> part.level) << 16);
            EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_SURFACE_CONTROL), 0x10u);
            EXPECT_EQ(emitted.at(NVB197_CLEAR_SURFACE), image.clear);
        }
    }
}

// One Depth32Float_Stencil8 texture as the depth at one level and slice and as the stencil at another: an M4 Max keeps
// each aspect in its own part (harness/passformsoracle.mm zeta_two_levels, zeta_two_slices), so each is loaded from its
// part and stored back into it
TEST_F(FacadeZetaPass, OneTexturesAspectsAtTwoPartsEachMoveInAndOutOfTheirOwnPart)
{
    auto texture = zetaChain(MTLPixelFormatDepth32Float_Stencil8);
    ASSERT_NE(texture, nil);
    const auto &chain = *((SwitchMetalTexture *)texture)->_raw.chain;
    const auto address = [&](uint32_t level, uint32_t slice) {
        return chain.va + chain.layout.level[level].offset + uint64_t(slice) * chain.layout.arrayStride;
    };
    for (const bool otherLevel : {false, true}) {
        SCOPED_TRACE(otherLevel ? "levels 1 and 2" : "slices 1 and 2");
        auto p = pass(texture, texture, MTLLoadActionLoad, MTLStoreActionStore);
        p.depthAttachment.level = 1;
        p.depthAttachment.slice = 1;
        p.stencilAttachment.level = otherLevel ? 2 : 1;
        p.stencilAttachment.slice = otherLevel ? 1 : 2;
        const size_t first = kernel_model::model.jobs.size();
        const auto [taken, path] = run(p, nil);
        EXPECT_TRUE(taken);
        EXPECT_EQ(path, "gm20b");
        std::vector<std::pair<uint64_t, uint64_t>> copies;
        methods(first, &copies);
        for (const uint64_t part : {address(1, 1), address(otherLevel ? 2 : 1, otherLevel ? 1 : 2)}) {
            EXPECT_TRUE(std::any_of(copies.begin(), copies.end(),
                                    [&](const auto &c) {
                                        return c.first == part;
                                    }))
                << "loaded from " << std::hex << part;
            EXPECT_TRUE(std::any_of(copies.begin(), copies.end(),
                                    [&](const auto &c) {
                                        return c.second == part;
                                    }))
                << "stored into " << std::hex << part;
        }
    }
}

TEST_F(FacadeZetaPass, SeparateMipAttachmentsCopyTheirOwnLevelsAndSlices)
{
    auto depth = zetaChain(MTLPixelFormatDepth32Float), stencil = zetaChain(MTLPixelFormatStencil8, 200, 120, 6, 5);
    ASSERT_NE(depth, nil);
    ASSERT_NE(stencil, nil);
    ASSERT_NE(((SwitchMetalTexture *)depth)->_raw.chain, nullptr);
    ASSERT_NE(((SwitchMetalTexture *)stencil)->_raw.chain, nullptr);
    for (const bool view : {false, true}) {
        id<MTLTexture> d = depth, s = stencil;
        uint32_t depthLevel = 1, stencilLevel = 2, depthSlice = 1, stencilSlice = 3;
        if (view) {
            d = [depth newTextureViewWithPixelFormat:MTLPixelFormatDepth32Float
                                         textureType:MTLTextureType2DArray
                                              levels:NSMakeRange(1, 3)
                                              slices:NSMakeRange(1, 5)];
            ASSERT_NE(d, nil);
            d = [d newTextureViewWithPixelFormat:MTLPixelFormatDepth32Float
                                     textureType:MTLTextureType2DArray
                                          levels:NSMakeRange(0, 2)
                                          slices:NSMakeRange(0, 3)];
            s = [stencil newTextureViewWithPixelFormat:MTLPixelFormatStencil8
                                           textureType:MTLTextureType2DArray
                                                levels:NSMakeRange(1, 4)
                                                slices:NSMakeRange(2, 3)];
            depthLevel = 1;
            stencilLevel = 2;
            depthSlice = 1;
            stencilSlice = 0;
        }
        ASSERT_NE(d, nil);
        ASSERT_NE(s, nil);
        const auto &depthRaw = ((SwitchMetalTexture *)depth)->_raw, &stencilRaw = ((SwitchMetalTexture *)stencil)->_raw;
        const uint32_t absoluteDepthLevel = depthLevel + (view ? 1 : 0);
        const uint32_t absoluteStencilLevel = stencilLevel + (view ? 1 : 0);
        const uint32_t absoluteDepthSlice = depthSlice + (view ? 1 : 0);
        const uint32_t absoluteStencilSlice = stencilSlice + (view ? 2 : 0);
        for (const auto load : {MTLLoadActionClear, MTLLoadActionLoad}) {
            SCOPED_TRACE(::testing::Message() << view << '/' << load);
            auto p = pass(d, s, load, MTLStoreActionStore);
            p.depthAttachment.level = depthLevel;
            p.depthAttachment.slice = depthSlice;
            p.stencilAttachment.level = stencilLevel;
            p.stencilAttachment.slice = stencilSlice;
            p.renderTargetArrayLength = 2;
            const size_t first = kernel_model::model.jobs.size();
            const auto [taken, path] = run(p, nil);
            EXPECT_TRUE(taken);
            EXPECT_EQ(path, "gm20b");
            std::vector<std::pair<uint64_t, uint64_t>> copies;
            const auto emitted = methods(first, &copies);
            ASSERT_FALSE(copies.empty());
            if (load == MTLLoadActionClear) {
                ASSERT_NE(emitted.find(NVB197_SET_CLEAR_RECT_HORIZONTAL), emitted.end());
                ASSERT_NE(emitted.find(NVB197_SET_CLEAR_RECT_VERTICAL), emitted.end());
                EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_RECT_HORIZONTAL), (100u >> absoluteDepthLevel) << 16);
                EXPECT_EQ(emitted.at(NVB197_SET_CLEAR_RECT_VERTICAL), (60u >> absoluteDepthLevel) << 16);
            }

            const struct {
                const gm20b::Texture *texture;
                uint32_t level, slice;
            } selected[] = {{&depthRaw, absoluteDepthLevel, absoluteDepthSlice},
                            {&stencilRaw, absoluteStencilLevel, absoluteStencilSlice}};

            for (const auto &part : selected) {
                const auto &chain = *part.texture->chain;
                for (uint32_t layer = 0; layer < 6; ++layer) {
                    const uint64_t address =
                        chain.va + chain.layout.level[part.level].offset + uint64_t(layer) * chain.layout.arrayStride;
                    const bool used = layer >= part.slice && layer < part.slice + 2;
                    EXPECT_EQ(std::any_of(copies.begin(), copies.end(),
                                          [&](const auto &copy) {
                                              return copy.first == address;
                                          }),
                              used && load == MTLLoadActionLoad);
                    EXPECT_EQ(std::any_of(copies.begin(), copies.end(),
                                          [&](const auto &copy) {
                                              return copy.second == address;
                                          }),
                              used);
                }
                EXPECT_FALSE(std::any_of(copies.begin(), copies.end(), [&](const auto &copy) {
                    return copy.first == part.texture->va || copy.second == part.texture->va;
                }));
            }
        }
    }
}

// A pair at levels of their own is the driver's; a level past either chain, or of a texture of no chain, is Metal's error
TEST_F(FacadeZetaPass, MipAdmissionTakesAspectsAtTheirOwnLevelsAndKeepsTheChainBounds)
{
    auto depth = zetaChain(MTLPixelFormatDepth32Float), stencil = zetaChain(MTLPixelFormatStencil8);
    ASSERT_NE(depth, nil);
    ASSERT_NE(stencil, nil);
    const auto refused = [](MTLRenderPassDescriptor *p) {
        auto cb = [[gDevice newCommandQueue] commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, PredPassDepthStencil);
    };
    auto separate = pass(depth, stencil, MTLLoadActionClear, MTLStoreActionStore);
    separate.depthAttachment.level = 1;
    separate.stencilAttachment.level = 2;
    const auto [taken, path] = run(separate, nil);
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "gm20b");
    separate.stencilAttachment.level = 4;
    refused(separate);
    auto past = pass(depth, nil, MTLLoadActionClear, MTLStoreActionStore);
    past.depthAttachment.level = 4;
    refused(past);
    auto whole = pass(zeta(MTLPixelFormatDepth32Float), nil, MTLLoadActionClear, MTLStoreActionStore);
    whole.depthAttachment.level = 1;
    refused(whole);
}

TEST_F(FacadeZetaPass, Depth16MipConversionKeepsItsPreparedHeaderAndSelectedStoreback)
{
    for (const bool view : {false, true}) {
        auto depth = zetaChain(MTLPixelFormatDepth16Unorm, 100, 60, view ? 4 : 1);
        auto stencil = zetaChain(MTLPixelFormatStencil8, 200, 120, 4, 5);
        ASSERT_NE(depth, nil);
        ASSERT_NE(stencil, nil);
        const auto &root = ((SwitchMetalTexture *)depth)->_raw;
        ASSERT_NE(root.chain, nullptr);
        id<MTLTexture> selected = depth;
        if (view)
            selected = [depth newTextureViewWithPixelFormat:MTLPixelFormatDepth16Unorm
                                                textureType:MTLTextureType2D
                                                     levels:NSMakeRange(1, 3)
                                                     slices:NSMakeRange(2, 1)];
        ASSERT_NE(selected, nil);
        const uint32_t level = view ? 2 : 1, slice = view ? 2 : 0;
        for (const auto load : {MTLLoadActionClear, MTLLoadActionLoad}) {
            auto p = pass(selected, stencil, load, MTLStoreActionStore);
            p.depthAttachment.level = 1;
            p.stencilAttachment.level = level + 1;
            p.stencilAttachment.slice = 2;
            uint32_t prepared = 0;
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                SCOPED_TRACE(::testing::Message() << view << '/' << load << '/' << repeat);
                const size_t first = kernel_model::model.jobs.size();
                uint32_t tic = 0;
                const auto [taken, path] = run(p, nil, &tic);
                EXPECT_TRUE(taken);
                EXPECT_EQ(path, "gm20b");
                if (load == MTLLoadActionLoad) {
                    EXPECT_NE(tic, 0u);
                    if (repeat)
                        EXPECT_EQ(tic, prepared);
                    prepared = tic;
                } else {
                    EXPECT_EQ(tic, 0u);
                }
                const auto emitted = methods(first);
                for (const auto method : {NVB197_SET_ZT_A, NVB197_SET_ZT_B, NVB197_SET_ZT_FORMAT, NVB197_SET_ZT_SIZE_A,
                                          NVB197_SET_ZT_SIZE_B})
                    ASSERT_NE(emitted.find(method), emitted.end());
                const uint64_t address = uint64_t(emitted.at(NVB197_SET_ZT_A)) << 32 | emitted.at(NVB197_SET_ZT_B);
                EXPECT_EQ(address, root.chain->va + root.chain->layout.level[level].offset +
                                       uint64_t(slice) * root.chain->layout.arrayStride);
                EXPECT_EQ(emitted.at(NVB197_SET_ZT_FORMAT), 0x13u);
                EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_A), root.chain->layout.level[level].rowBytes / 2);
                EXPECT_EQ(emitted.at(NVB197_SET_ZT_SIZE_B), 60u >> level);
            }
        }
    }
}

TEST_F(FacadeZetaPass, ADepth32FloatBesideAStencil8IsTheRawPaths)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float), stencil = zeta(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil);
    for (MTLLoadAction load : {MTLLoadActionClear, MTLLoadActionLoad, MTLLoadActionDontCare})
        for (MTLStoreAction store : {MTLStoreActionStore, MTLStoreActionDontCare}) {
            const auto [taken, path] = run(pass(depth, stencil, load, store), nil);
            EXPECT_TRUE(taken) << load << "/" << store;
            EXPECT_EQ(path, "gm20b") << load << "/" << store;
        }
}

TEST_F(FacadeZetaPass, APairFromASlicePastZeroIsTheRawPaths)
{
    const auto array = [](MTLPixelFormat format) {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:32 mipmapped:NO];
        d.textureType = MTLTextureType2DArray;
        d.arrayLength = 3;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget;
        return [gDevice newTextureWithDescriptor:d];
    };
    id<MTLTexture> depth = array(MTLPixelFormatDepth32Float), stencil = array(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil);
    for (NSUInteger from : {1, 2}) {
        MTLRenderPassDescriptor *p = pass(depth, stencil, MTLLoadActionLoad, MTLStoreActionStore);
        p.depthAttachment.slice = from;
        p.stencilAttachment.slice = 3 - from;
        const auto [taken, path] = run(p, nil);
        EXPECT_TRUE(taken) << from;
        EXPECT_EQ(path, "gm20b") << from;
    }
}

TEST_F(FacadeZetaPass, ADepth16UnormBesideAStencil8IsTakenAndATwoSampleOneIsNot)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth16Unorm), stencil = zeta(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil);
    const auto taken = [](MTLRenderPassDescriptor *p) {
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
        [e endEncoding];
        return (((SwitchMetalCommandBuffer *)cb)->_pred0 != PredPassDepthStencil);
    };
    EXPECT_TRUE(taken(pass(depth, stencil, MTLLoadActionLoad, MTLStoreActionStore)));
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth16Unorm
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.textureType = MTLTextureType2DMultisample;
    d.sampleCount = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> depth4 = [gDevice newTextureWithDescriptor:d];
    d.pixelFormat = MTLPixelFormatStencil8;
    id<MTLTexture> stencil4 = [gDevice newTextureWithDescriptor:d];
    ASSERT_TRUE(depth4 != nil && stencil4 != nil);
    EXPECT_FALSE(taken(pass(depth4, stencil4, MTLLoadActionLoad, MTLStoreActionStore)));
}

TEST_F(FacadeZetaPass, AStoreTheEncoderDecidesIsTheRawPaths)
{
    id<MTLTexture> combined = zeta(MTLPixelFormatDepth32Float_Stencil8);
    ASSERT_TRUE(combined != nil);
    for (MTLStoreAction decided : {MTLStoreActionStore, MTLStoreActionDontCare}) {
        const auto [taken, path] =
            run(pass(combined, combined, MTLLoadActionClear, MTLStoreActionUnknown), ^(id<MTLRenderCommandEncoder> e) {
                [e setDepthStoreAction:decided];
                [e setStencilStoreAction:decided];
            });
        EXPECT_TRUE(taken) << decided;
        EXPECT_EQ(path, "gm20b") << decided;
    }
}

// The sample-position agnostic depth store the encoder decides is taken as the plain store, on the driver
TEST_F(FacadeZetaPass, ASamplePositionAgnosticStoreTheEncoderDecidesIsTheStore)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float);
    ASSERT_TRUE(depth != nil);
    __block MTLStoreAction kept = MTLStoreActionUnknown;
    const auto [taken, path] =
        run(pass(depth, nil, MTLLoadActionClear, MTLStoreActionUnknown), ^(id<MTLRenderCommandEncoder> e) {
            [e setDepthStoreAction:MTLStoreActionCustomSampleDepthStore];
            SwitchMetalRenderEncoder *r = (SwitchMetalRenderEncoder *)e;
            kept = r->_cb->_passes[r->_passAt].zeta[0].store;
        });
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "gm20b");
    EXPECT_EQ(kept, MTLStoreActionStore);
}

TEST_F(FacadeZetaPass, APassBegunWithASamplePositionAgnosticStoreStoresItsDepth)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float);
    ASSERT_TRUE(depth != nil);
    __block MTLStoreAction kept = MTLStoreActionUnknown;
    MTLRenderPassDescriptor *agnostic = pass(depth, nil, MTLLoadActionClear, MTLStoreActionCustomSampleDepthStore);
    const auto [taken, path] = run(agnostic, ^(id<MTLRenderCommandEncoder> e) {
        SwitchMetalRenderEncoder *r = (SwitchMetalRenderEncoder *)e;
        kept = r->_cb->_passes[r->_passAt].zeta[0].store;
    });
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "gm20b");
    EXPECT_EQ(kept, MTLStoreActionStore);
}

// A resolve the encoder decides where the pass names no resolve texture, Metal's validation error: nothing to resolve
// into, so the aspect keeps the store the action names besides, as a colour attachment does, on the driver
TEST_F(FacadeZetaPass, AResolveTheEncoderDecidesWithNoResolveTextureKeepsItsStore)
{
    id<MTLTexture> combined = zeta(MTLPixelFormatDepth32Float_Stencil8);
    ASSERT_TRUE(combined != nil);
    for (MTLStoreAction decided : {MTLStoreActionMultisampleResolve, MTLStoreActionStoreAndMultisampleResolve}) {
        __block MTLStoreAction depthKept = MTLStoreActionUnknown, stencilKept = MTLStoreActionUnknown;
        NSDictionary *before = counters(gDevice);
        const auto [taken, path] =
            run(pass(combined, combined, MTLLoadActionClear, MTLStoreActionUnknown), ^(id<MTLRenderCommandEncoder> e) {
                [e setDepthStoreAction:decided];
                [e setStencilStoreAction:decided];
                SwitchMetalRenderEncoder *r = (SwitchMetalRenderEncoder *)e;
                depthKept = r->_cb->_passes[r->_passAt].zeta[0].store;
                stencilKept = r->_cb->_passes[r->_passAt].zeta[1].store;
            });
        const MTLStoreAction kept =
            decided == MTLStoreActionMultisampleResolve ? MTLStoreActionDontCare : MTLStoreActionStore;
        EXPECT_TRUE(taken) << decided;
        EXPECT_EQ(path, "gm20b") << decided;
        EXPECT_EQ(depthKept, kept) << decided;
        EXPECT_EQ(stencilKept, kept) << decided;
        const std::map<std::string, long long> eachAspect = {{"skipped", 2}, {"skipped.pass.ds", 2}};
        EXPECT_EQ(skippedMoved(before, counters(gDevice)), eachAspect) << decided;
    }
}

// A store the encoder decides other than DontCare, Store or the agnostic store, Metal's validation error: stored, and
// counted skipped at each call
TEST_F(FacadeZetaPass, AnUnknownStoreTheEncoderDecidesIsStoredAndCountedSkipped)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float);
    ASSERT_TRUE(depth != nil);
    __block MTLStoreAction kept = MTLStoreActionDontCare;
    NSDictionary *before = counters(gDevice);
    const auto [taken, path] =
        run(pass(depth, nil, MTLLoadActionClear, MTLStoreActionUnknown), ^(id<MTLRenderCommandEncoder> e) {
            [e setDepthStoreAction:MTLStoreActionUnknown];
            SwitchMetalRenderEncoder *r = (SwitchMetalRenderEncoder *)e;
            kept = r->_cb->_passes[r->_passAt].zeta[0].store;
        });
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "gm20b");
    EXPECT_EQ(kept, MTLStoreActionStore);
    EXPECT_EQ(skippedMoved(before, counters(gDevice)), skippedOnce("pass.ds"));
}

TEST_F(FacadeZetaPass, ANanClearDepthIsTheRawPaths)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float);
    ASSERT_TRUE(depth != nil);
    MTLRenderPassDescriptor *p = pass(depth, nil, MTLLoadActionClear, MTLStoreActionStore);
    p.depthAttachment.clearDepth = NAN;
    const auto [taken, path] = run(p, nil);
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "gm20b");
}

}  // namespace
