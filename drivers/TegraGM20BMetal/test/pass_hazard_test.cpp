// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/pass_hazard.h"

namespace {

using gm20b::HazardImage;
using gm20b::HazardLayout;
using gm20b::PassChain;
using gm20b::PassUses;

HazardImage image(uint64_t va, uint32_t format, uint32_t width, uint32_t height,
                  HazardLayout layout = HazardLayout::BlockLinear)
{
    HazardImage i;
    i.va = va;
    i.bytes = uint64_t(width) * height * (format == gm20b::kHazardRgba16f ? 8 : 4);
    i.format = format;
    i.width = width;
    i.height = height;
    i.layout = layout;
    i.plain = true;
    return i;
}

PassUses pass(uint32_t width, uint32_t height)
{
    PassUses p;
    p.known = true;
    p.areaWidth = width;
    p.areaHeight = height;
    return p;
}

PassChain after(const HazardImage &written)
{
    PassUses a = pass(written.width, written.height);
    a.write(written);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    return chain;
}

TEST(PassHazard, PlainColorTargetReadAndWriteAreHeldAcrossFormatsLayoutsAndSizes)
{
    constexpr uint32_t kFormats[] = {10, 70, 71, 80, 81, 90, 115};
    for (uint32_t fmt : kFormats) {
        for (HazardLayout layout : {HazardLayout::BlockLinear, HazardLayout::Pitch, HazardLayout::Compressed}) {
            for (const auto [w, h] : std::initializer_list<std::pair<uint32_t, uint32_t>>{
                     {64, 64}, {180, 180}, {320, 180}, {1280, 720}}) {
                const HazardImage t = image(0x10000, fmt, w, h, layout);
                PassUses readSame = pass(w, h);
                readSame.write(image(0x800000, fmt, w, h, layout));
                readSame.sample(t, false);
                EXPECT_TRUE(gm20b::pixelBarrierHolds(after(t), readSame));

                PassUses readHalf = pass(w / 2, h / 2);
                readHalf.write(image(0x800000, fmt, w / 2, h / 2, layout));
                readHalf.sample(t, false);
                EXPECT_TRUE(gm20b::pixelBarrierHolds(after(t), readHalf));

                PassUses over = pass(w, h);
                over.write(t);
                EXPECT_TRUE(gm20b::pixelBarrierHolds(after(t), over));
            }
        }
    }
}

TEST(PassHazard, CrossStageAndNonPlainDependenciesKeepTheDrain)
{
    const HazardImage t = image(0x10000, gm20b::kHazardBgra8, 1280, 720);
    const auto reads = [&](PassUses b) {
        b.sample(t, false);
        return gm20b::pixelBarrierHolds(after(t), b);
    };
    PassUses b = pass(1280, 720);
    b.known = false;
    EXPECT_FALSE(reads(b));
    b = pass(1280, 720);
    b.vertexSamples = true;
    EXPECT_FALSE(reads(b));
    b = pass(1280, 720);
    b.depthStencil = true;
    EXPECT_FALSE(reads(b));
    b = pass(1280, 720);
    b.sample(t, true);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(after(t), b));
    HazardImage nonPlain = t;
    nonPlain.plain = false;
    b = pass(1280, 720);
    b.sample(nonPlain, false);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(after(nonPlain), b));
    PassChain unknown = after(t);
    unknown.valid = false;
    EXPECT_FALSE(gm20b::pixelBarrierHolds(unknown, pass(64, 64)));
}

TEST(PassHazard, ATargetAnEarlierPassSampledKeepsTheDrain)
{
    const HazardImage t = image(0x10000, gm20b::kHazardBgra8, 64, 64), u = image(0x20000, gm20b::kHazardBgra8, 64, 64);
    PassUses a = pass(64, 64);
    a.write(u);
    a.sample(t, false);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    PassUses b = pass(64, 64);
    b.write(t);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(chain, b));
}

TEST(PassHazard, TheChainHoldsEveryPassSinceTheDrain)
{
    const HazardImage big = image(0x10000, gm20b::kHazardBgra8, 1280, 720),
                      small = image(0x900000, gm20b::kHazardBgra8, 32, 32);
    PassChain chain = after(big);
    PassUses unrelated = pass(32, 32);
    unrelated.write(small);
    ASSERT_TRUE(gm20b::pixelBarrierHolds(chain, unrelated));
    gm20b::advanceChain(chain, unrelated, false);
    PassUses vsReader = pass(640, 360);
    vsReader.vertexSamples = true;
    vsReader.write(image(0xa00000, gm20b::kHazardBgra8, 640, 360));
    vsReader.sample(big, false);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(chain, vsReader));
    gm20b::advanceChain(chain, vsReader, true);
    EXPECT_EQ(chain.writtenCount, 1u);
}

// A memoryless attachment's backing holds nothing a later pass may read (its contents are undefined at each pass's start):
// passes whose scratch shares that memory depend on nothing through it, whatever its size
TEST(PassHazard, ScratchOverTheSameBackingIsNoDependency)
{
    const HazardImage backing = image(0x4000000, gm20b::kHazardRgba16f, 768, 1344);
    PassUses a = pass(722, 1282);
    a.write(image(0x10000, gm20b::kHazardBgra8, 722, 1282));
    a.scratchWrite(backing);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    PassUses b = pass(1792, 1472);
    b.write(image(0x2000000, gm20b::kHazardBgra8, 1792, 1472));
    HazardImage larger = backing;
    larger.bytes *= 3;
    b.scratchWrite(larger);
    EXPECT_EQ(gm20b::drainCause(chain, b), gm20b::DrainCause::None);
}

// Scratch over memory a real image of the chain names, or a real image over an earlier pass's scratch, keeps the drain
TEST(PassHazard, ScratchOverARealImageKeepsTheDrain)
{
    const HazardImage target = image(0x10000, gm20b::kHazardBgra8, 320, 180);
    PassUses a = pass(320, 180);
    a.write(target);
    PassChain chain;
    gm20b::advanceChain(chain, a, true);
    PassUses b = pass(320, 180);
    b.scratchWrite(target);
    EXPECT_EQ(gm20b::drainCause(chain, b), gm20b::DrainCause::Scratch);
    PassUses c = pass(320, 180);
    c.scratchWrite(image(0x900000, gm20b::kHazardRgba16f, 320, 180));
    gm20b::advanceChain(chain, c, true);
    PassUses d = pass(320, 180);
    d.sample(image(0x900000, gm20b::kHazardRgba16f, 320, 180), false);
    EXPECT_EQ(gm20b::drainCause(chain, d), gm20b::DrainCause::Scratch);
    PassUses e = pass(320, 180);
    e.write(image(0x900000, gm20b::kHazardRgba16f, 320, 180));
    EXPECT_EQ(gm20b::drainCause(chain, e), gm20b::DrainCause::Scratch);
}

// The same textures sampled by every pass are listed once: forty passes of four textures of a set of five stay one chain
TEST(PassHazard, TheChainListsEachImageOnce)
{
    PassChain chain;
    for (uint32_t p = 0; p < 40; p++) {
        PassUses next = pass(722, 1282);
        next.write(image(0x10000000 + uint64_t(p) * 0x1000000, gm20b::kHazardBgra8, 722, 1282));
        for (uint32_t t = 0; t < 4; t++)
            next.sample(image(0x100000 + uint64_t((p + t) % 5) * 0x10000, gm20b::kHazardBgra8, 64, 64), false);
        ASSERT_TRUE(p == 0 || gm20b::pixelBarrierHolds(chain, next)) << p;
        gm20b::advanceChain(chain, next, p == 0);
    }
    EXPECT_TRUE(chain.valid);
    EXPECT_EQ(chain.sampledCount, 5u);
    EXPECT_EQ(chain.writtenCount, 40u);
}

TEST(PassHazard, EachDrainSaysWhy)
{
    const HazardImage big = image(0x10000, gm20b::kHazardBgra8, 1280, 720);
    HazardImage nonPlain = big;
    nonPlain.plain = false;
    PassUses unknown = pass(64, 64);
    unknown.known = false;
    EXPECT_EQ(gm20b::drainCause(after(big), unknown), gm20b::DrainCause::Unknown);
    PassChain invalid = after(big);
    invalid.valid = false;
    EXPECT_EQ(gm20b::drainCause(invalid, pass(64, 64)), gm20b::DrainCause::Chain);
    PassUses reader = pass(640, 360);
    reader.vertexSamples = true;
    reader.sample(big, false);
    EXPECT_EQ(gm20b::drainCause(after(big), reader), gm20b::DrainCause::Read);
    PassUses over = pass(1280, 720);
    over.write(nonPlain);
    EXPECT_EQ(gm20b::drainCause(after(nonPlain), over), gm20b::DrainCause::Write);
    PassUses depth = pass(64, 64);
    depth.depthStencil = true;
    EXPECT_EQ(gm20b::drainCause(after(big), depth), gm20b::DrainCause::Depth);
}

TEST(PassHazard, IndependentPassesNeedNoBarrierWhileSharedScratchKeepsPixelBarrier)
{
    const HazardImage a = image(0x10000, gm20b::kHazardBgra8, 1280, 720),
                      b = image(0x900000, gm20b::kHazardBgra8, 640, 360),
                      sharedTex = image(0x1200000, gm20b::kHazardBgra8, 64, 64),
                      backing = image(0x2000000, gm20b::kHazardRgba16f, 320, 180);
    PassUses first = pass(1280, 720);
    first.write(a);
    first.sample(sharedTex, false);
    PassChain chain;
    gm20b::advanceChain(chain, first, true);

    PassUses disjoint = pass(640, 360);
    disjoint.write(b);
    disjoint.sample(sharedTex, false);
    EXPECT_TRUE(gm20b::passesIndependent(chain, disjoint));

    PassUses readsA = pass(640, 360);
    readsA.write(b);
    readsA.sample(a, false);
    EXPECT_FALSE(gm20b::passesIndependent(chain, readsA));

    PassUses writesSharedTex = pass(64, 64);
    writesSharedTex.write(sharedTex);
    EXPECT_FALSE(gm20b::passesIndependent(chain, writesSharedTex));

    PassUses withScratch = pass(320, 180);
    withScratch.write(a);
    withScratch.scratchWrite(backing);
    PassChain scratchChain;
    gm20b::advanceChain(scratchChain, withScratch, true);
    PassUses nextScratch = pass(320, 180);
    nextScratch.write(b);
    nextScratch.scratchWrite(backing);
    EXPECT_FALSE(gm20b::passesIndependent(scratchChain, nextScratch));
    EXPECT_TRUE(gm20b::pixelBarrierHolds(scratchChain, nextScratch));
}

TEST(PassHazard, VolatilePitchTextureIsExcludedFromPlainAndPixelBarrier)
{
    struct MockTexture {
        struct Import {
            uint64_t base = 0, bytes = 0;
        };

        struct Chain {
            struct {
                uint64_t bytes = 0;
            } layout;
        };

        struct {
            uint64_t bytes = 0;
        } span;

        struct {
            bool gpuVolatile = false;
        } storage;

        uint64_t va = 0, layerBytes = 0;
        uint32_t format = 0, width = 0, height = 0, pitch = 0, depth = 1, layers = 1;
        uint8_t samples = 1;
        bool comptags = false, blockLinear = false, memoryless = false, zeta = false, planar = false;
        const Import *import = nullptr;
        const Chain *chain = nullptr;
    };

    MockTexture cached;
    cached.va = 0x10000;
    cached.format = gm20b::kHazardBgra8;
    cached.width = 320;
    cached.height = 180;
    cached.pitch = 320 * 4;
    cached.storage.gpuVolatile = false;

    MockTexture vol = cached;
    vol.storage.gpuVolatile = true;

    const HazardImage cachedImg = gm20b::hazardImage(cached);
    const HazardImage volImg = gm20b::hazardImage(vol);
    EXPECT_TRUE(cachedImg.plain);
    EXPECT_EQ(cachedImg.layout, HazardLayout::Pitch);
    EXPECT_FALSE(volImg.plain);
    EXPECT_EQ(volImg.layout, HazardLayout::Pitch);

    PassUses cachedLevel = pass(160, 90);
    cachedLevel.write(image(0x200000, gm20b::kHazardBgra8, 160, 90));
    cachedLevel.sample(cachedImg, false);
    EXPECT_TRUE(gm20b::pixelBarrierHolds(after(cachedImg), cachedLevel));

    PassUses volLevel = pass(160, 90);
    volLevel.write(image(0x200000, gm20b::kHazardBgra8, 160, 90));
    volLevel.sample(volImg, false);
    EXPECT_FALSE(gm20b::pixelBarrierHolds(after(volImg), volLevel));
    EXPECT_EQ(gm20b::drainCause(after(volImg), volLevel), gm20b::DrainCause::Read);
}

}  // namespace
