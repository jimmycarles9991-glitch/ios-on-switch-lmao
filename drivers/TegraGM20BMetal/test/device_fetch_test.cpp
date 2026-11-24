// SPDX-License-Identifier: MIT
#include "device_harness.h"
#include "../depth_stencil_state.h"
#include "../record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "../mme.h"
#include <algorithm>
#include <functional>
#include <iterator>
#include <set>

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint32_t kTriangle = 3, kStrip = 4;  // MTLPrimitiveType

// A state whose fragment program fetches colour 0 and writes it: in its storage form (inside the pixel interlock, the
// attachment a storage image) or its barrier form (fetched through the texture unit)
gm20b::RuntimeRecord fetchingRecord(uint64_t key, bool storage)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    gm20b::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    gm20b::setEntryCount(pp, 1);
    pp.entry[0] = {0, gm20b::EntryFetch, 2, 0, 0, 0, 0};
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
    return r;
}

class DeviceFetch : public harness::DeviceTest {
protected:
    gm20b::Texture target, small;
    gm20b::Texture *into = &target;
    gm20b::Buffer indices, arguments;

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

    gm20b::Pipeline *pipeline(bool storage)
    {
        std::string why;
        gm20b::Pipeline *p = dev.addRecord(fetchingRecord(storage ? 0x5701 : 0xba01, storage), false, why);
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

    template <class Draw>
    Drawn drawOnce(gm20b::Pipeline *pl, Draw draw, const gm20b::DepthStencilState *state = nullptr)
    {
        Drawn d;
        const gm20b::FetchCounts &k = dev.stats.fetch;
        const uint64_t interlocked = k.interlocked, split = k.sequential, triangles = k.sequentialTriangles;
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return d;
        gm20b::PassDesc pass;
        pass.color[0].texture = into;
        pass.color[0].load = gm20b::ColorAttachment::Load;
        pass.count = 1;
        pass.storageMask = pl->storageBits;
        pass.hiddenZeta = state != nullptr;
        gm20b::RenderEncoder *e = cb->render(pass);
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

    Drawn strip(gm20b::Pipeline *pl)
    {
        return drawOnce(pl, [](gm20b::RenderEncoder &e, const char **why) {
            return e.draw(kStrip, 0, 10, 1, 0, why);
        });
    }

    Drawn restart(gm20b::Pipeline *pl, uint8_t cull = 0)
    {
        return drawOnce(pl, [this, cull](gm20b::RenderEncoder &e, const char **why) {
            e.setCullMode(cull);
            return e.drawIndexed(kStrip, 9, 2, &indices, 0, 1, 0, 0, why);
        });
    }

    // RenderBox's quad: an indexed strip of four, under `state` over a Depth32Float_Stencil8 attachment where given
    Drawn quad(gm20b::Pipeline *pl, const gm20b::DepthStencilState *state = nullptr)
    {
        return drawOnce(
            pl,
            [this](gm20b::RenderEncoder &e, const char **why) {
                return e.drawIndexed(kStrip, 4, 2, &indices, 0, 1, 0, 0, why);
            },
            state);
    }

    Drawn instanced(gm20b::Pipeline *pl)
    {
        return drawOnce(pl, [](gm20b::RenderEncoder &e, const char **why) {
            return e.draw(kTriangle, 0, 6, 24, 0, why);
        });
    }

    // One triangle an instance, `instances` of them
    Drawn eachInstance(gm20b::Pipeline *pl, bool indexed, uint32_t instances)
    {
        return drawOnce(pl, [this, indexed, instances](gm20b::RenderEncoder &e, const char **why) {
            return indexed ? e.drawIndexed(kTriangle, 3, 2, &indices, 0, instances, 0, 0, why)
                           : e.draw(kTriangle, 0, 3, instances, 0, why);
        });
    }

    Drawn indirect(gm20b::Pipeline *pl)
    {
        return drawOnce(pl, [this](gm20b::RenderEncoder &e, const char **why) {
            return e.drawIndirect(kTriangle, &arguments, 0, why);
        });
    }

    Drawn indexedIndirect(gm20b::Pipeline *pl)
    {
        return drawOnce(pl, [this](gm20b::RenderEncoder &e, const char **why) {
            return e.drawIndexedIndirect(kStrip, 2, &indices, 0, &arguments, 16, why);
        });
    }
};

TEST_F(DeviceFetch, TheStorageFormDrawsEveryTopologyWholeInsideTheInterlock)
{
    gm20b::Pipeline *pl = pipeline(true);
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
    gm20b::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    ASSERT_EQ(pl->storageBits, 0);
    const Drawn s = strip(pl);
    EXPECT_TRUE(s.ok) << s.why;
    EXPECT_EQ(s.interlocked, 0u);
    EXPECT_EQ(s.split, 1u);
    EXPECT_EQ(s.triangles, 8u);
}

// Rule fetch.twin (twin_rule.h): a storage form's strip over a measured render area is drawn by its barrier twin a
// triangle at a time; every shape the twin's draw would refuse, and a smaller area, stay whole inside the interlock
TEST_F(DeviceFetch, TheTwinRuleTakesTheTwinOnlyWhereItsDrawKeepsEveryShape)
{
    gm20b::Pipeline *pl = pipeline(true), *twin = pipeline(false);
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
    for (const auto load :
         {gm20b::ColorAttachment::Clear, gm20b::ColorAttachment::DontCare, gm20b::ColorAttachment::Load}) {
        target.ropBlendSerial = 5;
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = load;
        pass.count = 1;
        cb->render(pass)->end();
        cb->abandon();
        EXPECT_EQ(target.ropBlendSerial, load == gm20b::ColorAttachment::Load ? 5u : 0u) << int(load);
    }
}

// An indexed strip a triangle at a time: one split draw, so the hardware's restart draws no triangle across 0xffff, as
// the whole strip draws none there
TEST_F(DeviceFetch, TheBarrierFormDrawsAnIndexedStripATriangleAtATime)
{
    gm20b::Pipeline *pl = pipeline(false);
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

// The fetch macro calls of the last job's words: each call's slot and the words it was given
std::vector<std::vector<uint32_t>> fetchCalls()
{
    std::vector<std::vector<uint32_t>> calls;
    if (kernel_model::model.jobs.empty())
        return calls;
    const std::vector<uint32_t> &words = kernel_model::model.jobs.back().words;
    std::vector<switch_gm20b_method> methods(words.size());
    const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    EXPECT_GE(count, 0);
    const uint32_t first = NVB197_CALL_MME_MACRO(0);
    for (long i = 0; i < count; i++) {
        const switch_gm20b_method &m = methods[i];
        if (m.subchannel || m.method < first || (m.method - first) % 8)
            continue;
        const uint32_t slot = (m.method - first) / 8;
        if (slot != gm20b::mme::kFetchDraw && slot != gm20b::mme::kFetchDrawIndexed)
            continue;
        std::vector<uint32_t> call = {slot, m.value};
        for (long k = i + 1; k < count && methods[k].method == m.method + 4; k++)
            call.push_back(methods[k].value);
        calls.push_back(call);
    }
    return calls;
}

// The 3D methods the last job writes, each as often as it writes it
std::multiset<uint32_t> jobMethods()
{
    const std::vector<uint32_t> &words = kernel_model::model.jobs.back().words;
    std::vector<switch_gm20b_method> methods(words.size());
    const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    EXPECT_GE(count, 0);
    std::multiset<uint32_t> out;
    for (long i = 0; i < count; i++)
        if (!methods[i].subchannel)
            out.insert(methods[i].method);
    return out;
}

// The fetch macro's scratch words the last job wrote last: the first and later BEGINs' sizes
std::pair<uint32_t, uint32_t> fetchSizes()
{
    std::pair<uint32_t, uint32_t> sizes = {0, 0};
    const std::vector<uint32_t> &words = kernel_model::model.jobs.back().words;
    std::vector<switch_gm20b_method> methods(words.size());
    const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    for (long i = 0; i < count; i++) {
        if (methods[i].method == NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_FETCH_FIRST))
            sizes.first = methods[i].value;
        if (methods[i].method == NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_FETCH_STEP))
            sizes.second = methods[i].value;
    }
    return sizes;
}

// Every shape of more than one primitive of a state that fetches the colour it writes through the texture unit: one
// fetch macro call with the draw's own words, whatever its primitive count, culled or two-sided, restarted, instanced or
// indirect (mme.h fetchDraw; the GPU draws it a primitive at a time, kits/SwitchGM20BEmit/test/emit_fetch_test.cpp)
TEST_F(DeviceFetch, TheBarrierFormDrawsEveryShapeAsOneFetchMacroCall)
{
    gm20b::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    gm20b::DepthStencilDesc winding;
    winding.depthCompare = 6;
    winding.back.pass = 6;
    const gm20b::DepthStencilState twoSided(winding);
    const uint32_t args[5] = {9, 1, 0, 0, 0};
    memcpy(static_cast<uint8_t *>(arguments.cpu) + 16, args, sizeof args);
    const uint32_t fetch = gm20b::mme::kFetchDraw, indexed = gm20b::mme::kFetchDrawIndexed;

    const struct {
        const char *name;
        std::function<Drawn()> draw;
        std::vector<uint32_t> call;
    } shapes[] = {
        {"strip",
         [&] {
             return strip(pl);
         },
         {fetch, 10, 1, 0, 0}},
        {"culled restart",
         [&] {
             return restart(pl, 1);
         },
         {indexed, 9, 1, 0, 0, 0}},
        {"two-sided quad",
         [&] {
             return quad(pl, &twoSided);
         },
         {indexed, 4, 1, 0, 0, 0}},
        {"instanced",
         [&] {
             return instanced(pl);
         },
         {fetch, 6, 24, 0, 0}},
        {"one triangle an instance",
         [&] {
             return eachInstance(pl, false, 4);
         },
         {fetch, 3, 4, 0, 0}},
        {"one indexed triangle an instance",
         [&] {
             return eachInstance(pl, true, 4);
         },
         {indexed, 3, 4, 0, 0, 0}},
        {"indirect",
         [&] {
             return indirect(pl);
         },
         {fetch, 6, 1, 0, 0}},
        {"indexed indirect",
         [&] {
             return indexedIndirect(pl);
         },
         {indexed, 9, 1, 0, 0, 0}},
    };

    for (const auto &s : shapes) {
        const Drawn d = s.draw();
        EXPECT_TRUE(d.ok) << s.name << ": " << d.why;
        EXPECT_EQ(d.split, 1u) << s.name;
        EXPECT_EQ(d.interlocked, 0u) << s.name;
        EXPECT_EQ(fetchCalls(), (std::vector<std::vector<uint32_t>>{s.call})) << s.name;
    }
}

// A long draw's words are a short one's: the CPU writes one call whatever the primitive count. The channel's first draw
// also writes what its context leaves unknown (SET_RT_LAYER, SET_ANTI_ALIAS_ALPHA_CONTROL, SET_SAMPLE_MASK_X0_Y0..X1_Y1:
// Device::newContext), so the draws compared come after one.
TEST_F(DeviceFetch, AFetchDrawsWordsDoNotGrowWithItsPrimitives)
{
    gm20b::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);
    size_t sizes[3] = {};
    std::multiset<uint32_t> methods[3];
    uint32_t k = 0;
    for (const uint32_t count : {6u, 6u, 3000u}) {
        const Drawn d = drawOnce(pl, [count](gm20b::RenderEncoder &e, const char **why) {
            return e.draw(kTriangle, 0, count, 1, 0, why);
        });
        EXPECT_TRUE(d.ok) << d.why;
        EXPECT_EQ(d.triangles, count / 3);
        sizes[k] = kernel_model::model.jobs.back().words.size();
        methods[k] = jobMethods();
        EXPECT_EQ(methods[k].count(NVB197_SET_TIR_CONTROL) + methods[k].count(NVB197_SET_TIR), 0u)
            << "a one-sample pass writes no TIR";
        k++;
        EXPECT_EQ(fetchSizes(), (std::pair<uint32_t, uint32_t>{3, 3}));
    }
    std::multiset<uint32_t> firstOnly;
    std::set_difference(methods[0].begin(), methods[0].end(), methods[1].begin(), methods[1].end(),
                        std::inserter(firstOnly, firstOnly.end()));
    EXPECT_EQ(firstOnly, (std::multiset<uint32_t>{NVB197_SET_RT_LAYER, NVB197_SET_ANTI_ALIAS_ALPHA_CONTROL,
                                                  NVB197_SET_SAMPLE_MASK_X0_Y0, NVB197_SET_SAMPLE_MASK_X1_Y0,
                                                  NVB197_SET_SAMPLE_MASK_X0_Y1, NVB197_SET_SAMPLE_MASK_X1_Y1}));
    EXPECT_TRUE(std::includes(methods[0].begin(), methods[0].end(), methods[1].begin(), methods[1].end()));
    EXPECT_EQ(sizes[1], sizes[2]);
}

// Points, lines and line strips by their own sizes: a point or two-vertex line a BEGIN, a line strip's first BEGIN two
// vertices and each later one the vertex that makes the next line
TEST_F(DeviceFetch, PointsAndLinesTakeTheirRunSizes)
{
    gm20b::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);

    const struct {
        uint32_t primitive, count;
        std::pair<uint32_t, uint32_t> sizes;
    } shapes[] = {{0, 5, {1, 1}}, {1, 6, {2, 2}}, {2, 5, {2, 1}}, {4, 5, {3, 1}}};

    for (const auto &s : shapes) {
        const Drawn d = drawOnce(pl, [&s](gm20b::RenderEncoder &e, const char **why) {
            return e.draw(s.primitive, 0, s.count, 1, 0, why);
        });
        EXPECT_TRUE(d.ok) << s.primitive << ": " << d.why;
        EXPECT_EQ(fetchSizes(), s.sizes) << s.primitive;
        EXPECT_EQ(fetchCalls(), (std::vector<std::vector<uint32_t>>{{gm20b::mme::kFetchDraw, s.count, 1, 0, 0}}))
            << s.primitive;
    }
}

// A storage slot whose record names no residual word is one its programs read through the root table's address, never
// its descriptor (every slot read through a view has its word): bound off 16 bytes, it is drawn, its descriptor's base
// aligned down
TEST_F(DeviceFetch, ASlotWithNoResidualWordBoundOffItsAlignmentIsDrawn)
{
    gm20b::RuntimeRecord r = fetchingRecord(0xb0f, false);
    r.pipeline.flags = 0;
    r.pipeline.entry[0] = {0, gm20b::EntryUbo, 2, 0, 1, 0, 0};
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, false, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_LT(gm20b::bufferResidualWord(pl->entries[0]), 0);
    const Drawn d = drawOnce(pl, [this](gm20b::RenderEncoder &e, const char **why) {
        e.setBuffer(2, 0, &indices, 8);
        return e.draw(kTriangle, 0, 3, 1, 0, why);
    });
    EXPECT_TRUE(d.ok) << d.why;
}

TEST_F(DeviceFetch, ConvexTwoTriangleQuadElidesIntraDrawBarrierAndDisjointQuadsElideInterDrawBarrier)
{
    gm20b::RuntimeRecord r = fetchingRecord(0xca01, false);
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 a0=31:0:0:16:1:1 ";
    r.pipeline.vertexSlot = 0;
    r.pipeline.vertexStride = 16;
    r.blob = {0x1a00, 0x1111, 0x1c00, (1u << 12) | 16};
    std::string why;
    gm20b::Pipeline *pl = dev.addRecord(r, true, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(pl->caTransform && pl->positionComponents == 4);

    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float quadA[16] = {-1.0f, 1.0f, 0, 1, -0.5f, 1.0f, 0, 1, -1.0f, 0.5f, 0, 1, -0.5f, 0.5f, 0, 1};
    const float quadB[16] = {0.0f, 0.0f, 0, 1, 0.5f, 0.0f, 0, 1, 0.0f, -0.5f, 0, 1, 0.5f, -0.5f, 0, 1};
    const float quadOverlap[16] = {-0.75f, 0.75f, 0, 1, -0.25f, 0.75f, 0, 1, -0.75f, 0.25f, 0, 1, -0.25f, 0.25f, 0, 1};
    const float quadList[24] = {-1.0f, 1.0f, 0, 1, -0.5f, 1.0f, 0, 1, -1.0f, 0.5f, 0, 1,
                                -0.5f, 1.0f, 0, 1, -0.5f, 0.5f, 0, 1, -1.0f, 0.5f, 0, 1};
    const float foldedStrip[16] = {-1.0f, 1.0f, 0, 1, -0.5f, 1.0f, 0, 1, -0.5f, 0.5f, 0, 1, -1.0f, 0.5f, 0, 1};
    const float overlapList[24] = {-1.0f, 1.0f, 0, 1, 0.0f,  1.0f, 0, 1, -0.5f,  0.0f, 0, 1,
                                   0.0f,  1.0f, 0, 1, -0.5f, 0.0f, 0, 1, -0.75f, 0.5f, 0, 1};

    const Drawn convexStrip = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, quadA, sizeof quadA);
        return e.draw(kStrip, 0, 4, 1, 0, w);
    });
    EXPECT_TRUE(convexStrip.ok) << convexStrip.why;
    EXPECT_EQ(convexStrip.split, 0u);
    EXPECT_TRUE(fetchCalls().empty());

    const Drawn convexList = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, quadList, sizeof quadList);
        return e.draw(kTriangle, 0, 6, 1, 0, w);
    });
    EXPECT_TRUE(convexList.ok) << convexList.why;
    EXPECT_EQ(convexList.split, 0u);
    EXPECT_TRUE(fetchCalls().empty());

    const Drawn folded = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, foldedStrip, sizeof foldedStrip);
        return e.draw(kStrip, 0, 4, 1, 0, w);
    });
    EXPECT_TRUE(folded.ok) << folded.why;
    EXPECT_EQ(folded.split, 1u);
    EXPECT_EQ(folded.triangles, 2u);

    const Drawn overlapping = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, overlapList, sizeof overlapList);
        return e.draw(kTriangle, 0, 6, 1, 0, w);
    });
    EXPECT_TRUE(overlapping.ok) << overlapping.why;
    EXPECT_EQ(overlapping.split, 1u);
    EXPECT_EQ(overlapping.triangles, 2u);

    const Drawn wireframe = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setFillMode(1);
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, quadA, sizeof quadA);
        return e.draw(kStrip, 0, 4, 1, 0, w);
    });
    EXPECT_TRUE(wireframe.ok) << wireframe.why;
    EXPECT_EQ(wireframe.split, 1u);

    const Drawn instancedQuad = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, quadA, sizeof quadA);
        return e.draw(kStrip, 0, 4, 2, 0, w);
    });
    EXPECT_TRUE(instancedQuad.ok) << instancedQuad.why;
    EXPECT_EQ(instancedQuad.split, 1u);

    const Drawn disjoint = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, quadA, sizeof quadA);
        if (!e.draw(kStrip, 0, 4, 1, 0, w))
            return false;
        e.setBytes(1, 0, quadB, sizeof quadB);
        return e.draw(kStrip, 0, 4, 1, 0, w);
    });
    EXPECT_TRUE(disjoint.ok) << disjoint.why;
    EXPECT_EQ(disjoint.split, 0u);
    EXPECT_EQ(jobMethods().count(NVB197_PIXEL_SHADER_BARRIER), 0u);

    const Drawn colliding = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, quadA, sizeof quadA);
        if (!e.draw(kStrip, 0, 4, 1, 0, w))
            return false;
        e.setBytes(1, 0, quadOverlap, sizeof quadOverlap);
        return e.draw(kStrip, 0, 4, 1, 0, w);
    });
    EXPECT_TRUE(colliding.ok) << colliding.why;
    EXPECT_EQ(colliding.split, 0u);
    EXPECT_EQ(jobMethods().count(NVB197_PIXEL_SHADER_BARRIER), 1u);

    gm20b::Texture compressed = target;
    compressed.comptags = true;
    into = &compressed;
    const Drawn compDisjoint = drawOnce(pl, [&](gm20b::RenderEncoder &e, const char **w) {
        e.setBytes(1, 2, identity, sizeof identity);
        e.setBytes(1, 0, quadA, sizeof quadA);
        if (!e.draw(kStrip, 0, 4, 1, 0, w))
            return false;
        e.setBytes(1, 0, quadB, sizeof quadB);
        return e.draw(kStrip, 0, 4, 1, 0, w);
    });
    into = &target;
    EXPECT_TRUE(compDisjoint.ok) << compDisjoint.why;
    EXPECT_EQ(compDisjoint.split, 2u);
    EXPECT_EQ(jobMethods().count(NVB197_PIXEL_SHADER_BARRIER), 1u);
}

TEST_F(DeviceFetch, FetchBarrierUsesSysmemBarOnlyForDirectVolatilePitchTargets)
{
    gm20b::Pipeline *pl = pipeline(false);
    ASSERT_NE(pl, nullptr);

    const auto barrierValues = [&]() {
        const std::vector<uint32_t> &words = kernel_model::model.jobs.back().words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        EXPECT_GE(count, 0);
        std::vector<uint32_t> out;
        for (long i = 0; i < count; i++)
            if (!methods[i].subchannel && methods[i].method == NVB197_PIXEL_SHADER_BARRIER)
                out.push_back(methods[i].value);
        return out;
    };

    const auto drawTwo = [&](gm20b::Texture &dest) {
        into = &dest;
        const Drawn d = drawOnce(pl, [](gm20b::RenderEncoder &e, const char **w) {
            return e.draw(kTriangle, 0, 3, 1, 0, w) && e.draw(kTriangle, 0, 3, 1, 0, w);
        });
        into = &target;
        EXPECT_TRUE(d.ok) << d.why;
        EXPECT_EQ(d.split, 0u);
        return barrierValues();
    };

    constexpr uint32_t kPitch = 64 * 4;
    gm20b::Buffer volBuf, cachedBuf;
    gm20b::Texture volDirect, volShadowed, cachedDirect;
    ASSERT_TRUE(dev.createBuffer(uint64_t(kPitch) * 64 + 128, volBuf, true, true, true));
    ASSERT_TRUE(dev.createBuffer(uint64_t(kPitch) * 64, cachedBuf, true, true, false));
    ASSERT_TRUE(dev.createBufferTexture(volBuf, 0, 64, 64, kPitch, kRGBA8, volDirect));
    ASSERT_TRUE(dev.createBufferTexture(volBuf, 64, 64, 64, kPitch, kRGBA8, volShadowed));
    ASSERT_TRUE(dev.createBufferTexture(cachedBuf, 0, 64, 64, kPitch, kRGBA8, cachedDirect));
    ASSERT_TRUE(volDirect.renderable && !volDirect.shadowTarget && volDirect.storageRange().gpuVolatile);
    ASSERT_TRUE(!volShadowed.renderable && volShadowed.shadowTarget && volShadowed.storageRange().gpuVolatile);
    ASSERT_TRUE(cachedDirect.renderable && !cachedDirect.shadowTarget && !cachedDirect.storageRange().gpuVolatile);

    EXPECT_EQ(drawTwo(volDirect), (std::vector<uint32_t>{uint32_t(NVB197_PIXEL_SHADER_BARRIER_SYSMEMBAR_ENABLE_TRUE)}));
    EXPECT_EQ(drawTwo(cachedDirect),
              (std::vector<uint32_t>{uint32_t(NVB197_PIXEL_SHADER_BARRIER_SYSMEMBAR_ENABLE_FALSE)}));
    EXPECT_EQ(drawTwo(volShadowed),
              (std::vector<uint32_t>{uint32_t(NVB197_PIXEL_SHADER_BARRIER_SYSMEMBAR_ENABLE_FALSE)}));
    EXPECT_EQ(drawTwo(target), (std::vector<uint32_t>{uint32_t(NVB197_PIXEL_SHADER_BARRIER_SYSMEMBAR_ENABLE_FALSE)}));

    dev.destroyTexture(cachedDirect);
    dev.destroyTexture(volShadowed);
    dev.destroyTexture(volDirect);
    dev.destroyBuffer(cachedBuf);
    dev.destroyBuffer(volBuf);
}

}  // namespace
