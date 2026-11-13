// SPDX-License-Identifier: MIT
// useHeap through the raw facade on the host (raw_facade_harness.h): on render and compute encoders a heap is one use of
// the command buffer's plan and one use of its arena in each job, however often it is used and however many resources the
// arena holds (RawMetalRenderEncoder.mm, RawMetalAnswers.mm, RawMetalEncode.mm). A heap resource used alone is the range it
// holds of the arena.
#include "raw_device_harness.h"
#include "raw_facade_harness.h"

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
        ASSERT_NE(((RMHeap *)heap)->_arena, 0u);
        arena = raw::DeviceHarness::arena(*gRM->raw, ((RMHeap *)heap)->_arena - 1).handle;
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
            ASSERT_TRUE(((RMTexture *)t)->_raw.arena) << i;
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
        RMCommandPlan &plan = ((RMCommandBuffer *)cb)->_plan;
        plan.prepareRange(0, plan.size());
        plan.forEachResource(0, plan.size(), RMResourceKind::Heap, [&](const RMRangeResource &) {
            used.planHeaps++;
        });
        plan.forEachResource(0, plan.size(), RMResourceKind::Texture, [&](const RMRangeResource &) {
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
        RMCommandPlan &plan = ((RMCommandBuffer *)cb)->_plan;
        plan.prepareRange(0, plan.size());
        size_t heaps = 0;
        plan.forEachResource(0, plan.size(), RMResourceKind::Heap, [&](const RMRangeResource &) {
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

}  // namespace
