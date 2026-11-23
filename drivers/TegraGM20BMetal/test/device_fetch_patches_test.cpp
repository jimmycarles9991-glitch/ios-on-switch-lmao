// SPDX-License-Identifier: MIT
// Patch draws of a tessellation state that reads the colour it writes, on gm20b::Device (fetch_order.h): its storage
// form draws every one whole inside the pixel interlock, and a form that fetches through the texture unit is refused as
// RefGeometry, one to three patches of one instance among them, since the fetch barrier between BEGINs cannot order the
// triangles the tessellator makes of one patch.
#include "device_harness.h"
#include "../icb.h"
#include "../mme.h"
#include "../record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include <functional>

namespace {

constexpr uint32_t kRGBA8 = 70, kControlWord = 5, kPatch = gm20b::icb::kPatchPrimitive;

gm20b::RuntimeRecord fetchingPatches(uint64_t key, bool storage)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    for (gm20b::CapturedProgram &p : r.tessPrograms) {
        const auto stage = &p - r.tessPrograms;
        p.image.assign(80 + 64, 0);
        p.image[80] = uint8_t(1 + stage);
        p.id = "fetchtess" + std::to_string(stage);
        p.gprs = 12;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    gm20b::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.controlWord = kControlWord;
    pp.controlPoints = 4;
    gm20b::setEntryCount(pp, gm20b::TessBufferCount + 1);
    for (uint8_t k = 0; k < gm20b::TessBufferCount; k++)
        pp.entry[k] = {uint16_t(16 * (k + 1)),
                       gm20b::EntryTessBuffer,
                       1,
                       k,
                       uint8_t(1 | (kControlWord + gm20b::kTessResidualWord[k]) << 2),
                       0,
                       0};
    pp.entry[gm20b::TessBufferCount] = {0, gm20b::EntryFetch, 2, 0, 0, 0, 0};
    pp.pushMask = 0x3fu << (kControlWord - 1);
    pp.flags = gm20b::PipelineFetches;
    if (storage) {
        pp.flags |= gm20b::PipelineStorageMetadata;
        pp.storageMask = pp.storageWriteMask = 1;
        pp.storageSamples = 1;
        pp.storageTic[0] = gm20b::formatInfo(kRGBA8)->tic0;
        r.blob = {0x1224, 1, 0x1a00, 0};
    } else {
        r.blob = {0x1a00, 0x1111};
    }
    r.extras = {{0x0320, 0x202}, {0x0dcc, 1}, {0x2080, 0x21}, {0x2090, 1}, {0x20c0, 0x31}, {0x20d0, 2}};
    r.restore = {{0x2080, 0x20}, {0x20c0, 0x30}};
    return r;
}

// What the jobs from `firstJob` on hold of the barrier form's ordering: fetch macro calls, and the fetch scratch words
// an indirect command buffer's execute of a fetching state sets
size_t orderingWords(size_t firstJob)
{
    std::vector<uint32_t> words;
    for (size_t j = firstJob; j < kernel_model::model.jobs.size(); j++)
        words.insert(words.end(), kernel_model::model.jobs[j].words.begin(), kernel_model::model.jobs[j].words.end());
    std::vector<switch_gm20b_method> methods(words.size());
    const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    EXPECT_GE(count, 0);
    const uint32_t call = NVB197_CALL_MME_MACRO(0);
    size_t n = 0;
    for (long i = 0; i < count; i++) {
        const switch_gm20b_method &m = methods[i];
        if (m.subchannel)
            continue;
        const bool macro =
            m.method >= call && (m.method - call) % 8 == 0 &&
            ((m.method - call) / 8 == gm20b::mme::kFetchDraw || (m.method - call) / 8 == gm20b::mme::kFetchDrawIndexed);
        const bool scratch = m.method == NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_FETCH_BEGIN) ||
                             m.method == NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_FETCH_ICB);
        n += macro || scratch;
    }
    return n;
}

class DeviceFetchPatches : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Buffer factors, arguments, slots;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, factors));
        ASSERT_TRUE(dev.createBuffer(256, arguments));
        ASSERT_TRUE(dev.createBuffer(gm20b::icb::kPatchSlotWords * 4, slots, true));
        // MTLDrawPatchIndirectArguments: patchCount, instanceCount, patchStart, baseInstance
        const uint32_t draw[4] = {5, 1, 0, 0};
        memcpy(arguments.cpu, draw, sizeof draw);
        gm20b::icb::PatchDraw d{};
        const char *why = nullptr;
        ASSERT_TRUE(gm20b::RenderEncoder::icbPatchDraw(
            dev, {0, 2, 1, 0, &factors, 32, 12, 1.0f, nullptr, 0, nullptr, 0}, d, &why))
            << (why ? why : "");
        gm20b::icb::patches(reinterpret_cast<uint32_t *>(slots.cpu), 0, d);
    }

    gm20b::Pipeline *pipeline(bool storage)
    {
        std::string why;
        gm20b::Pipeline *p = dev.addRecord(fetchingPatches(storage ? 0x7e51 : 0x7eb1, storage), false, why);
        EXPECT_NE(p, nullptr) << why;
        return p;
    }

    struct Drawn {
        bool ok = false;
        uint32_t draws = 0;
        uint64_t interlocked = 0, refusedAsPatches = 0;
        size_t jobs = 0, fetchWords = 0;
        gm20b::Refusal refusal;
        std::string why;
    };

    // One draw of `pl` into the target in a pass of its own, committed where it was drawn
    Drawn drawOnce(gm20b::Pipeline *pl, const std::function<bool(gm20b::RenderEncoder &, const char **)> &draw)
    {
        Drawn d;
        const gm20b::FetchCounts &k = dev.stats.fetch;
        const uint64_t interlocked = k.interlocked, refused = k.refusedAtEncode[size_t(gm20b::FetchRefusal::Geometry)];
        const size_t jobs = kernel_model::model.jobs.size();
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return d;
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = gm20b::ColorAttachment::Load;
        pass.count = 1;
        pass.storageMask = pl->storageBits;
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            d.why = "no pass";
            cb->abandon();
            return d;
        }
        e->setPipeline(pl);
        const char *why = nullptr;
        d.ok = draw(*e, &why);
        d.why = why ? why : "";
        d.refusal = cb->refusal;
        e->end();
        d.draws = cb->draws;
        if (d.ok) {
            uint32_t seqno = 0;
            EXPECT_TRUE(cb->commit(seqno));
        } else {
            cb->abandon();
        }
        d.interlocked = k.interlocked - interlocked;
        d.refusedAsPatches = k.refusedAtEncode[size_t(gm20b::FetchRefusal::Geometry)] - refused;
        d.jobs = kernel_model::model.jobs.size() - jobs;
        d.fetchWords = orderingWords(jobs);
        return d;
    }

    struct Shape {
        const char *name;
        std::function<bool(gm20b::RenderEncoder &, const char **)> draw;
        uint32_t patches, instances;  // the refusal's b and c: 0 where the GPU holds them
    };

    std::vector<Shape> shapes()
    {
        const auto direct = [](uint32_t patches, uint32_t instances) {
            return [patches, instances](gm20b::RenderEncoder &e, const char **why) {
                return e.drawPatches(4, 0, patches, instances, 0, nullptr, 0, nullptr, 0, why);
            };
        };
        return {
            {"one patch", direct(1, 1), 1, 1},
            {"three patches", direct(3, 1), 3, 1},
            {"seven patches", direct(7, 1), 7, 1},
            {"one patch, two instances", direct(1, 2), 1, 2},
            {"indirect",
             [this](gm20b::RenderEncoder &e, const char **why) {
                 return e.drawPatchesIndirect(4, nullptr, 0, nullptr, 0, &arguments, 0, why);
             },
             0, 0},
            {"patch commands",
             [this](gm20b::RenderEncoder &e, const char **why) {
                 e.setTessellationFactors(&factors, 0, 0);
                 return e.executeSlots(
                     {&slots, 0, 1, gm20b::icb::kPatchSlotWords, nullptr, 0, kPatch, false, false, nullptr, 0}, why);
             },
             0, 0},
        };
    }
};

TEST(FetchPatches, APatchIsNeverOnePrimitiveOfAFetchingDraw)
{
    using gm20b::FetchRefusal;
    for (const uint32_t patches : {1u, 2u, 3u, 7u}) {
        EXPECT_FALSE(gm20b::singlePrimitive({kPatch, patches, 1})) << patches;
        EXPECT_EQ(gm20b::fetchRefusal(true, false, {kPatch, patches, 1}), FetchRefusal::Geometry) << patches;
    }
    EXPECT_EQ(gm20b::fetchRefusal(true, false, {kPatch, 1, 2}), FetchRefusal::Geometry);
    EXPECT_EQ(gm20b::fetchRefusal(true, false, {kPatch, 0, 1, true}), FetchRefusal::Geometry);
    EXPECT_TRUE(gm20b::singlePrimitive({kPatch, 0, 1})) << "no patch draws nothing";
    EXPECT_EQ(gm20b::fetchRefusal(false, false, {kPatch, 7, 2}), FetchRefusal::None);
    EXPECT_EQ(gm20b::fetchRefusal(true, false, {3, 3, 1}), FetchRefusal::None) << "a triangle is still one primitive";
}

TEST_F(DeviceFetchPatches, EveryPatchDrawIsTheStorageFormsWholeOrRefused)
{
    gm20b::Pipeline *storage = pipeline(true), *barrier = pipeline(false);
    ASSERT_NE(storage, nullptr);
    ASSERT_NE(barrier, nullptr);
    ASSERT_EQ(storage->storageBits, 1);
    ASSERT_EQ(barrier->storageBits, 0);
    ASSERT_EQ(barrier->fetchBits & barrier->writeBits & 1, 1);
    for (const Shape &s : shapes()) {
        const Drawn whole = drawOnce(storage, s.draw);
        EXPECT_TRUE(whole.ok) << s.name << ": " << whole.why;
        EXPECT_EQ(whole.draws, 1u) << s.name;
        EXPECT_EQ(whole.interlocked, 1u) << s.name;
        EXPECT_GE(whole.jobs, 1u) << s.name;
        EXPECT_EQ(whole.fetchWords, 0u) << s.name << ": the interlock orders it, no barrier a primitive";

        const Drawn refused = drawOnce(barrier, s.draw);
        EXPECT_FALSE(refused.ok) << s.name;
        EXPECT_EQ(refused.why, gm20b::fetchRefusalWhy(gm20b::FetchRefusal::Geometry)) << s.name;
        EXPECT_EQ(refused.refusal.id, gm20b::RefGeometry) << s.name;
        EXPECT_EQ(refused.refusal.a, kPatch) << s.name;
        EXPECT_EQ(refused.refusal.b, s.patches) << s.name;
        EXPECT_EQ(refused.refusal.c, s.instances) << s.name;
        EXPECT_EQ(refused.refusedAsPatches, 1u) << s.name;
        EXPECT_EQ(refused.jobs, 0u) << s.name << ": nothing of a refused draw is submitted";
    }
}

}  // namespace
