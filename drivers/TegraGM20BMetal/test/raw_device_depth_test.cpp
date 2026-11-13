// SPDX-License-Identifier: MIT
#include "raw_device_harness.h"
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_record.h"
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3;

raw::DepthStencilState depthState(uint8_t compare, bool write = false)
{
    raw::DepthStencilDesc d;
    d.depthCompare = compare;
    d.depthWrite = write;
    return raw::DepthStencilState(d);
}

class DeviceHiddenDepth : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
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
    std::string draw(const raw::DepthStencilState &state, bool hidden)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.hiddenZeta = hidden;
        raw::RenderEncoder *e = cb->render(pass);
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
    raw::DepthStencilDesc stencil;
    stencil.front.compare = stencil.back.compare = 5;
    EXPECT_EQ(draw(raw::DepthStencilState(stencil), true), "");
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
    raw::Texture target;

    raw::Pipeline *state(std::vector<uint32_t> blob)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = 0xd1 + blob.size();
        r.blob = std::move(blob);
        std::string why;
        raw::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    // One pass of a triangle per entry of `clamps`, each under that clamp: drawn, or why not
    std::string draw(raw::Pipeline *pl, std::vector<bool> clamps)
    {
        raw::CommandBuffer *cb = dev.begin();
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb ? cb->render(pass) : nullptr;
        if (!e)
            return "no pass";
        e->setPipeline(pl);
        const char *why = nullptr;
        bool ok = true;
        for (bool clamp : clamps) {
            e->setDepthClamp(clamp);
            ok = ok && e->draw(kTriangle, 0, 3, 1, 0, &why);
        }
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
    raw::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {false, true, true, false, false}), "");
    EXPECT_EQ(writes(first, kClipControl), (std::vector<uint32_t>{kClip, kClamped, kClip}));
    EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{kZero, kMinusInf}));
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf}));
}

// A later pass binds the state again from its registers and clips with an unbounded range, whatever the pass before clamped
TEST_F(DeviceDepthClamp, TheNextPassStartsFromItsStatesClipControl)
{
    raw::Pipeline *pl = state({0x1a00, 0x1111, kClipControl, kClip});
    ASSERT_NE(pl, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ(draw(pl, {true}), "");
    EXPECT_EQ(draw(pl, {false}), "");
    EXPECT_EQ(writes(first, kClipControl), (std::vector<uint32_t>{kClip, kClamped, kClip}));
    EXPECT_EQ(writes(first, kClipMinZ), (std::vector<uint32_t>{kZero, kMinusInf}));
    EXPECT_EQ(writes(first, kClipMaxZ), (std::vector<uint32_t>{kOne, kPlusInf}));
}

TEST_F(DeviceDepthClamp, AStateThatSetsNoClipControlCannotClamp)
{
    raw::Pipeline *pl = state({0x1a00, 0x1111});
    ASSERT_NE(pl, nullptr);
    EXPECT_EQ(draw(pl, {false}), "");
    EXPECT_NE(draw(pl, {true}), "");
}

}  // namespace
