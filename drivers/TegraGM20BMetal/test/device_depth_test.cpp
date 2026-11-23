// SPDX-License-Identifier: MIT
#include "device_harness.h"
#include "../depth_stencil_state.h"
#include "../record.h"
#include <tuple>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

gm20b::DepthStencilState depthState(uint8_t compare, bool write = false)
{
    gm20b::DepthStencilDesc d;
    d.depthCompare = compare;
    d.depthWrite = write;
    return gm20b::DepthStencilState(d);
}

class DeviceHiddenDepth : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = 0xd0;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        pl = dev.addRecord(r, false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    // A triangle under `state` in a pass with no depth/stencil attachment, given the hidden one or not: drawn, or why not
    std::string draw(const gm20b::DepthStencilState &state, bool hidden)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.hiddenZeta = hidden;
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(pl);
        e->setDepthStencil(&state);
        const char *why = nullptr;
        const bool ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        return "";
    }
};

TEST_F(DeviceHiddenDepth, EveryCompareButAlwaysDrawsOverTheHiddenAttachment)
{
    for (uint8_t compare = 0; compare < 7; compare++)
        EXPECT_EQ(draw(depthState(compare, compare & 1), true), "") << int(compare);
    gm20b::DepthStencilDesc stencil;
    stencil.front.compare = stencil.back.compare = 5;
    EXPECT_EQ(draw(gm20b::DepthStencilState(stencil), true), "");
}

TEST_F(DeviceHiddenDepth, AlwaysNeedsNoHiddenAttachment)
{
    EXPECT_EQ(draw(depthState(7), false), "");
    EXPECT_EQ(draw(depthState(7, true), false), "");
}

TEST_F(DeviceHiddenDepth, APassNotGivenItRefusesAStateThatTestsDepth)
{
    EXPECT_NE(draw(depthState(1), false), "");
    EXPECT_NE(draw(depthState(0), false), "");
}

constexpr uint32_t kClipControl = 0x193c, kClipMinZ = 0x0c08, kClipMaxZ = 0x0c0c, kClip = 0x181d, kClamped = 0x101a;
constexpr uint32_t kZero = 0, kOne = 0x3f800000, kMinusInf = 0xff800000, kPlusInf = 0x7f800000;

// The values the device's jobs since `first` wrote to 3D class method `method`, in order
std::vector<uint32_t> writes(size_t first, uint32_t method)
{
    std::vector<uint32_t> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++)
        for (const TegraGM20BAbiPush &push : jobs[j].pushes) {
            const auto *w = reinterpret_cast<const uint32_t *>(kernel_model::model.host(push.va));
            for (uint32_t i = 0; w && i < push.dwords;) {
                const uint32_t hdr = w[i++], op = hdr >> 29, subc = hdr >> 13 & 7, m = (hdr & 0xfff) << 2,
                               n = hdr >> 16 & 0x1fff;
                if (!hdr)
                    continue;
                if (op == 7)
                    break;
                if (op == 4) {
                    if (!subc && m == method)
                        out.push_back(n);
                    continue;
                }
                for (uint32_t k = 0; k < n && i < push.dwords; k++, i++)
                    if (!subc && m + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0) == method)
                        out.push_back(w[i]);
            }
        }
    return out;
}

class DeviceDepthClamp : public harness::DeviceTest {
protected:
    gm20b::Texture target;

    gm20b::Refusal refused;  // the last refused draw's

    gm20b::Pipeline *state(std::vector<uint32_t> blob)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = 0xd1 + blob.size();
        r.blob = std::move(blob);
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One pass of Metal's (`metal`) or the device's own of a triangle per entry of `clamps`, each under that clamp, the
    // viewport's depth range `near` to `far`, a constant depth bias of `bias`: drawn, or why not
    std::string draw(gm20b::Pipeline *pl, std::vector<bool> clamps, bool metal = true, double near = 0, double far = 1,
                     float bias = 0, float slope = 0, float biasClamp = 0)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.metalDepth = metal;
        gm20b::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e)
            return "no pass";
        e->setPipeline(pl);
        e->setViewport(0, 0, 64, 64, near, far);
        e->setDepthBias(bias, slope, biasClamp);
        const char *why = nullptr;
        bool ok = true;
        for (bool clamp : clamps) {
            e->setDepthClamp(clamp);
            ok = ok && e->draw(kTriangle, 0, 3, 1, 0, &why);
        }
        refused = cb->refusal;
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        return "";
    }

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    }
};

// MTLDepthClipModeClamp: the state's clip control at its bind, NVK's depth clamp without depth clip over it from the first
// clamped draw with the viewport's depth range to clamp to, the state's control and an unbounded range again once the
// encoder clips; nothing written while the mode holds
TEST_F(DeviceDepthClamp, AClampedDrawWritesNvksClampOverItsStatesClipControl)
{
    gm20b::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {false, true, true, false, false}), "");
    EXPECT_EQ(writes(first, kClipControl), (std::vector<uint32_t>{kClip, kClamped, kClip}));
    EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{kZero, kMinusInf}));
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf}));
}

// A later pass binds the state again from its registers and clips at Metal's 0 to 1, whatever the pass before clamped;
// each pass leaves the range unbounded for the device's own passes after it
TEST_F(DeviceDepthClamp, TheNextPassStartsFromItsStatesClipControl)
{
    gm20b::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {true}), "");
    EXPECT_EQ(draw(pl, {false}), "");
    EXPECT_EQ(writes(first, kClipControl), (std::vector<uint32_t>{kClip, kClamped, kClip}));
    EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{kZero, kMinusInf, kZero, kMinusInf}));
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf, kOne, kPlusInf}));
}

constexpr uint32_t bitsOf(float f)
{
    return __builtin_bit_cast(uint32_t, f);
}

// An M4 Max stores a Depth32Float depth that a finite bias, a viewport range past 1 or a fragment function pushes out of
// 0..1 in clip mode as 0 or 1 (depth_bounds.h): Metal's passes clip at 0 to 1 whatever the bias and range
TEST_F(DeviceDepthClamp, AMetalPassClipsEveryDepthAtZeroToOne)
{
    gm20b::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    for (const auto &[near, far, bias] : {std::tuple{0.0, 1.0, 1e7f}, std::tuple{0.25, 1.75, 0.0f}}) {
        const size_t first = kernel_model::model.jobs.size();
        EXPECT_EQ(draw(pl, {false}, true, near, far, bias), "");
        EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{kZero, kMinusInf})) << far;
        EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf})) << far;
    }
}

// In clamp mode an M4 Max bounds a depth from the viewport's lower depth but at least 0 to its higher, which an unbiased
// draw meets at 1 (each vertex clamped to 0..1 first), a fragment function's own depth at 1 at most
TEST_F(DeviceDepthClamp, AClampedMetalPassBoundsFromNearAtLeastZero)
{
    gm20b::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {true}, true, -0.5, 1.5), "");
    EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{kZero, kMinusInf}));
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf}));
    first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {true}, true, 1.75, 0.25), "");
    EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{bitsOf(0.25f), kMinusInf}));
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf}));
    first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {true}, true, 0.25, 0.75), "");
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{bitsOf(0.75f), kPlusInf}));
    pl->fragmentDepth = true;
    first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {true}, true, 0.25, 1.75), "");
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf}));
}

// An M4 Max clamps each vertex's depth to 0..1 before the bias in clamp mode, so the deepest depth it stores is 1 plus the
// most the bias adds there (its constant times 2^-23, within the bias clamp): a triangle past 1 stores that, as the range's
// top meets it. A slope factor or a negative constant leaves the top the viewport's (the most it adds depends on the
// triangle)
TEST_F(DeviceDepthClamp, AClampedMetalPassBoundsDepthAtOnePlusTheMostItsBiasAdds)
{
    gm20b::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    const float most = 1.0f + 3e6f * 0x1p-23f;
    for (const auto &[bias, slope, clamp, top] :
         {std::tuple{0.0f, 0.0f, 0.0f, kOne}, std::tuple{3e6f, 0.0f, 0.0f, bitsOf(most)},
          std::tuple{1e7f, 0.0f, 0.25f, bitsOf(1.25f)}, std::tuple{-1e7f, 0.0f, 0.0f, bitsOf(1.75f)},
          std::tuple{0.0f, 4.0f, 0.0f, bitsOf(1.75f)}, std::tuple{1e9f, 0.0f, 0.0f, bitsOf(1.75f)}}) {
        const size_t first = kernel_model::model.jobs.size();
        EXPECT_EQ(draw(pl, {true}, true, 0.25, 1.75, bias, slope, clamp), "");
        EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{bitsOf(0.25f), kMinusInf})) << bias;
        EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{top, kPlusInf})) << bias << " " << slope;
    }
}

// Holding the range costs a Metal pass two writes and no state bind: the channel keeps the state's segment across the
// passes of one command buffer, as the state's registers do not hold the range, unless they name it (then the range
// written over them makes the next pass bind it again)
TEST_F(DeviceDepthClamp, AMetalPassBoundsDepthWithoutBindingItsStateAgain)
{
    std::vector<uint32_t> blob = {0x1a00, 0x1111, kClipControl, kClip};
    for (uint32_t m = 0x1a04; m < 0x1a20; m += 4)
        blob.insert(blob.end(), {m, 0});
    gm20b::Pipeline *plain = state(blob);
    blob.insert(blob.end(), {kClipMinZ, kMinusInf, kClipMaxZ, kPlusInf});
    gm20b::Pipeline *naming = state(blob);
    ASSERT_NE(plain, nullptr);
    ASSERT_NE(naming, nullptr);
    ASSERT_TRUE(plain->segment);
    ASSERT_TRUE(naming->namesViewportClipRange);
    for (const auto &[pl, binds] : {std::pair{plain, 1u}, std::pair{naming, 2u}}) {
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        for (uint32_t pass = 0; pass < 2; pass++) {
            gm20b::PassDesc p;
            p.color[0].texture = &target;
            p.count = 1;
            p.metalDepth = true;
            gm20b::RenderEncoder *e = cb->render(p);
            ASSERT_NE(e, nullptr);
            e->setPipeline(pl);
            const char *why = nullptr;
            ASSERT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << why;
            e->end();
        }
        EXPECT_EQ(gm20b::DeviceHarness::segmentBinds(*cb), binds) << pl->namesViewportClipRange;
        uint32_t seqno = 0;
        ASSERT_TRUE(cb->commit(seqno));
    }
}

// The device's own passes copy depth as it is: no bound whatever their depth range
TEST_F(DeviceDepthClamp, TheDevicesOwnPassesStoreDepthUnbounded)
{
    gm20b::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {false}, false, 0.25, 1.75), "");
    EXPECT_EQ(writes(first, kClipMinZ), std::vector<uint32_t>{});
    EXPECT_EQ(writes(first, kClipMaxZ), std::vector<uint32_t>{});
}

TEST_F(DeviceDepthClamp, AStateThatSetsNoClipControlCannotClamp)
{
    gm20b::Pipeline *pl = state({0x1a00, 0x1111});
    ASSERT_NE(pl, nullptr);
    EXPECT_EQ(draw(pl, {false}), "");
    EXPECT_EQ(draw(pl, {true}), "a depth clamp on a state whose registers set no clip control");
    EXPECT_EQ(refused.id, gm20b::RefClipControl);
}

}  // namespace
