// SPDX-License-Identifier: MIT
// A fragment function's [[stencil]] output on gm20b::Device (m2v's ShadowStencilUse): GM20B's fragment program header has
// colour, sample mask and depth outputs only, so the record's program tests and writes the stencil on the R8Uint stand-in
// inside the interlock, the copy engine filling the stand-in from the stencil before the draws and draining it after them.
// Read off the submitted words: the hardware's stencil test is off for those draws, its depth test is the state's, a depth
// stand-in of an earlier shader-tested draw is drained before them, the ShadowStencilState words are the state's faces
// with the attached bit, a draw whose stencil write could depend on the depth test is refused, and in a pass without a
// stencil the record binds no stand-in and its words say nothing is attached.
#include "device_harness.h"
#include "../depth_stencil_state.h"
#include "../record.h"
#include "../shader_test_push.h"
#include "../shader_tests.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BTexture.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h"
#include <algorithm>
#include <map>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kDepth32Float = 252, kDepth32FloatStencil8 = 260, kTriangle = 3;
constexpr uint32_t kR8Uint = 13, kR32Uint = 98;  // VkFormat
constexpr uint32_t kMacroCalls = 0x3800;
constexpr uint32_t kCeLaunch = 0x0300, kCeInUpper = 0x0400, kCeInLower = 0x0404, kCeOutUpper = 0x0408,
                   kCeOutLower = 0x040c;
constexpr uint32_t kShadowWord = 5;  // push words 4 and 5

struct Event {
    bool draw;
    uint64_t from, to;
    std::map<uint32_t, uint32_t> state;  // a draw's 3D methods as the channel holds them at it
};

std::vector<Event> read(size_t first)
{
    std::vector<Event> out;
    std::map<uint32_t, uint32_t> state;
    uint64_t in = 0, outVa = 0;
    auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == gm20b::SUBC_COPY) {
            if (m == kCeInUpper)
                in = (in & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kCeInLower)
                in = (in & ~0xffffffffull) | v;
            else if (m == kCeOutUpper)
                outVa = (outVa & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kCeOutLower)
                outVa = (outVa & ~0xffffffffull) | v;
            else if (m == kCeLaunch && in && outVa)
                out.push_back({false, in, outVa, {}});
            return;
        }
        if (subc != gm20b::SUBC_3D)
            return;
        if (m >= kMacroCalls) {
            const uint32_t macro = (m - kMacroCalls) / 8;
            if (!(m & 4) && macro >= SWITCH_GM20B_MACRO_DRAWS_FIRST && macro <= SWITCH_GM20B_MACRO_DRAWS_LAST)
                out.push_back({true, 0, 0, state});
        } else
            state[m] = v;
    };
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        for (const uint32_t w : jobs[j].words) {
            if (left) {
                method(subc, m, w);
                left--;
                if (op == 1 || op == 5)
                    m += 4;
                if (op == 5)
                    op = 3;
                continue;
            }
            if (!w)
                continue;
            op = w >> 29;
            subc = w >> 13 & 7;
            m = (w & 0xfff) << 2;
            if (op == 4)
                method(subc, m, w >> 16 & 0x1fff);
            else
                left = w >> 16 & 0x1fff;
        }
    }
    return out;
}

uint32_t storageView(uint32_t vkFormat)
{
    const uint32_t identity[4] = {};
    uint32_t word0 = 0;
    return switch_gm20b_view_word0(vkFormat, identity, true, &word0) ? word0 : 0;
}

// Less with depth writes; front LessEqual, Zero on failure, Replace on pass; back NotEqual, IncrementWrap, DecrementWrap.
// `dependent`: the front face's depth failure Keep, so its stencil write follows the depth test
gm20b::DepthStencilDesc shadowState(bool dependent)
{
    gm20b::DepthStencilDesc d;
    d.depthCompare = 1;
    d.depthWrite = true;
    d.front = {3, 1, uint8_t(dependent ? 0 : 2), 2, 0x3c, 0x0f};
    d.back = {5, 6, 7, 7, 0xff, 0xf0};
    return d;
}

class DeviceShadowStencil : public harness::DeviceTest {
protected:
    gm20b::Texture target, zeta, depthOnly;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32FloatStencil8, 1, false, zeta));
        ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depthOnly));
    }

    // A shader-side stencil's record (its word at kShadowWord, its stand-in bound where `attached`), or with `tested` a
    // storage form's whose program runs the depth and stencil tests on its stand-ins
    gm20b::Pipeline *record(uint64_t hash, bool attached, bool tested = false)
    {
        gm20b::RuntimeRecord r;
        r.keyString = attached || tested ? "samples=1 c0=70 ds=260/260 " : "samples=1 c0=70 ds=252/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        gm20b::PackPipeline &pp = r.pipeline;
        pp.keyHash = hash;
        pp.flags = gm20b::PipelinePushMetadata;
        pp.setBytes = 16;
        if (tested) {
            pp.flags |= gm20b::PipelineStorageMetadata;
            gm20b::setEntryCount(pp, 3);
            pp.entry[0] = {0, gm20b::EntryDepthTest, 2, 0, 0, 0, 0};
            pp.entry[1] = {4, gm20b::EntryStencilTest, 2, 0, 0, 0, 0};
            pp.entry[2] = {8, gm20b::EntryFetch, 2, 0, 0, 0, 0};
            pp.shaderTests = {
                1, 9, gm20b::kShaderTestDepth | gm20b::kShaderTestStencil, 0, storageView(100), storageView(kR8Uint),
                0};
            pp.storageMask = pp.storageWriteMask = 1;
            pp.storageSamples = 1;
            pp.storageTests = 4;
            pp.storageTic[0] = storageView(kR32Uint);
            pp.pushMask = 0xfff;
            r.blob = {0x1224, 2, 0x1a00, 0};
        } else {
            gm20b::setEntryCount(pp, 1);
            pp.entry[0] = {0, gm20b::EntryStencilTest, 2, 0, 0, 0, 0};
            pp.shaderTests = {0,
                              0,
                              uint8_t(attached ? gm20b::kShaderTestStencil : 0),
                              uint8_t(kShadowWord),
                              0,
                              attached ? storageView(kR8Uint) : 0u,
                              0};
            pp.pushMask = 0x30;
            r.blob = {0x1224, 2, 0x1a00, 0x1111};
        }
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // The shader-side stencil's " | tests=4" variant: no storage attachment, its program running the depth and stencil
    // tests on both stand-ins for StencilWrite draws
    gm20b::Pipeline *variant(uint64_t hash)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=260/260 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        gm20b::PackPipeline &pp = r.pipeline;
        pp.keyHash = hash;
        pp.flags = gm20b::PipelinePushMetadata;
        pp.setBytes = 16;
        gm20b::setEntryCount(pp, 2);
        pp.entry[0] = {0, gm20b::EntryDepthTest, 2, 0, 0, 0, 0};
        pp.entry[1] = {4, gm20b::EntryStencilTest, 2, 0, 0, 0, 0};
        pp.shaderTests = {
            1, 9, gm20b::kShaderTestDepth | gm20b::kShaderTestStencil, 0, storageView(100), storageView(kR8Uint), 0};
        pp.storageTests = 4;
        pp.pushMask = 0xfff;
        r.blob = {0x1224, 2, 0x1a00, 0x1111};
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One pass over `z` drawing a triangle with each of `records` under `state`: "" drawn, else why not; `out` what was
    // submitted, `words` the root's shadow words after it
    std::string draw(gm20b::Texture &z, std::initializer_list<gm20b::Pipeline *> records,
                     const gm20b::DepthStencilDesc &d, std::vector<Event> &out, uint32_t words[2] = nullptr,
                     gm20b::Refusal *refusal = nullptr)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        const size_t first = kernel_model::model.jobs.size();
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.depthStencil.texture = &z;
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        const gm20b::DepthStencilState state(d);
        e->setDepthStencil(&state);
        const char *why = nullptr;
        for (gm20b::Pipeline *pl : records) {
            e->setPipeline(pl);
            if (!e->draw(kTriangle, 0, 3, 1, 0, &why)) {
                if (refusal)
                    *refusal = cb->refusal;
                cb->abandon();
                return why ? why : "refused";
            }
        }
        if (words) {
            words[0] = gm20b::DeviceHarness::rootPush(*cb, kShadowWord - 1);
            words[1] = gm20b::DeviceHarness::rootPush(*cb, kShadowWord);
        }
        e->end();
        uint32_t seqno = 0;
        if (!cb->commit(seqno)) {
            cb->abandon();
            return "not committed";
        }
        out = read(first);
        return "";
    }
};

TEST_F(DeviceShadowStencil, TheHardwareTestsDepthAsTheStateSaysAndNoStencil)
{
    gm20b::Pipeline *pl = record(0x5d1, true);
    ASSERT_NE(pl, nullptr);
    std::vector<Event> events;
    ASSERT_EQ(draw(zeta, {pl}, shadowState(false), events), "");
    const auto at = std::find_if(events.begin(), events.end(), [](const Event &e) {
        return e.draw;
    });
    ASSERT_NE(at, events.end());
    const auto held = [&](uint32_t m) {
        const auto v = at->state.find(m);
        return v == at->state.end() ? 0u : v->second;
    };
    EXPECT_EQ(held(NVB197_SET_DEPTH_TEST), 1u);
    EXPECT_EQ(held(NVB197_SET_DEPTH_WRITE), 1u) << "the state's depth writes";
    EXPECT_EQ(held(NVB197_SET_DEPTH_FUNC), gm20b::depthStencilCompare(1)) << "the state's Less";
    EXPECT_EQ(held(NVB197_SET_STENCIL_TEST), 0u) << "the program is the stencil test";
}

TEST_F(DeviceShadowStencil, TheStandInIsFilledBeforeTheDrawsAndDrainedAfterThem)
{
    gm20b::Pipeline *pl = record(0x5d2, true);
    ASSERT_NE(pl, nullptr);
    std::vector<Event> events;
    ASSERT_EQ(draw(zeta, {pl, pl}, shadowState(false), events), "");
    std::vector<size_t> draws;
    for (size_t k = 0; k < events.size(); k++)
        if (events[k].draw)
            draws.push_back(k);
    ASSERT_EQ(draws.size(), 2u);
    const auto copies = [&](size_t from, size_t to, bool intoZeta) {
        return std::count_if(events.begin() + from, events.begin() + to, [&](const Event &e) {
            return !e.draw && (intoZeta ? e.to == zeta.va : e.from == zeta.va);
        });
    };
    EXPECT_GE(copies(0, draws[0], false), 1) << "the stencil into the stand-in before the first draw";
    EXPECT_EQ(copies(draws[0], draws[1], false) + copies(draws[0], draws[1], true), 0)
        << "consecutive shadow draws share one fill and one drain";
    EXPECT_GE(copies(draws[1], events.size(), true), 1) << "the stand-in back into the stencil after the draws";
}

TEST_F(DeviceShadowStencil, ItsWordsAreTheStateFacesWithTheAttachedBit)
{
    gm20b::Pipeline *pl = record(0x5d3, true);
    ASSERT_NE(pl, nullptr);
    std::vector<Event> events;
    uint32_t words[2] = {};
    const gm20b::DepthStencilDesc d = shadowState(false);
    ASSERT_EQ(draw(zeta, {pl}, d, events, words), "");
    EXPECT_EQ(words[0], 3u | 1u << 3 | 2u << 6 | 0x3cu << 9 | 0x0fu << 17 | 1u << 31);
    EXPECT_EQ(words[1], 5u | 6u << 3 | 7u << 6 | 0xffu << 9 | 0xf0u << 17 | 1u << 31);
}

TEST_F(DeviceShadowStencil, ADepthStandInIsDrainedBeforeAShadowDraw)
{
    gm20b::Pipeline *tested = record(0x5d4, false, true);
    gm20b::Pipeline *shadow = record(0x5d5, true);
    ASSERT_NE(tested, nullptr);
    ASSERT_NE(shadow, nullptr);
    std::vector<Event> events;
    ASSERT_EQ(draw(zeta, {tested, shadow}, shadowState(false), events), "");
    std::vector<size_t> draws;
    for (size_t k = 0; k < events.size(); k++)
        if (events[k].draw)
            draws.push_back(k);
    ASSERT_EQ(draws.size(), 2u);
    EXPECT_TRUE(std::any_of(events.begin() + draws[0], events.begin() + draws[1], [&](const Event &e) {
        return !e.draw && e.to == zeta.va;
    })) << "the depth stand-in back into the attachment before the hardware tests depth";
    EXPECT_EQ(events[draws[1]].state.at(NVB197_SET_DEPTH_FUNC), gm20b::depthStencilCompare(1));
}

TEST_F(DeviceShadowStencil, AStencilWriteThatFollowsTheDepthTestIsRefused)
{
    gm20b::Pipeline *pl = record(0x5d6, true);
    ASSERT_NE(pl, nullptr);
    std::vector<Event> events;
    gm20b::Refusal refusal;
    EXPECT_NE(draw(zeta, {pl}, shadowState(true), events, nullptr, &refusal), "");
    EXPECT_EQ(refusal.id, gm20b::RefTestsInvariant);
    gm20b::DepthStencilDesc always = shadowState(true);
    always.depthCompare = 7;
    EXPECT_EQ(draw(zeta, {pl}, always, events), "") << "a depth test that passes every fragment decides nothing";
}

TEST_F(DeviceShadowStencil, APassWithoutStencilBindsNoStandInAndSaysSo)
{
    gm20b::Pipeline *pl = record(0x5d7, false);
    ASSERT_NE(pl, nullptr);
    std::vector<Event> events;
    uint32_t words[2] = {~0u, ~0u};
    gm20b::DepthStencilDesc d = shadowState(false);
    ASSERT_EQ(draw(depthOnly, {pl}, d, events, words), "");
    EXPECT_EQ(words[0] >> 31, 0u) << "nothing attached: the program tests nothing";
    EXPECT_EQ(words[1] >> 31, 0u);
    EXPECT_FALSE(std::any_of(events.begin(), events.end(), [&](const Event &e) {
        return !e.draw && (e.from == depthOnly.va || e.to == depthOnly.va);
    })) << "no stand-in to fill";
}

TEST_F(DeviceShadowStencil, ItsDepthDependentVariantRunsBothTestsOnTheStandIns)
{
    gm20b::Pipeline *pl = variant(0x5d8);
    ASSERT_NE(pl, nullptr);
    std::vector<Event> events;
    ASSERT_EQ(draw(zeta, {pl}, shadowState(true), events), "");
    const auto at = std::find_if(events.begin(), events.end(), [](const Event &e) {
        return e.draw;
    });
    ASSERT_NE(at, events.end());
    EXPECT_GE(std::count_if(events.begin(), at,
                            [&](const Event &e) {
                                return !e.draw && e.from == zeta.va;
                            }),
              2)
        << "depth and stencil into their stand-ins";
    const auto held = [&](uint32_t m) {
        const auto v = at->state.find(m);
        return v == at->state.end() ? 0u : v->second;
    };
    EXPECT_NE(held(NVB197_SET_DEPTH_FUNC), gm20b::depthStencilCompare(1)) << "the program is the depth test";
    EXPECT_EQ(held(NVB197_SET_DEPTH_WRITE), 0u);
    EXPECT_EQ(held(NVB197_SET_STENCIL_TEST), 0u);
    gm20b::DepthStencilDesc depthOnly;
    depthOnly.depthCompare = 1;
    depthOnly.depthWrite = true;
    gm20b::Refusal refusal;
    EXPECT_NE(draw(zeta, {pl}, depthOnly, events, nullptr, &refusal), "") << "made for StencilWrite draws alone";
    EXPECT_EQ(refusal.id, gm20b::RefTestsInvariant);
}

}  // namespace
