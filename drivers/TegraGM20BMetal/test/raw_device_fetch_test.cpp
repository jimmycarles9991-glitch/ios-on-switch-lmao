// SPDX-License-Identifier: MIT
#include "raw_device_harness.h"
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
    raw::Texture target;
    raw::Buffer indices, arguments;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
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

    template <class Draw> Drawn drawOnce(raw::Pipeline *pl, Draw draw)
    {
        Drawn d;
        const raw::FetchCounts &k = dev.stats.fetch;
        const uint64_t interlocked = k.interlocked, split = k.sequential, triangles = k.sequentialTriangles;
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return d;
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = raw::ColorAttachment::Load;
        pass.count = 1;
        pass.storageMask = pl->storageBits;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            d.why = "no pass";
            cb->abandon();
            return d;
        }
        e->setPipeline(pl);
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

    Drawn restart(raw::Pipeline *pl)
    {
        return drawOnce(pl, [this](raw::RenderEncoder &e, const char **why) {
            return e.drawIndexed(kStrip, 9, 2, &indices, 0, 1, 0, 0, why);
        });
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

TEST_F(DeviceFetch, TheBarrierFormRefusesWhatOneTriangleADrawCannotKeep)
{
    raw::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    EXPECT_FALSE(restart(pl).ok);
    EXPECT_FALSE(instanced(pl).ok);
    EXPECT_FALSE(indirect(pl).ok);
}

}  // namespace
