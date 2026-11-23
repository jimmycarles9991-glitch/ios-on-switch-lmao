// SPDX-License-Identifier: MIT
// An MTLHeap an encoder used (useHeap) on gm20b::Device (device_harness.h, heap_members.h): a command buffer names it
// once, and its job names the heap's arena as one BO however many resources the arena holds, of whatever kinds (each kind
// the device makes, its memory a mapping of the arena's BO under the kind's PTE kind), plus each resource the heap made
// outside it. A shared heap's arena is write-combined and host-written; a private heap's places buffers too.
#include "device_harness.h"

#include <algorithm>
#include <array>
#include "../heap_members.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kRGB9E5 = 93, kBC1 = 130, kDepth32 = 252, kDepth32Stencil8 = 260;

struct Kind {
    const char *name;
    gm20b::TextureAsk ask;
};

gm20b::TextureAsk ask(gm20b::TextureAsk::Kind kind, uint32_t format, bool tiled = false)
{
    gm20b::TextureAsk a;
    a.kind = kind;
    a.width = 64;
    a.height = kind == gm20b::TextureAsk::Line ? 1 : 48;
    a.format = format;
    a.tiled = tiled;
    return a;
}

// Every kind a heap of `blockLinear` (a private one) holds of the device's (TegraGM20BMetalHeap.mm heapTextureAsk)
std::vector<Kind> kinds(bool blockLinear)
{
    using K = gm20b::TextureAsk;
    std::vector<Kind> out = {{"2D", ask(K::Plain, kRGBA8, blockLinear)},
                             {"2D of a format no target takes", ask(K::Plain, kRGB9E5, blockLinear)},
                             {"block-compressed 2D", ask(K::Plain, kBC1, true)},
                             {"1D", ask(K::Line, kRGBA8)}};
    K mips = ask(K::Mips, kRGBA8), array = ask(K::Array, kRGBA8), cube = ask(K::Array, kRGBA8),
      volume = ask(K::Volume, kRGBA8);
    mips.levels = 5;
    array.type = gm20b::fx::kTexTwoDArray;
    array.layers = 4;
    cube.type = gm20b::fx::kTexCube;
    cube.height = 64;
    cube.layers = 6;
    volume.depth = 4;
    volume.view2d = true;
    K blockMips = ask(K::Block, kBC1), blockArray = ask(K::Block, kBC1), blockCube = ask(K::Block, kBC1),
      blockVolume = ask(K::Block, kBC1);
    blockMips.levels = 5;
    blockMips.type = gm20b::fx::kTexTwoD;
    blockArray.layers = 3;
    blockArray.type = gm20b::fx::kTexTwoDArray;
    blockCube.height = 64;
    blockCube.layers = 6;
    blockCube.type = gm20b::fx::kTexCube;
    blockVolume.depth = 4;
    blockVolume.levels = 2;
    blockVolume.type = gm20b::fx::kTexThreeD;
    out.insert(out.end(), {{"mips", mips},
                           {"2D array", array},
                           {"cube", cube},
                           {"3D", volume},
                           {"block-compressed mips", blockMips},
                           {"block-compressed 2D array", blockArray},
                           {"block-compressed cube", blockCube},
                           {"block-compressed 3D", blockVolume}});
    K depth = ask(K::Zeta, kDepth32), stencil = ask(K::Zeta, kDepth32Stencil8), depthCube = ask(K::Zeta, kDepth32);
    depth.sampled = depthCube.sampled = true;
    depthCube.type = gm20b::fx::kTexCube;
    depthCube.height = 64;
    depthCube.layers = 6;
    depthCube.levels = 3;
    out.insert(out.end(), {{"depth", depth}, {"depth-stencil", stencil}, {"depth cube of levels", depthCube}});
    if (!blockLinear)
        return out;
    K msaa = ask(K::Multisample, kRGBA8), msaaDepth = ask(K::Zeta, kDepth32);
    msaa.samples = msaaDepth.samples = 4;
    out.insert(out.end(), {{"MSAA", msaa}, {"MSAA depth", msaaDepth}});
    return out;
}

class DeviceHeapUse : public harness::DeviceTest {
protected:
    uint32_t arena = 0;

    void makeArena(bool blockLinear, uint64_t bytes = 32u << 20)
    {
        ASSERT_TRUE(dev.createArena(bytes, blockLinear, arena));
    }

    // One command buffer of one copy that used `heap` `uses` times: its job's uses
    std::vector<TegraGM20BAbiSurfaceUse> submit(const gm20b::HeapMembers &heap, uint32_t uses = 1)
    {
        gm20b::Buffer from, to;
        EXPECT_TRUE(dev.createBuffer(64, from) && dev.createBuffer(64, to));
        gm20b::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!cb)
            return {};
        cb->internalCompletion();
        for (uint32_t i = 0; i < uses; i++)
            cb->useHeap(heap);
        cb->use(from, false);
        cb->use(to, true, false);
        gm20b::BlitEncoder *e = cb->blit();
        e->copyBytes(from.va, to.va, 64);
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        copied_ = {kernel_model::model.resolve({from.va, TegraGM20BSurfaceGpuAddress, 0}),
                   kernel_model::model.resolve({to.va, TegraGM20BSurfaceGpuAddress, 0})};
        return kernel_model::model.jobs.back().uses;
    }

    // `uses` less the last copy's own buffers: after their first CPU lease they are named by ranged address, one use
    // each, which says nothing about how the arena is named
    size_t notCopied(const std::vector<TegraGM20BAbiSurfaceUse> &uses) const
    {
        return size_t(std::count_if(uses.begin(), uses.end(), [&](const TegraGM20BAbiSurfaceUse &u) {
            return u.handle != copied_[0] && u.handle != copied_[1];
        }));
    }

    std::array<uint64_t, 2> copied_{};

    size_t named(const std::vector<TegraGM20BAbiSurfaceUse> &uses, uint64_t handle, uint32_t *access = nullptr)
    {
        size_t n = 0;
        for (const auto &use : uses)
            if (use.handle == handle) {
                n++;
                if (access)
                    *access = use.access;
            }
        return n;
    }

    void everyKindOnce(bool blockLinear, bool placement, const Kind &kind);

    void place(uint32_t textures)
    {
        for (uint32_t i = 0; i < textures; i++) {
            gm20b::Texture t;
            ASSERT_TRUE(dev.arenaTexture(arena, 16, 16, kRGBA8, t)) << i;
        }
    }
};

TEST_F(DeviceHeapUse, AHeapsArenaIsOneUseHoweverManyResourcesItHolds)
{
    makeArena(true);
    const gm20b::HeapMembers heap(arena, false);
    const uint64_t bo = gm20b::DeviceHarness::arena(dev, arena).handle;
    place(4);
    const auto few = submit(heap, 3);
    place(400);
    const auto many = submit(heap, 3);
    uint32_t access = 0;
    EXPECT_EQ(named(few, bo), 1u);
    EXPECT_EQ(named(many, bo, &access), 1u);
    EXPECT_EQ(access & (TegraGM20BSurfaceWrite | TegraGM20BSurfaceHostWritten), 0u);
    EXPECT_EQ(few.size(), many.size());
}

// 4 then 404 of `kind` in an arena of their footprint (at offsets of it in a placement heap's): the job names the arena once,
// each texture lies in a mapping of the arena's BO, nothing is outside it
void DeviceHeapUse::everyKindOnce(bool blockLinear, bool placement, const Kind &kind)
{
    SCOPED_TRACE(std::string(kind.name) + (blockLinear ? ", private" : ", shared") + (placement ? ", placement" : ""));
    constexpr uint32_t kMany = 404;
    uint64_t bytes = 0, align = 0;
    ASSERT_TRUE(gm20b::Device::footprint(kind.ask, bytes, align));
    const uint64_t stride = (bytes + align - 1) / align * align;
    ASSERT_TRUE(dev.createArena(stride * kMany, blockLinear, arena, placement));
    const gm20b::HeapMembers heap(arena, !blockLinear);
    const uint64_t bo = gm20b::DeviceHarness::arena(dev, arena).handle;
    std::vector<gm20b::Texture> made(kMany);
    const auto place = [&](uint32_t from, uint32_t to) {
        for (uint32_t i = from; i < to; i++) {
            gm20b::ArenaAt at{arena, placement ? std::optional<uint64_t>(i * stride) : std::nullopt};
            ASSERT_TRUE(dev.arenaTexture(at, kind.ask, made[i])) << i;
            EXPECT_TRUE(made[i].arena);
            EXPECT_EQ(made[i].span.bytes, bytes);
            EXPECT_EQ(kernel_model::model.resolve({made[i].va, TegraGM20BSurfaceGpuAddress, 0}), bo) << i;
        }
    };
    place(0, 4);
    const auto few = submit(heap, 3);
    place(4, kMany);
    const auto many = submit(heap, 3);
    EXPECT_EQ(named(few, bo), 1u);
    EXPECT_EQ(named(many, bo), 1u);
    EXPECT_EQ(notCopied(few), notCopied(many));
    EXPECT_EQ(heap.outside(), 0u);
    for (gm20b::Texture &x : made)
        dev.destroyTexture(x);
    dev.destroyArena(arena);
}

TEST_F(DeviceHeapUse, EveryKindIsOneUseOfTheArenaAtFourAndAt404)
{
    for (const bool blockLinear : {true, false})
        for (const bool placement : {false, true})
            for (const Kind &kind : kinds(blockLinear))
                everyKindOnce(blockLinear, placement, kind);
}

// A heap's block-compressed chain is the image one in memory of its own is (createBlockTexture): its layout, and its header
// but for the address words
TEST_F(DeviceHeapUse, ABlockCompressedChainInAHeapIsTheImageOfItsOwn)
{
    constexpr uint32_t kASTC4x4 = 204;
    gm20b::TextureAsk a = ask(gm20b::TextureAsk::Block, kASTC4x4);
    a.width = 100;
    a.height = 60;
    a.levels = 7;
    a.type = gm20b::fx::kTexTwoD;
    makeArena(true);
    gm20b::Texture placed, own;
    ASSERT_TRUE(dev.arenaTexture(gm20b::ArenaAt{arena, std::nullopt}, a, placed));
    ASSERT_TRUE(dev.createBlockTexture(100, 60, 7, 1, kASTC4x4, gm20b::fx::kTexTwoD, own));
    ASSERT_NE(placed.chain, nullptr);
    ASSERT_NE(own.chain, nullptr);
    EXPECT_TRUE(placed.arena);
    EXPECT_FALSE(own.arena);
    uint64_t bytes = 0, align = 0;
    ASSERT_TRUE(gm20b::Device::footprint(a, bytes, align));
    EXPECT_EQ(placed.span.bytes, bytes);
    EXPECT_EQ(placed.chain->layout.bytes, own.chain->layout.bytes);
    EXPECT_EQ(placed.chain->va, placed.va);
    EXPECT_EQ(placed.width, own.width);
    EXPECT_EQ(placed.pitch, own.pitch);
    EXPECT_EQ(placed.blockHeight, own.blockHeight);
    const auto p = gm20b::DeviceHarness::header(dev, placed.tic), o = gm20b::DeviceHarness::header(dev, own.tic);
    for (size_t w : {0, 3, 4, 5, 6, 7})
        EXPECT_EQ(p[w], o[w]) << "header word " << w;
    dev.destroyTexture(placed);
    dev.destroyTexture(own);
}

TEST_F(DeviceHeapUse, BuffersAreOneUseOfTheArenaAtFourAndAt404)
{
    for (const bool blockLinear : {true, false}) {
        makeArena(blockLinear);
        const gm20b::HeapMembers heap(arena, !blockLinear);
        const uint64_t bo = gm20b::DeviceHarness::arena(dev, arena).handle;
        std::vector<gm20b::Buffer> made(404);
        for (uint32_t i = 0; i < 4; i++)
            ASSERT_TRUE(dev.arenaBuffer(arena, 1000, made[i]));
        const auto few = submit(heap, 3);
        for (uint32_t i = 4; i < made.size(); i++)
            ASSERT_TRUE(dev.arenaBuffer(arena, 1000, made[i]));
        const auto many = submit(heap, 3);
        EXPECT_EQ(named(few, bo), 1u);
        EXPECT_EQ(named(many, bo), 1u);
        EXPECT_EQ(few.size(), many.size());
        EXPECT_EQ(kernel_model::model.resolve({made.back().va, TegraGM20BSurfaceGpuAddress, 0}), bo);
    }
}

// One of each kind in one private arena, a buffer too: one use, and one mapping of the BO a kind (its own, its pitch view,
// the shared block-linear kind's, and one per depth format)
TEST_F(DeviceHeapUse, AHeapOfEveryKindIsOneUseOfOneBo)
{
    makeArena(true, 64u << 20);
    const gm20b::HeapMembers heap(arena, false);
    const uint64_t bo = gm20b::DeviceHarness::arena(dev, arena).handle;
    std::vector<gm20b::Texture> made;
    for (const Kind &kind : kinds(true)) {
        made.emplace_back();
        ASSERT_TRUE(dev.arenaTexture({arena, std::nullopt}, kind.ask, made.back())) << kind.name;
    }
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 4096, b));
    for (const gm20b::Texture &x : made)
        EXPECT_EQ(kernel_model::model.resolve({x.va, TegraGM20BSurfaceGpuAddress, 0}), bo);
    EXPECT_EQ(named(submit(heap), bo), 1u);
    // its pitch view, and Depth32Float's and Depth32Float_Stencil8's zeta kinds
    EXPECT_EQ(gm20b::DeviceHarness::arenaViews(dev, arena), 3u);
}

// Each kind's footprint is the one its placement takes, at the alignment its layout asks (NVK's image alignment for a
// block-linear one, as m2v's twin over it in place takes it)
TEST(DeviceFootprint, IsWhatAPlacementTakes)
{
    for (const bool blockLinear : {true, false})
        for (const Kind &kind : kinds(blockLinear)) {
            uint64_t bytes = 0, align = 0;
            ASSERT_TRUE(gm20b::Device::footprint(kind.ask, bytes, align)) << kind.name;
            EXPECT_NE(bytes, 0u) << kind.name;
            EXPECT_EQ(align & (align - 1), 0u) << kind.name;
            const bool pitch =
                kind.ask.kind == gm20b::TextureAsk::Plain && (!blockLinear || kind.ask.format == kRGB9E5);
            EXPECT_EQ(align % (pitch ? 256 : gm20b::kBlAlign), 0u) << kind.name;
            EXPECT_EQ(bytes % (pitch ? 256 : gm20b::kBlAlign), 0u) << kind.name;
        }
    gm20b::TextureAsk none = ask(gm20b::TextureAsk::Plain, kBC1, false);
    uint64_t bytes = 0, align = 0;
    EXPECT_FALSE(gm20b::Device::footprint(none, bytes, align));  // a block-compressed texture is never pitch
}

TEST_F(DeviceHeapUse, AResourceMadeOutsideTheArenaIsAUseUntilItGoes)
{
    makeArena(true);
    gm20b::HeapMembers heap(arena, false);
    gm20b::Buffer outside;
    ASSERT_TRUE(dev.createBuffer(4096, outside));
    const uint64_t bo = kernel_model::model.resolve({outside.va, TegraGM20BSurfaceGpuAddress, 0});
    const size_t before = named(submit(heap), bo);
    heap.add({outside.va, outside.length, false});
    EXPECT_EQ(named(submit(heap), bo), before + 1);
    heap.remove(outside.va);
    EXPECT_EQ(named(submit(heap), bo), before);
    EXPECT_EQ(heap.outside(), 0u);
}

TEST_F(DeviceHeapUse, ASharedHeapsArenaIsWriteCombinedAndNamedHostWritten)
{
    makeArena(false);
    EXPECT_TRUE(gm20b::DeviceHarness::arenaWriteCombined(dev, arena));
    gm20b::Buffer b;
    ASSERT_TRUE(dev.arenaBuffer(arena, 256, b));
    EXPECT_TRUE(b.wc);
    const gm20b::HeapMembers heap(arena, true);
    uint32_t access = 0;
    EXPECT_EQ(named(submit(heap), gm20b::DeviceHarness::arena(dev, arena).handle, &access), 1u);
    EXPECT_TRUE(access & TegraGM20BSurfaceHostWritten);
}

TEST_F(DeviceHeapUse, APrivateHeapPlacesBuffersInItsPitchView)
{
    makeArena(true);
    EXPECT_FALSE(gm20b::DeviceHarness::arenaWriteCombined(dev, arena));
    gm20b::Buffer b;
    gm20b::Texture t;
    ASSERT_TRUE(dev.arenaBuffer(arena, 1000, b));
    ASSERT_TRUE(dev.arenaTexture(arena, 64, 64, kRGBA8, t));
    EXPECT_TRUE(b.arena);
    EXPECT_FALSE(b.wc);
    EXPECT_NE(b.va - b.span.offset, t.va - t.span.offset);  // the pitch view and the block-linear view of one BO
}

TEST(HeapMembers, ChangeOnlyAsTheHeapMakesAndFreesResources)
{
    gm20b::HeapMembers heap(3, true);
    EXPECT_EQ(heap.arena(), 3u);
    EXPECT_TRUE(heap.hostVisible());
    const uint64_t first = heap.generation();
    heap.add({0x10000, 256, true});
    heap.add({0x20000, 512, false});
    heap.add({0, 64, false});  // nothing to name
    EXPECT_EQ(heap.outside(), 2u);
    heap.remove(0x10000);
    heap.remove(0x99999);
    EXPECT_EQ(heap.outside(), 1u);
    EXPECT_EQ(heap.generation(), first + 3);
    uint64_t va = 0;
    heap.each([&](const gm20b::HeapMember &m) {
        va = m.va;
    });
    EXPECT_EQ(va, 0x20000u);
}

}  // namespace
