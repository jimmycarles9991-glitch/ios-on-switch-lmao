// SPDX-License-Identifier: MIT
// useHeap through the facade on the host (facade_harness.h): on render and compute encoders a heap is one use of
// the command buffer's plan and one use of its arena in each job, however often it is used and however many resources the
// arena holds (TegraGM20BMetalRenderEncoder.mm, TegraGM20BMetalAnswers.mm, TegraGM20BMetalEncode.mm). A heap resource used alone is the range it
// holds of the arena.
#include "device_harness.h"
#include "facade_harness.h"
#include "predication_model.h"

namespace {

class FacadeHeapUse : public testing::Test {
protected:
    id<MTLHeap> heap;
    id<MTLTexture> target;
    std::vector<id<MTLTexture>> placed;
    uint64_t arena = 0;

    void SetUp() override
    {
        heap = facade::heap(MTLStorageModeShared, 32u << 20);
        ASSERT_NE(heap, nil);
        ASSERT_NE(((SwitchMetalHeap *)heap)->_arena, 0u);
        arena = gm20b::DeviceHarness::arena(*gRM->raw, ((SwitchMetalHeap *)heap)->_arena - 1).handle;
        MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:64
                                                                                    height:64
                                                                                 mipmapped:NO];
        t.storageMode = MTLStorageModePrivate;
        t.usage = MTLTextureUsageRenderTarget;
        target = [facade::device() newTextureWithDescriptor:t];
        ASSERT_NE(target, nil);
    }

    void place(uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++) {
            id<MTLTexture> t = facade::texture(MTLStorageModeShared, heap);
            ASSERT_NE(t, nil) << i;
            ASSERT_TRUE(((SwitchMetalTexture *)t)->_raw.arena) << i;
            placed.push_back(t);
        }
    }

    id<MTLRenderCommandEncoder> pass(id<MTLCommandBuffer> cb)
    {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = target;
        d.colorAttachments[0].loadAction = MTLLoadActionClear;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        return [cb renderCommandEncoderWithDescriptor:d];
    }

    // The jobs `cb`'s commit submitted, each one's uses
    std::vector<std::vector<TegraGM20BAbiSurfaceUse>> commit(id<MTLCommandBuffer> cb)
    {
        const size_t mark = kernel_model::model.jobs.size();
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
        std::vector<std::vector<TegraGM20BAbiSurfaceUse>> out;
        for (size_t j = mark; j < kernel_model::model.jobs.size(); j++)
            out.push_back(kernel_model::model.jobs[j].uses);
        return out;
    }

    size_t arenaUses(const std::vector<TegraGM20BAbiSurfaceUse> &uses, uint32_t access = 0) const
    {
        return size_t(std::count_if(uses.begin(), uses.end(), [&](const TegraGM20BAbiSurfaceUse &u) {
            return u.handle == arena && (u.access & access) == access;
        }));
    }

    struct Used {
        size_t planHeaps = 0, planTextures = 0, uses = 0;
        std::vector<size_t> arenaPerJob;
    };

    // A pass and a compute encoder with no dispatch, using the heap three times and twice, in one command buffer
    Used run()
    {
        Used used;
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        const id<MTLHeap> heaps[1] = {heap};
        id<MTLRenderCommandEncoder> render = pass(cb);
        [render useHeap:heap];
        [render useHeap:heap stages:MTLRenderStageFragment];
        [render useHeaps:heaps count:1];
        [render endEncoding];
        id<MTLComputeCommandEncoder> compute = [cb computeCommandEncoder];
        [compute useHeap:heap];
        [compute useHeaps:heaps count:1];
        [compute endEncoding];
        SwitchMetalCommandPlan &plan = ((SwitchMetalCommandBuffer *)cb)->_plan;
        plan.prepareRange(0, plan.size());
        plan.forEachResource(0, plan.size(), SwitchMetalResourceKind::Heap, [&](const SwitchMetalRangeResource &) {
            used.planHeaps++;
        });
        plan.forEachResource(0, plan.size(), SwitchMetalResourceKind::Texture, [&](const SwitchMetalRangeResource &) {
            used.planTextures++;
        });
        for (const auto &uses : commit(cb)) {
            used.uses += uses.size();
            used.arenaPerJob.push_back(arenaUses(uses));
        }
        return used;
    }
};

TEST_F(FacadeHeapUse, AHeapIsOnePlanUseAndOneArenaUsePerJobAt4And404Textures)
{
    @autoreleasepool {
        run();  // the channel's first command buffer carries its context init as a job of its own
        place(4);
        const Used few = run();
        place(400);
        const Used many = run();
        for (const Used &u : {few, many}) {
            EXPECT_EQ(u.planHeaps, 1u);
            EXPECT_EQ(u.planTextures, 1u) << "the pass's target alone";
            ASSERT_FALSE(u.arenaPerJob.empty());
            for (size_t n : u.arenaPerJob)
                EXPECT_EQ(n, 1u);
        }
        EXPECT_EQ(few.arenaPerJob.size(), many.arenaPerJob.size());
        EXPECT_EQ(few.uses, many.uses) << "what a job names does not grow with the heap";
    }
}

TEST_F(FacadeHeapUse, AComputeEncodersUseIsTheCommandBuffersToo)
{
    @autoreleasepool {
        place(2);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> compute = [cb computeCommandEncoder];
        [compute useHeap:heap];
        [compute endEncoding];
        [pass(cb) endEncoding];
        SwitchMetalCommandPlan &plan = ((SwitchMetalCommandBuffer *)cb)->_plan;
        plan.prepareRange(0, plan.size());
        size_t heaps = 0;
        plan.forEachResource(0, plan.size(), SwitchMetalResourceKind::Heap, [&](const SwitchMetalRangeResource &) {
            heaps++;
        });
        EXPECT_EQ(heaps, 1u);
        const auto jobs = commit(cb);
        ASSERT_FALSE(jobs.empty());
        EXPECT_EQ(arenaUses(jobs.back()), 1u);
    }
}

TEST_F(FacadeHeapUse, ASharedHeapsArenaIsNamedHostWritten)
{
    @autoreleasepool {
        place(1);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> render = pass(cb);
        [render useHeap:heap];
        [render endEncoding];
        const auto jobs = commit(cb);
        ASSERT_FALSE(jobs.empty());
        EXPECT_EQ(arenaUses(jobs.back(), TegraGM20BSurfaceHostWritten), 1u);
    }
}

TEST_F(FacadeHeapUse, AHeapTextureUsedAloneNamesItsOwnRange)
{
    @autoreleasepool {
        place(10);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLRenderCommandEncoder> render = pass(cb);
        [render useResource:placed[0] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        [render useResource:placed[1] usage:MTLResourceUsageRead];
        [render endEncoding];
        const auto jobs = commit(cb);
        ASSERT_FALSE(jobs.empty());
        EXPECT_EQ(arenaUses(jobs.back(), TegraGM20BSurfaceRange), 2u) << "the two used, none of the other eight";
    }
}

TEST_F(FacadeHeapUse, AHeapTextureAComputeEncoderUsesNamesItsOwnRange)
{
    @autoreleasepool {
        place(10);
        id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> compute = [cb computeCommandEncoder];
        const id<MTLResource> two[2] = {placed[1], placed[2]};
        [compute useResource:placed[0] usage:MTLResourceUsageRead];
        [compute useResources:two count:2 usage:MTLResourceUsageRead];
        [compute endEncoding];
        [pass(cb) endEncoding];
        const auto jobs = commit(cb);
        ASSERT_FALSE(jobs.empty());
        EXPECT_EQ(arenaUses(jobs.back(), TegraGM20BSurfaceRange), 3u) << "the three used, none of the other seven";
    }
}

TEST_F(FacadeHeapUse, ABlitReadsAHeapBufferAcrossRestoredMappings)
{
    kernel_model::model.unknownUsesRefused = true;
    constexpr uint64_t quarter = 0x40000;
    for (MTLStorageMode mode : {MTLStorageModeShared, MTLStorageModePrivate}) {
        SCOPED_TRACE(int(mode));
        @autoreleasepool {
            id<MTLHeap> h = facade::heap(mode, 4 * quarter);
            ASSERT_NE(h, nil);
            const auto options = MTLResourceOptions(mode << MTLResourceStorageModeShift);
            id<MTLBuffer> a, b, c;
            @autoreleasepool {
                a = [h newBufferWithLength:quarter options:options];
                b = [h newBufferWithLength:quarter options:options];
                c = [h newBufferWithLength:quarter options:options];
            }
            ASSERT_NE(a, nil);
            ASSERT_NE(b, nil);
            ASSERT_NE(c, nil);
            const uint64_t firstVa = ((SwitchMetalBuffer *)a)->_raw.va;
            [b makeAliasable];
            b = nil;
            gRM->raw->trimArenas(gRM->committed);
            ASSERT_EQ(gm20b::DeviceHarness::arena(*gRM->raw, ((SwitchMetalHeap *)h)->_arena - 1).handle, 0u);
            [a makeAliasable];
            a = nil;
            id<MTLBuffer> source = [h newBufferWithLength:2 * quarter options:options];
            ASSERT_NE(source, nil);
            ASSERT_EQ(((SwitchMetalBuffer *)source)->_raw.va, firstVa);
            const uint64_t bytes = source.length;
            std::vector<uint8_t> expected(bytes);
            for (size_t i = 0; i < expected.size(); ++i)
                expected[i] = uint8_t((i * 13) ^ (i >> 12));
            memcpy(((SwitchMetalBuffer *)source)->_raw.cpu, expected.data(), bytes);
            id<MTLBuffer> destination = [facade::device() newBufferWithLength:bytes
                                                                      options:MTLResourceStorageModeShared];
            ASSERT_NE(destination, nil);
            memset(destination.contents, 0xA5, bytes);
            id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:bytes];
            [blit endEncoding];
            const size_t firstJob = kernel_model::model.jobs.size();
            [cb commit];
            [cb waitUntilCompleted];
            ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
            predication_model::Gpu gpu(0);
            gpu.map = [](uint64_t va) {
                return kernel_model::model.host(va);
            };
            for (size_t j = firstJob; j < kernel_model::model.jobs.size(); ++j)
                for (const auto &push : kernel_model::model.jobs[j].pushes) {
                    const auto *words = reinterpret_cast<const uint32_t *>(
                        kernel_model::model.host(push.va, uint64_t(push.dwords) * 4));
                    ASSERT_NE(words, nullptr);
                    gpu.run(words, push.dwords, true);
                }
            EXPECT_EQ(memcmp(destination.contents, expected.data(), bytes), 0);
        }
    }
}

TEST_F(FacadeHeapUse, ARecordedHeapUseOutlivesMakeAliasableAndIdleTrim)
{
    kernel_model::model.unknownUsesRefused = true;
    constexpr uint64_t quarter = 0x40000;
    for (MTLStorageMode mode : {MTLStorageModeShared, MTLStorageModePrivate}) {
        SCOPED_TRACE(int(mode));
        @autoreleasepool {
            id<MTLHeap> h = facade::heap(mode, 4 * quarter);
            ASSERT_NE(h, nil);
            const auto options = MTLResourceOptions(mode << MTLResourceStorageModeShift);
            id<MTLBuffer> a = [h newBufferWithLength:quarter options:options];
            id<MTLBuffer> b = [h newBufferWithLength:quarter options:options];
            id<MTLBuffer> c = [h newBufferWithLength:quarter options:options];
            ASSERT_NE(a, nil);
            ASSERT_NE(b, nil);
            ASSERT_NE(c, nil);
            uint8_t *const bytes = ((SwitchMetalBuffer *)b)->_raw.cpu;
            memset(bytes, 0x3C, quarter);
            id<MTLBuffer> source = [facade::device() newBufferWithLength:quarter options:MTLResourceStorageModeShared];
            ASSERT_NE(source, nil);
            memset(source.contents, 0xA7, quarter);
            id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromBuffer:source sourceOffset:0 toBuffer:b destinationOffset:0 size:quarter];
            [blit endEncoding];
            [b makeAliasable];
            EXPECT_EQ(h.usedSize, 2 * quarter);
            gRM->raw->trimArenas(gRM->committed);
            const size_t firstJob = kernel_model::model.jobs.size();
            [cb commit];
            [cb waitUntilCompleted];
            ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
            predication_model::Gpu gpu(0);
            gpu.map = [](uint64_t va) {
                return kernel_model::model.host(va);
            };
            for (size_t j = firstJob; j < kernel_model::model.jobs.size(); ++j)
                for (const auto &push : kernel_model::model.jobs[j].pushes) {
                    const auto *words = reinterpret_cast<const uint32_t *>(
                        kernel_model::model.host(push.va, uint64_t(push.dwords) * 4));
                    ASSERT_NE(words, nullptr);
                    gpu.run(words, push.dwords, true);
                }
            const std::vector<uint8_t> expected(quarter, 0xA7);
            EXPECT_EQ(memcmp(bytes, expected.data(), quarter), 0);
            EXPECT_EQ(a.length + c.length, 2 * quarter);
        }
    }
}

TEST_F(FacadeHeapUse, ARecordedHeapViewKeepsItsAliasableOwnersMappings)
{
    kernel_model::model.unknownUsesRefused = true;
    constexpr uint64_t quarter = 0x40000;
    for (MTLStorageMode mode : {MTLStorageModeShared, MTLStorageModePrivate}) {
        SCOPED_TRACE(int(mode));
        @autoreleasepool {
            id<MTLHeap> h = facade::heap(mode, 4 * quarter);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:256
                                                                                        height:256
                                                                                     mipmapped:NO];
            d.storageMode = mode;
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
            id<MTLTexture> a = [h newTextureWithDescriptor:d];
            id<MTLTexture> b = [h newTextureWithDescriptor:d];
            id<MTLTexture> c = [h newTextureWithDescriptor:d];
            ASSERT_NE(a, nil);
            ASSERT_NE(b, nil);
            ASSERT_NE(c, nil);
            id<MTLTexture> view = [b newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
            ASSERT_NE(view, nil);
            id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = view;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
            [b makeAliasable];
            b = nil;
            EXPECT_EQ(h.usedSize, 2 * quarter);
            gRM->raw->trimArenas(gRM->committed);
            [cb commit];
            [cb waitUntilCompleted];
            EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
            EXPECT_EQ(a.width + c.width, 512u);
        }
    }
}

// A command buffer that uses a purged heap's texture through a view of a view, or over a heap buffer, names neither the
// heap nor its root: its commit brings the arena back all the same
TEST_F(FacadeHeapUse, ACommitBringsBackAPurgedHeapItReachesThroughViewsAndBufferTextures)
{
    kernel_model::model.unknownUsesRefused = true;
    @autoreleasepool {
        id<MTLTexture> root = facade::texture(MTLStorageModeShared, heap);
        ASSERT_NE(root, nil);
        id<MTLTexture> view = [[root newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm]
            newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
        id<MTLBuffer> bytes = [heap newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        ASSERT_NE(bytes, nil);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:16
                                                                                    height:16
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> overBytes = [bytes newTextureWithDescriptor:d offset:0 bytesPerRow:64];
        ASSERT_NE(overBytes, nil);
        id<MTLTexture> viewOverBytes = [overBytes newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
        const uint32_t index = ((SwitchMetalHeap *)heap)->_arena - 1;
        const id<MTLTexture> reaching[3] = {view, overBytes, viewOverBytes};
        for (size_t k = 0; k < 3; k++) {
            SCOPED_TRACE(k);
            id<MTLTexture> used = reaching[k];
            ASSERT_NE(used, nil);
            ASSERT_TRUE(((SwitchMetalTexture *)used)->_gm20bOk);
            std::atomic<uint32_t> idle{0};
            gRM->raw->purgeArena(index, idle);
            ASSERT_TRUE(gRM->raw->arenaPurged(index));
            id<MTLCommandBuffer> cb = [[facade::device() newCommandQueue] commandBuffer];
            id<MTLRenderCommandEncoder> render = pass(cb);
            [render useResource:used usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
            [render endEncoding];
            commit(cb);
            EXPECT_FALSE(gRM->raw->arenaPurged(index));
        }
    }
}

}  // namespace
