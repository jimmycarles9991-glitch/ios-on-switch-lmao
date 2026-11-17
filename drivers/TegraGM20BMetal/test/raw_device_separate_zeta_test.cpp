// SPDX-License-Identifier: MIT
// A pass whose depth and stencil attachments are two textures (a Depth32Float and a Stencil8) on raw::Device: it renders a
// Depth32Float_Stencil8 backing of theirs, the copy engine moving each loaded aspect from its texture into the backing
// before the draws and each stored one back after them, and nothing of an aspect neither loaded nor stored. Read off the
// device's submitted words: the zeta target's address and format, and each copy's source and destination in order around
// the draws, and the depth bias's units where the backing stands in for a Depth16Unorm, and of infinite or NaN factors.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_depth_bias.h"
#include "../raw/runtime/raw_depth_stencil_state.h"
#include "../raw/runtime/raw_record.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kDepth32Float = 252, kStencil8 = 253, kTriangle = 3, kZF32X24S8 = 0x19;
constexpr uint32_t kZtA = 0x0fe0, kZtFormat = 0x0fe8, kMacroCalls = 0x3800, kDepthBiasControl = 0x1110,
                   kDepthBias = 0x15bc, kSlopeScaleDepthBias = 0x156c, kDepthBiasClamp = 0x187c,
                   kClipMinZ = NVB197_SET_VIEWPORT_CLIP_MIN_Z(0), kClipMaxZ = NVB197_SET_VIEWPORT_CLIP_MAX_Z(0);
constexpr uint32_t kCeLaunch = 0x0300, kCeInUpper = 0x0400, kCeInLower = 0x0404, kCeOutUpper = 0x0408,
                   kCeOutLower = 0x040c;

// What a job did, in order: copies (source, destination) and draws (macro calls)
struct Event {
    bool draw;
    uint64_t from, to;
};

struct Submitted {
    std::vector<Event> events;
    // SET_DEPTH_BIAS_CONTROL, SET_DEPTH_BIAS, SET_SLOPE_SCALE_DEPTH_BIAS and SET_DEPTH_BIAS_CLAMP writes (method,
    // value) and draws (kMacroCalls, 0), in order
    std::vector<std::pair<uint32_t, uint32_t>> bias;
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
        else if (m == kDepthBiasControl || m == kDepthBias || m == kSlopeScaleDepthBias || m == kDepthBiasClamp ||
                 m == kClipMinZ || m == kClipMaxZ)
            out.bias.push_back({m, v});
        else if (m >= kMacroCalls) {
            out.events.push_back({true, 0, 0});
            out.bias.push_back({kMacroCalls, 0});
        }
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
    raw::Pipeline *pl = nullptr, *other = nullptr;
    uint32_t slice = 0;  // the pair's layers drawn from this slice of each texture on (raw::layersFrom)

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
        r.pipeline.keyHash = 0x5ea;
        r.blob = {0x1a00, 0x2222};
        other = dev.addRecord(r, false, why);
        ASSERT_NE(other, nullptr) << why;
    }

    // One pass of a depth- and stencil-tested triangle over the two textures: what it submitted, or why not ("" drawn)
    std::string draw(bool loadDepth, bool loadStencil, bool storeDepth, bool storeStencil, Submitted &out,
                     uint32_t layers = 1, uint8_t biasDepthBits = 0, float bias = 0, float slope = 0, float clamp = 0)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        const size_t first = kernel_model::model.jobs.size();
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::Texture depthPart = raw::layersFrom(depth, slice), stencilPart = raw::layersFrom(stencil, slice);
        raw::Texture *depthFrom = slice ? &depthPart : &depth, *stencilFrom = slice ? &stencilPart : &stencil;
        pass.depthStencil.texture = depthFrom;
        pass.depthStencil.clearDepth = !loadDepth;
        pass.depthStencil.clearStencil = !loadStencil;
        pass.separate = {depthFrom, stencilFrom, loadDepth, loadStencil, storeDepth, storeStencil};
        pass.layers = layers;
        pass.biasDepthBits = biasDepthBits;
        pass.metalDepth = true;
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
        e->setDepthBias(bias, slope, clamp);
        const char *why = nullptr;
        bool ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        if (ok && bias) {
            e->setPipeline(other);
            ok = e->draw(kTriangle, 0, 3, 1, 0, &why);
        }
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

// A layered pass of a Depth32Float array beside a Stencil8 array moves every layer of each loaded and stored aspect
TEST_F(DeviceSeparateZeta, ALayeredPairMovesEveryLayer)
{
    dev.destroyTexture(target);
    dev.destroyTexture(depth);
    dev.destroyTexture(stencil);
    ASSERT_TRUE(dev.createArray(64, 64, 2, kRGBA8, raw::fx::kTexTwoDArray, target));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depth, 2));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kStencil8, 1, false, stencil, 2));
    Submitted s;
    ASSERT_EQ(draw(true, true, true, true, s, 2), "");
    for (const raw::Texture *t : {&depth, &stencil})
        for (uint64_t layer = 0; layer < 2; layer++) {
            const uint64_t va = t->va + layer * t->layerBytes;
            EXPECT_TRUE(std::any_of(s.events.begin(), s.events.end(),
                                    [&](const Event &e) {
                                        return e.from == va;
                                    }))
                << "layer " << layer << " read";
            EXPECT_TRUE(std::any_of(s.events.begin(), s.events.end(),
                                    [&](const Event &e) {
                                        return e.to == va;
                                    }))
                << "layer " << layer << " written";
        }
}

// A pair from a slice past 0 of each array moves the layers from there and leaves the ones before it
TEST_F(DeviceSeparateZeta, APairFromASliceMovesItsLayersAlone)
{
    dev.destroyTexture(target);
    dev.destroyTexture(depth);
    dev.destroyTexture(stencil);
    ASSERT_TRUE(dev.createArray(64, 64, 2, kRGBA8, raw::fx::kTexTwoDArray, target));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kDepth32Float, 1, false, depth, 3));
    ASSERT_TRUE(dev.createOwnedZeta(64, 64, kStencil8, 1, false, stencil, 3));
    slice = 1;
    Submitted s;
    ASSERT_EQ(draw(true, true, true, true, s, 2), "");
    for (const raw::Texture *t : {&depth, &stencil})
        for (uint64_t layer = 0; layer < 3; layer++) {
            const uint64_t va = t->va + layer * t->layerBytes;
            const bool moved = layer >= slice;
            EXPECT_EQ(std::any_of(s.events.begin(), s.events.end(),
                                  [&](const Event &e) {
                                      return e.from == va;
                                  }),
                      moved)
                << "layer " << layer << " read";
            EXPECT_EQ(std::any_of(s.events.begin(), s.events.end(),
                                  [&](const Event &e) {
                                      return e.to == va;
                                  }),
                      moved)
                << "layer " << layer << " written";
        }
}

TEST_F(DeviceSeparateZeta, SelectedMipPartsKeepTheChainsArrayStrideAndCopyOrdering)
{
    dev.destroyTexture(target);
    dev.destroyTexture(depth);
    dev.destroyTexture(stencil);
    ASSERT_TRUE(dev.createArray(50, 30, 2, kRGBA8, raw::fx::kTexTwoDArray, target));
    ASSERT_TRUE(dev.createOwnedZeta(100, 60, kDepth32Float, 1, false, depth, 4, raw::fx::kTexTwoDArray, 4));
    ASSERT_TRUE(dev.createOwnedZeta(200, 120, kStencil8, 1, false, stencil, 4, raw::fx::kTexTwoDArray, 5));
    const raw::Texture depthOwner = depth, stencilOwner = stencil;
    ASSERT_TRUE(raw::chainLevelImage(depthOwner, 1, depth));
    ASSERT_TRUE(raw::chainLevelImage(stencilOwner, 2, stencil));
    slice = 1;
    Submitted submitted;
    const std::string why = draw(true, true, true, true, submitted, 2);
    depth = depthOwner;
    stencil = stencilOwner;
    ASSERT_EQ(why, "");
    const auto firstDraw = std::find_if(submitted.events.begin(), submitted.events.end(), [](const Event &event) {
        return event.draw;
    });
    ASSERT_NE(firstDraw, submitted.events.end());
    for (const auto &image : {std::pair{&depth, 1u}, std::pair{&stencil, 2u}}) {
        const auto *owner = image.first;
        const auto level = image.second;
        for (uint32_t layer = 0; layer < owner->layers; ++layer) {
            const uint64_t address = owner->chain->va + owner->chain->layout.level[level].offset +
                                     uint64_t(layer) * owner->chain->layout.arrayStride;
            const bool selected = layer == 1 || layer == 2;
            EXPECT_EQ(std::any_of(submitted.events.begin(), firstDraw,
                                  [&](const Event &event) {
                                      return !event.draw && event.from == address;
                                  }),
                      selected);
            EXPECT_EQ(std::any_of(firstDraw, submitted.events.end(),
                                  [&](const Event &event) {
                                      return !event.draw && event.to == address;
                                  }),
                      selected);
            EXPECT_FALSE(std::any_of(submitted.events.begin(), submitted.events.end(), [&](const Event &event) {
                return !event.draw && (event.from == owner->va || event.to == owner->va);
            }));
        }
    }
}

// A depth bias in a pass whose zeta stands in for a Depth16Unorm counts 16-bit codes, as Metal's does: the float zeta's
// constant 2^-24 units (SET_DEPTH_BIAS_CONTROL's FORCE_UNORM) times 2^24 / 65535, written again after each state's
// registers, which may hold the control; a pass of its own zeta counts that format's units
TEST_F(DeviceSeparateZeta, ADepth16StandInCountsItsBiasInCodes)
{
    for (const uint8_t bits : {uint8_t(0), uint8_t(16)}) {
        Submitted s;
        ASSERT_EQ(draw(true, true, true, true, s, 1, bits, 100.0f), "");
        const float constant = bits ? float(100.0 * 16777216.0 / 65535.0) : 100.0f;
        uint32_t constantBits;
        memcpy(&constantBits, &constant, 4);
        uint32_t controls = 0;
        bool biased = false, drawn = false;
        for (const auto &[m, v] : s.bias) {
            if (m == kDepthBiasControl) {
                EXPECT_EQ(v, bits ? 0u : 1u) << int(bits);
                controls++;
            } else if (m == kDepthBias && v) {  // 0: the pass's end putting the baseline back
                EXPECT_EQ(v, constantBits) << int(bits);
                biased = true;
            } else if (m == kMacroCalls && biased && !drawn) {
                drawn = true;
                EXPECT_GT(controls, 0u) << "the control before the first biased draw " << int(bits);
            }
        }
        EXPECT_TRUE(biased) << int(bits);
        EXPECT_EQ(controls, 2u) << "the control again after the second state's registers " << int(bits);
    }
}

// Infinite or NaN factors are finite ones in FORCE_UNORM units on every zeta (raw_depth_bias.h): a constant of 1 beside
// a slope of -inf leaves a flat triangle NaN (bounded by the clamp of 0.125) and a sloped one -inf, so a saturating
// constant, a slope that outweighs it and the clamp, through the bias-unit state the second state's registers make it
// write again; the float zeta's pixel clamp is Metal's 0 to 1 (its clip control leaves the clamp to the range)
TEST_F(DeviceSeparateZeta, AnInfiniteSlopeIsASaturatingBiasInForcedUnormUnits)
{
    const auto bits = [](float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        return u;
    };
    for (const uint8_t depthBits : {uint8_t(0), uint8_t(16)}) {
        Submitted s;
        ASSERT_EQ(draw(true, true, true, true, s, 1, depthBits, 1.0f, -INFINITY, 0.125f), "");
        uint32_t controls = 0, constants = 0, minZ = 0, maxZ = 0;
        bool drawn = false;
        for (const auto &[m, v] : s.bias) {
            if (m == kMacroCalls && !drawn) {
                drawn = true;
                EXPECT_EQ(minZ, bits(0.0f)) << "the pixel clamp from 0 before the draw " << int(depthBits);
                EXPECT_EQ(maxZ, bits(1.0f)) << "to 1 " << int(depthBits);
            } else if (m == kClipMinZ) {
                minZ = v;
            } else if (m == kClipMaxZ) {
                maxZ = v;
            } else if (m == kDepthBiasControl) {
                EXPECT_EQ(v, 0u) << int(depthBits);
                controls++;
            } else if (m == kDepthBias && v) {
                EXPECT_EQ(v, bits(raw::kSaturatingBias)) << int(depthBits);
                constants++;
            } else if (m == kSlopeScaleDepthBias && v) {
                EXPECT_EQ(v, bits(-raw::kSlopeOutweighs)) << int(depthBits);
            } else if (m == kDepthBiasClamp && v) {
                EXPECT_EQ(v, bits(0.125f)) << int(depthBits);
            }
        }
        EXPECT_EQ(constants, 1u) << int(depthBits);
        EXPECT_EQ(controls, 2u) << int(depthBits);
    }
}

}  // namespace
