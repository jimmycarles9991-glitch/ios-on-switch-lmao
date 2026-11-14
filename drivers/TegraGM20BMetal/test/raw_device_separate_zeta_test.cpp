// SPDX-License-Identifier: MIT
// A pass whose depth and stencil attachments are two textures (a Depth32Float and a Stencil8) on raw::Device: it renders a
// Depth32Float_Stencil8 backing of theirs, the copy engine moving each loaded aspect from its texture into the backing
// before the draws and each stored one back after them, and nothing of an aspect neither loaded nor stored. Read off the
// device's submitted words: the zeta target's address and format, and each copy's source and destination in order around
// the draws.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_record.h"
#include <algorithm>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kDepth32Float = 252, kStencil8 = 253, kTriangle = 3, kZF32X24S8 = 0x19;
constexpr uint32_t kZtA = 0x0fe0, kZtFormat = 0x0fe8, kMacroCalls = 0x3800;
constexpr uint32_t kCeLaunch = 0x0300, kCeInUpper = 0x0400, kCeInLower = 0x0404, kCeOutUpper = 0x0408,
                   kCeOutLower = 0x040c;

// What a job did, in order: copies (source, destination) and draws (macro calls)
struct Event {
    bool draw;
    uint64_t from, to;
};

struct Submitted {
    std::vector<Event> events;
    uint64_t zeta = 0;
    uint32_t zetaFormat = 0;
};

Submitted read(size_t first)
{
    Submitted out;
    uint64_t in = 0, outVa = 0, zt = 0;
    auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == raw::SUBC_COPY) {
            if (m == kCeInUpper)
                in = (in & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kCeInLower)
                in = (in & ~0xffffffffull) | v;
            else if (m == kCeOutUpper)
                outVa = (outVa & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kCeOutLower)
                outVa = (outVa & ~0xffffffffull) | v;
            else if (m == kCeLaunch && in && outVa)
                out.events.push_back({false, in, outVa});
            return;
        }
        if (subc != raw::SUBC_3D)
            return;
        if (m == kZtA)
            zt = uint64_t(v) << 32;
        else if (m == kZtA + 4)
            out.zeta = zt | v;
        else if (m == kZtFormat)
            out.zetaFormat = v;
        else if (m >= kMacroCalls)
            out.events.push_back({true, 0, 0});
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

class DeviceSeparateZeta : public harness::DeviceTest {
protected:
    raw::Texture target, depth, stencil;
    raw::Pipeline *pl = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depth));
        ASSERT_TRUE(dev.createOwnedZeta(64, 64, kStencil8, 1, false, stencil));
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=252/253 ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = 0x5e9;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        pl = dev.addRecord(r, false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    // One pass of a depth- and stencil-tested triangle over the two textures: what it submitted, or why not ("" drawn)
    std::string draw(bool loadDepth, bool loadStencil, bool storeDepth, bool storeStencil, Submitted &out)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        const size_t first = kernel_model::model.jobs.size();
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        pass.depthStencil.texture = &depth;
        pass.depthStencil.clearDepth = !loadDepth;
        pass.depthStencil.clearStencil = !loadStencil;
        pass.separate = {&depth, &stencil, loadDepth, loadStencil, storeDepth, storeStencil};
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        raw::DepthStencilDesc d;
        d.depthCompare = 1;
        d.depthWrite = true;
        d.front.compare = d.back.compare = 7;
        d.front.pass = d.back.pass = 2;
        const raw::DepthStencilState state(d);
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
        out = read(first);
        return "";
    }

    bool touches(const Event &e, const raw::Texture &t) const
    {
        return !e.draw && (e.from == t.va || e.to == t.va);
    }
};

TEST_F(DeviceSeparateZeta, ThePassRendersACombinedBackingOfItsOwn)
{
    Submitted s;
    ASSERT_EQ(draw(true, true, true, true, s), "");
    EXPECT_EQ(s.zetaFormat, kZF32X24S8) << "Depth32Float_Stencil8's ZF32_X24S8";
    EXPECT_NE(s.zeta, 0u);
    EXPECT_NE(s.zeta, depth.va) << "the depth texture is no Depth32Float_Stencil8";
    EXPECT_NE(s.zeta, stencil.va);
}

TEST_F(DeviceSeparateZeta, LoadedAspectsAreCopiedInBeforeTheDrawAndStoredOnesOutAfter)
{
    Submitted s;
    ASSERT_EQ(draw(true, true, true, true, s), "");
    const auto drawAt = std::find_if(s.events.begin(), s.events.end(), [](const Event &e) {
        return e.draw;
    });
    ASSERT_NE(drawAt, s.events.end()) << "a draw";
    const auto before = [&](auto pred) {
        return std::any_of(s.events.begin(), drawAt, pred);
    };
    const auto after = [&](auto pred) {
        return std::any_of(drawAt, s.events.end(), pred);
    };
    EXPECT_TRUE(before([&](const Event &e) {
        return !e.draw && e.from == depth.va && e.to == s.zeta;
    })) << "depth into the backing before the draw";
    EXPECT_TRUE(before([&](const Event &e) {
        return !e.draw && e.from == stencil.va;
    })) << "stencil read before the draw";
    EXPECT_TRUE(after([&](const Event &e) {
        return !e.draw && e.from == s.zeta && e.to == depth.va;
    })) << "depth back after the draw";
    EXPECT_TRUE(after([&](const Event &e) {
        return !e.draw && e.to == stencil.va;
    })) << "stencil back after the draw";
    EXPECT_FALSE(after([&](const Event &e) {
        return !e.draw && e.from == depth.va;
    }));
    EXPECT_FALSE(before([&](const Event &e) {
        return !e.draw && e.to == depth.va;
    }));
}

TEST_F(DeviceSeparateZeta, AnAspectNeitherLoadedNorStoredIsLeftAsItIs)
{
    Submitted s;
    ASSERT_EQ(draw(false, true, false, true, s), "");
    for (const Event &e : s.events)
        EXPECT_FALSE(touches(e, depth)) << "a copy of the depth texture neither loaded nor stored";
    EXPECT_TRUE(std::any_of(s.events.begin(), s.events.end(), [&](const Event &e) {
        return touches(e, stencil);
    }));
    ASSERT_EQ(draw(false, false, false, false, s), "");
    for (const Event &e : s.events)
        EXPECT_FALSE(touches(e, depth) || touches(e, stencil));
}

}  // namespace
