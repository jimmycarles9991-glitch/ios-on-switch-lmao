// SPDX-License-Identifier: MIT
// A Depth32Float beside a Stencil8 as the facade renders them: the pass's Depth32Float_Stencil8 holds both, a stencil
// resolve reads its stencil there (stencil_bits.cpp: a Depth32Float_Stencil8 source for a Stencil8 target), so the
// Depth32Float_Stencil8 stencil-bits state alone is what it needs, and a stored stencil is still copied back into its own
// texture after the pass.
#include "facade_harness.h"
#include "../TegraGM20BMetalInternal.h"
#include "../record.h"
#include "../stencil_bits.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BCopy.h"

#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace harness;

id<MTLDevice> gDevice;

id<MTLTexture> zeta(MTLPixelFormat format, NSUInteger samples = 1)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:32 mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    if (samples > 1) {
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
    }
    return [gDevice newTextureWithDescriptor:d];
}

class FacadeSeparateStencil : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_TRUE(gDevice != nil);
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 ds=260/260 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        const char *name = gm20b::stencilBitsState(gm20b::kZetaFormat, 4, gm20b::StencilSample::Zero);
        r.pipeline.keyHash = gm20b::Pack::fnv(name, strlen(name));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(gRM->raw->addRecord(r, false, why), nullptr) << why;
    }

    // One pass of `pass` and nothing drawn: whether the driver took it, and the path its command buffer ran
    static std::pair<bool, std::string> run(MTLRenderPassDescriptor *pass)
    {
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
        const bool taken = (((SwitchMetalCommandBuffer *)cb)->_pred0 != PredPassDepthStencil);
        [cb commit];
        [cb waitUntilCompleted];
        NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
        return {taken, cb.status == MTLCommandBufferStatusCompleted && !cb.error ? (path.UTF8String ?: "?") : "error"};
    }

    // The copy engine's launches from job `first` on that move texels (a line length): source and destination. The copy
    // kit launches by an immediate method (nouveau_copy_rect.c), its value in the header
    static std::vector<std::pair<uint64_t, uint64_t>> copies(size_t first)
    {
        std::vector<std::pair<uint64_t, uint64_t>> out;
        uint64_t from = 0, to = 0;
        uint32_t length = 0;
        const auto write = [&](uint32_t m, uint32_t v) {
            if (m == NVB0B5_OFFSET_IN_UPPER)
                from = (from & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == NVB0B5_OFFSET_IN_LOWER)
                from = (from & ~0xffffffffull) | v;
            else if (m == NVB0B5_OFFSET_OUT_UPPER)
                to = (to & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == NVB0B5_OFFSET_OUT_LOWER)
                to = (to & ~0xffffffffull) | v;
            else if (m == NVB0B5_LINE_LENGTH_IN)
                length = v;
            else if (m == NVB0B5_LAUNCH_DMA && length)
                out.emplace_back(from, to);
        };
        const auto &jobs = kernel_model::model.jobs;
        for (size_t j = first; j < jobs.size(); ++j) {
            const auto &words = jobs[j].words;
            for (size_t i = 0; i < words.size();) {
                const uint32_t header = words[i++], op = header >> 29, count = header >> 16 & 0x1fff,
                               subchannel = header >> 13 & 7, method = (header & 0xfff) << 2;
                if (op == 4) {
                    if (subchannel == gm20b::SUBC_COPY)
                        write(method, count);
                    continue;
                }
                for (uint32_t k = 0; k < count && i < words.size(); ++k, ++i)
                    if (subchannel == gm20b::SUBC_COPY)
                        write(method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i]);
            }
        }
        return out;
    }
};

// The resolve reads the pass's Depth32Float_Stencil8: the Stencil8 states are not asked for
TEST_F(FacadeSeparateStencil, AStencilResolveTakesTheDepth32FloatStencil8StateAlone)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float, 4), stencil = zeta(MTLPixelFormatStencil8, 4),
                   into = zeta(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil && into != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.depthAttachment.texture = depth;
    p.depthAttachment.loadAction = MTLLoadActionClear;
    p.depthAttachment.storeAction = MTLStoreActionDontCare;
    p.stencilAttachment.texture = stencil;
    p.stencilAttachment.loadAction = MTLLoadActionClear;
    p.stencilAttachment.storeAction = MTLStoreActionMultisampleResolve;
    p.stencilAttachment.resolveTexture = into;
    p.stencilAttachment.stencilResolveFilter = MTLMultisampleStencilResolveFilterSample0;
    const auto [taken, path] = run(p);
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "gm20b");
}

// A stored stencil of an ordinary pair goes back into its own texture: a copy launch writes into its memory
TEST_F(FacadeSeparateStencil, AnOrdinaryPairStillStoresItsStencil)
{
    id<MTLTexture> depth = zeta(MTLPixelFormatDepth32Float), stencil = zeta(MTLPixelFormatStencil8);
    ASSERT_TRUE(depth != nil && stencil != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.depthAttachment.texture = depth;
    p.depthAttachment.loadAction = MTLLoadActionLoad;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    p.stencilAttachment.texture = stencil;
    p.stencilAttachment.loadAction = MTLLoadActionLoad;
    p.stencilAttachment.storeAction = MTLStoreActionStore;
    const size_t first = kernel_model::model.jobs.size();
    const auto [taken, path] = run(p);
    EXPECT_TRUE(taken);
    EXPECT_EQ(path, "gm20b");
    const gm20b::Texture &s = ((SwitchMetalTexture *)stencil)->_raw;
    const uint64_t end = s.va + std::max<uint64_t>(s.storage.bytes, uint64_t(s.pitch) * s.height);
    bool stored = false;
    for (const auto &[from, to] : copies(first))
        stored |= to >= s.va && to < end;
    EXPECT_TRUE(stored) << "no copy into the Stencil8 texture after the pass";
}

}  // namespace
