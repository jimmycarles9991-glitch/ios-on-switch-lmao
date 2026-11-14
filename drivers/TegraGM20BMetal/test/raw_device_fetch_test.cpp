// SPDX-License-Identifier: MIT
#include "raw_device_harness.h"
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint32_t kTriangle = 3, kStrip = 4;  // MTLPrimitiveType

// A state whose fragment program fetches colour 0 and writes it: in its storage form (inside the pixel interlock, the
// attachment a storage image) or its barrier form (fetched through the texture unit)
raw::RuntimeRecord fetchingRecord(uint64_t key, bool storage)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    raw::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.entries = 1;
    pp.entry[0] = {0, raw::EntryFetch, 2, 0, 0, 0, 0};
    pp.flags = raw::PipelineFetches;
    if (storage) {
        pp.flags |= raw::PipelineStorageMetadata;
        pp.storageMask = pp.storageWriteMask = 1;
        pp.storageSamples = 1;
        pp.storageTic[0] = raw::formatInfo(kRGBA8)->tic0;
        r.blob = {0x1224, 1, 0x1a00, 0};
    } else {
        r.blob = {0x1a00, 0x1111};
    }
    return r;
}

class DeviceFetch : public harness::DeviceTest {
protected:
    raw::Texture target, small;
    raw::Texture *into = &target;
    raw::Buffer indices, arguments;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createTexture(32, 32, kRGBA8, small));
        ASSERT_TRUE(dev.createBuffer(4096, indices));
        ASSERT_TRUE(dev.createBuffer(256, arguments));
        const uint16_t strip[] = {0, 1, 2, 3, 0xffff, 4, 5, 6, 7};
        memcpy(indices.cpu, strip, sizeof strip);
        const uint32_t draw[4] = {6, 1, 0, 0};
        memcpy(arguments.cpu, draw, sizeof draw);
    }

    raw::Pipeline *pipeline(bool storage)
    {
        std::string why;
        raw::Pipeline *p = dev.addRecord(fetchingRecord(storage ? 0x5701 : 0xba01, storage), false, why);
        EXPECT_NE(p, nullptr) << why;
        return p;
    }

    // One draw of `pl` into the target, in a pass of its own: whether it was drawn and why not, its Metal draws, and how it
    // kept raster order (the device's fetch counts: inside the interlock, or split into one-triangle draws)
    struct Drawn {
        bool ok = false;
        uint32_t draws = 0;
        uint64_t interlocked = 0, split = 0, triangles = 0;
        std::string why;
    };

    template <class Draw> Drawn drawOnce(raw::Pipeline *pl, Draw draw, const raw::DepthStencilState *state = nullptr)
    {
        Drawn d;
        const raw::FetchCounts &k = dev.stats.fetch;
        const uint64_t interlocked = k.interlocked, split = k.sequential, triangles = k.sequentialTriangles;
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return d;
        raw::PassDesc pass;
        pass.color[0].texture = into;
        pass.color[0].load = raw::ColorAttachment::Load;
        pass.count = 1;
        pass.storageMask = pl->storageBits;
        pass.hiddenZeta = state != nullptr;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            d.why = "no pass";
            cb->abandon();
            return d;
        }
        e->setPipeline(pl);
        if (state)
            e->setDepthStencil(state);
        const char *why = nullptr;
        d.ok = draw(*e, &why);
        d.why = why ? why : "";
        e->end();
        d.draws = cb->draws;
        if (d.ok) {
            uint32_t seqno = 0;
            EXPECT_TRUE(cb->commit(seqno));
        } else {
            cb->abandon();
        }
        // the debug per-draw counts are the command buffer's until its commit folds them into the device's
        // (CommandBuffer::foldCounts)
        d.interlocked = k.interlocked - interlocked;
        d.split = k.sequential - split;
        d.triangles = k.sequentialTriangles - triangles;
        return d;
    }

    Drawn strip(raw::Pipeline *pl)
    {
        return drawOnce(pl, [](raw::RenderEncoder &e, const char **why) {
            return e.draw(kStrip, 0, 10, 1, 0, why);
        });
    }

    Drawn restart(raw::Pipeline *pl, uint8_t cull = 0)
    {
        return drawOnce(pl, [this, cull](raw::RenderEncoder &e, const char **why) {
            e.setCullMode(cull);
            return e.drawIndexed(kStrip, 9, 2, &indices, 0, 1, 0, 0, why);
        });
    }

    // RenderBox's quad: an indexed strip of four, under `state` over a Depth32Float_Stencil8 attachment where given
    Drawn quad(raw::Pipeline *pl, const raw::DepthStencilState *state = nullptr)
    {
        return drawOnce(
            pl,
            [this](raw::RenderEncoder &e, const char **why) {
                return e.drawIndexed(kStrip, 4, 2, &indices, 0, 1, 0, 0, why);
            },
            state);
    }

    Drawn instanced(raw::Pipeline *pl)
    {
        return drawOnce(pl, [](raw::RenderEncoder &e, const char **why) {
            return e.draw(kTriangle, 0, 6, 24, 0, why);
        });
    }

    Drawn indirect(raw::Pipeline *pl)
    {
        return drawOnce(pl, [this](raw::RenderEncoder &e, const char **why) {
            return e.drawIndirect(kTriangle, &arguments, 0, why);
        });
    }

    Drawn indexedIndirect(raw::Pipeline *pl)
    {
        return drawOnce(pl, [this](raw::RenderEncoder &e, const char **why) {
            return e.drawIndexedIndirect(kStrip, 2, &indices, 0, &arguments, 16, why);
        });
    }
};

TEST_F(DeviceFetch, TheStorageFormDrawsEveryTopologyWholeInsideTheInterlock)
{
    raw::Pipeline *pl = pipeline(true);
    ASSERT_NE(pl, nullptr);
    ASSERT_EQ(pl->storageBits, 1);
    ASSERT_EQ(pl->fetchBits & 1, 1);
    const uint32_t args[5] = {9, 1, 0, 0, 0};
    memcpy(static_cast<uint8_t *>(arguments.cpu) + 16, args, sizeof args);
    for (const auto &[name, drawn] :
         {std::pair{"strip", strip(pl)}, std::pair{"restart", restart(pl)}, std::pair{"instanced", instanced(pl)},
          std::pair{"indirect", indirect(pl)}, std::pair{"indexed indirect", indexedIndirect(pl)}}) {
        EXPECT_TRUE(drawn.ok) << name << ": " << drawn.why;
        EXPECT_EQ(drawn.draws, 1u) << name;
        EXPECT_EQ(drawn.interlocked, 1u) << name;
        EXPECT_EQ(drawn.split, 0u) << name;
    }
}

TEST_F(DeviceFetch, TheBarrierFormDrawsAStripATriangleAtATime)
{
    raw::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    ASSERT_EQ(pl->storageBits, 0);
    const Drawn s = strip(pl);
    EXPECT_TRUE(s.ok) << s.why;
    EXPECT_EQ(s.interlocked, 0u);
    EXPECT_EQ(s.split, 1u);
    EXPECT_EQ(s.triangles, 8u);
}

// Rule fetch.twin (raw_twin_rule.h): a storage form's strip over a measured render area is drawn by its barrier twin a
// triangle at a time; every shape the twin's draw would refuse, and a smaller area, stay whole inside the interlock
TEST_F(DeviceFetch, TheTwinRuleTakesTheTwinOnlyWhereItsDrawKeepsEveryShape)
{
    raw::Pipeline *pl = pipeline(true), *twin = pipeline(false);
    ASSERT_NE(pl, nullptr);
    ASSERT_NE(twin, nullptr);
    pl->barrierTwin = twin;
    const std::atomic<uint64_t> &taken = dev.stats.fetch.twinRule;
    const uint64_t before = taken;
    const Drawn s = strip(pl);
    EXPECT_TRUE(s.ok) << s.why;
    EXPECT_EQ(s.interlocked, 0u);
    EXPECT_EQ(s.split, 1u);
    EXPECT_EQ(s.triangles, 8u);
    EXPECT_EQ(taken - before, 1u);
    const uint32_t args[5] = {9, 1, 0, 0, 0};
    memcpy(static_cast<uint8_t *>(arguments.cpu) + 16, args, sizeof args);
    for (const auto &[name, drawn] :
         {std::pair{"restart", restart(pl)}, std::pair{"instanced", instanced(pl)}, std::pair{"indirect", indirect(pl)},
          std::pair{"indexed indirect", indexedIndirect(pl)}}) {
        EXPECT_TRUE(drawn.ok) << name << ": " << drawn.why;
        EXPECT_EQ(drawn.interlocked, 1u) << name;
        EXPECT_EQ(drawn.split, 0u) << name;
    }
    into = &small;
    const Drawn below = strip(pl);
    EXPECT_TRUE(below.ok) << below.why;
    EXPECT_EQ(below.interlocked, 1u) << "32x32 is under the smallest area measured";
    EXPECT_EQ(taken - before, 1u);
}

// Rule fetch.blend's accumulates: a pass that clears its target, or does not care, drops the mark of the command buffer
// whose rung-2 forms last blended into it; one that loads it keeps the mark
TEST_F(DeviceFetch, APassThatDoesNotLoadItsTargetForgetsItsRung2Blends)
{
    for (const auto load : {raw::ColorAttachment::Clear, raw::ColorAttachment::DontCare, raw::ColorAttachment::Load}) {
        target.ropBlendSerial = 5;
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = load;
        pass.count = 1;
        cb->render(pass)->end();
        cb->abandon();
        EXPECT_EQ(target.ropBlendSerial, load == raw::ColorAttachment::Load ? 5u : 0u) << int(load);
    }
}

// An indexed strip's runs are index ranges: the hardware's restart draws nothing in the runs holding 0xffff, as the
// whole strip draws no triangle there
TEST_F(DeviceFetch, TheBarrierFormDrawsAnIndexedStripATriangleAtATime)
{
    raw::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    const Drawn q = quad(pl);
    EXPECT_TRUE(q.ok) << q.why;
    EXPECT_EQ(q.interlocked, 0u);
    EXPECT_EQ(q.split, 1u);
    EXPECT_EQ(q.triangles, 2u);
    const Drawn r = restart(pl);
    EXPECT_TRUE(r.ok) << r.why;
    EXPECT_EQ(r.split, 1u);
    EXPECT_EQ(r.triangles, 7u);
}

// RenderBox's icon draw binds depth GreaterEqual with writes and an untouched stencil (traced on a Mac): its faces decide
// nothing; a stencil whose back face alone increments would make them decide
TEST_F(DeviceFetch, AnIndexedStripOverAStencilAttachmentIsRefusedOnlyWhereItsFacesDiffer)
{
    raw::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    raw::DepthStencilDesc renderBox;
    renderBox.depthCompare = 6;
    renderBox.depthWrite = true;
    const raw::DepthStencilState untouched(renderBox);
    const Drawn q = quad(pl, &untouched);
    EXPECT_TRUE(q.ok) << q.why;
    EXPECT_EQ(q.triangles, 2u);
    raw::DepthStencilDesc winding = renderBox;
    winding.back.pass = 6;
    const raw::DepthStencilState twoSided(winding);
    EXPECT_FALSE(quad(pl, &twoSided).ok);
}

TEST_F(DeviceFetch, TheBarrierFormRefusesWhatOneTriangleADrawCannotKeep)
{
    raw::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    const Drawn culled = restart(pl, 1);
    EXPECT_FALSE(culled.ok);
    EXPECT_EQ(culled.why, "framebuffer-fetch ordering requires an interlocked program or available bounded geometry");
    EXPECT_FALSE(instanced(pl).ok);
    EXPECT_FALSE(indirect(pl).ok);
}

}  // namespace
