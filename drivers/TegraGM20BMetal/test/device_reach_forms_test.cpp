// SPDX-License-Identifier: MIT
// The reach form of a state (reach.cpp Device::reachTwin) on gm20b::Device over a pack holding the reach state
// (record_pack.h): each pass loads a 64x64 target, a texel of its reach target a 32x32 tile, and its one draw is
// scissored to tile (1, 0). Read off the words the reach pass submitted, a draw that reaches is drawn there under the reach
// state's pixel program inside that tile, with the bindings its vertex and tessellation stages read; a draw Metal
// rasterizes nothing of is not drawn.
#include "device_harness.h"
#include "reach_calls.h"
#include "record_pack.h"
#include "../pack.h"
#include "../reach.h"
#include "../record.h"
#include "../residual.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using harness::DrawCall;
using DrawBody = std::function<bool(gm20b::RenderEncoder &, const char **)>;

constexpr uint32_t kRGBA8 = 70, kTriangleStrip = 4, kControlWord = 5, kClipControl = 0x193c;
constexpr uint32_t kTileX = 2u << 16 | 1u, kTileY = 1u << 16;  // SET_SCISSOR_HORIZONTAL, VERTICAL of tile (1, 0)
constexpr const char *kKey = "samples=1 c0=70:15:0 ds=0/0 ";

gm20b::RuntimeRecord state(uint64_t key, const char *keyString = kKey)
{
    gm20b::RuntimeRecord r;
    r.keyString = keyString;
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    return r;
}

gm20b::RuntimeRecord ownState(const char *name)
{
    return state(gm20b::Pack::fnv(name, strlen(name)), "samples=1 c0=13:15:0 ds=0/0 ");
}

// A tessellation state whose control stage reads the factor buffer through set 0's first entry
gm20b::RuntimeRecord tessellation(uint64_t key)
{
    gm20b::RuntimeRecord r = state(key);
    for (gm20b::CapturedProgram &p : r.tessPrograms) {
        p.image.assign(80 + 64, 0);
        p.image[80] = uint8_t(1 + (&p - r.tessPrograms));
        p.id = "reachtess" + std::to_string(&p - r.tessPrograms);
        p.gprs = 12;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    r.pipeline.controlWord = kControlWord;
    r.pipeline.controlPoints = 4;
    gm20b::setEntryCount(r.pipeline, gm20b::TessBufferCount);
    for (uint8_t k = 0; k < gm20b::TessBufferCount; k++)
        r.pipeline.entry[k] = {uint16_t(16 * k),
                               gm20b::EntryTessBuffer,
                               1,
                               k,
                               uint8_t(1 | (kControlWord + gm20b::kTessResidualWord[k]) << 2),
                               0,
                               0};
    r.pipeline.pushMask = 0x3fu << (kControlWord - 1);
    r.extras = {{0x0320, 0x202}, {0x0dcc, 1}, {0x2080, 0x21}, {0x2090, 1}, {0x20c0, 0x31}, {0x20d0, 2}};
    r.restore = {{0x2080, 0x20}, {0x20c0, 0x30}};
    return r;
}

class OneDraw final : public gm20b::DrawReplay {
public:
    explicit OneDraw(DrawBody body) : body_(std::move(body))
    {}

    bool any() const override
    {
        return true;
    }

    bool atFrameResolution() const override
    {
        return false;
    }

    bool replay(gm20b::RenderEncoder &e, const char **why) override
    {
        return body_(e, why);
    }

private:
    DrawBody body_;
};

class DeviceReachForms : public testing::Test {
protected:
    gm20b::Device dev;
    gm20b::Texture target;
    gm20b::Buffer factors, vertices;

    void SetUp() override
    {
        kernel_model::model = {};
        std::vector<gm20b::RuntimeRecord> own = {ownState(gm20b::kReachState), ownState(gm20b::kReachReduceState)};
        std::sort(own.begin(), own.end(), [](const gm20b::RuntimeRecord &a, const gm20b::RuntimeRecord &b) {
            return a.pipeline.keyHash < b.pipeline.keyHash;
        });
        std::string err;
        ASSERT_TRUE(dev.open(harness::recordPack(own, "reach-pack.bin").c_str(), 0, err, gm20b::Device::Role::Client))
            << err;
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, factors));
        ASSERT_TRUE(dev.createBuffer(4096, vertices));
    }

    gm20b::Pipeline *made(const gm20b::RuntimeRecord &r)
    {
        std::string why;
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    uint32_t reachProgram()
    {
        DrawCall end;
        const gm20b::Pipeline *reach = dev.ownPipeline(gm20b::kReachState);
        if (reach)
            harness::drawCalls(reach->methods, &end);
        return end.pixelProgram;
    }

    struct Reached {
        std::string why;  // empty: reached
        std::vector<DrawCall> calls;
    };

    // The reach of a pass loading `target` whose one draw of `pl` `draw` encodes, scissored to tile (1, 0) as the plan's
    // state applies it (PlanDraws::replay)
    Reached reach(gm20b::Pipeline *pl, const DrawBody &draw)
    {
        Reached out;
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {"no command buffer", {}};
        const size_t first = kernel_model::model.jobs.size();
        gm20b::PassDesc p;
        p.color[0].texture = &target;
        p.color[0].load = gm20b::ColorAttachment::Load;
        p.color[0].store = true;
        p.count = 1;
        OneDraw draws([&](gm20b::RenderEncoder &e, const char **why) {
            e.setPipeline(pl);
            e.setScissor(40, 4, 20, 20);
            e.takeReachTwin();
            return draw(e, why);
        });
        gm20b::PassReach reached;
        const char *why = "";
        uint32_t seqno = 0;
        if (!cb->reach(p, p.width, p.height, draws, reached, &why) || !reached.mask) {
            cb->abandon();
            return {why && *why ? why : "no mask", {}};
        }
        if (!cb->commit(seqno)) {
            cb->abandon();
            return {"not committed", {}};
        }
        std::vector<uint32_t> words;
        for (size_t j = first; j < kernel_model::model.jobs.size(); j++)
            words.insert(words.end(), kernel_model::model.jobs[j].words.begin(),
                         kernel_model::model.jobs[j].words.end());
        out.calls = harness::drawCalls(words);
        return out;
    }

    void expectReachedInTheTile(const Reached &r)
    {
        ASSERT_EQ(r.why, "");
        ASSERT_EQ(r.calls.size(), 1u);
        EXPECT_EQ(r.calls[0].pixelProgram, reachProgram()) << "the reach state's pixel program";
        EXPECT_EQ(r.calls[0].pixelStage, gm20b::kPixelStageOn);
        EXPECT_EQ(r.calls[0].scissorX, kTileX) << std::hex << r.calls[0].scissorX;
        EXPECT_EQ(r.calls[0].scissorY, kTileY) << std::hex << r.calls[0].scissorY;
    }
};

TEST_F(DeviceReachForms, APatchDrawReachesThroughItsControlStagesBindings)
{
    gm20b::Pipeline *pl = made(tessellation(0x7e01));
    ASSERT_NE(pl, nullptr);
    std::vector<uint32_t> set;
    const Reached r = reach(pl, [&](gm20b::RenderEncoder &e, const char **why) {
        e.setTessellationFactors(&factors, 6, 13);
        if (!e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, nullptr, 0, why))
            return false;
        if (const uint8_t *words = gm20b::DeviceHarness::upload(dev, gm20b::DeviceHarness::preparedSet(e), 12))
            set.assign(reinterpret_cast<const uint32_t *>(words), reinterpret_cast<const uint32_t *>(words) + 3);
        return true;
    });
    expectReachedInTheTile(r);
    gm20b::BufferDescriptor d;
    gm20b::bufferDescriptor(true, 0, 0, factors.va + 6, uint32_t(factors.length - 6), d);
    ASSERT_EQ(set.size(), 3u) << "the reach draw's set 0 in the upload ring";
    EXPECT_EQ(set[0], uint32_t(d.base)) << "the control stage's factor buffer";
    EXPECT_EQ(set[1], uint32_t(d.base >> 32));
    EXPECT_EQ(set[2], d.range);
}

TEST_F(DeviceReachForms, ADepthClampedDrawReaches)
{
    gm20b::RuntimeRecord clipped = state(0x7e02);
    clipped.extras = {{kClipControl, 1}};
    gm20b::Pipeline *pl = made(clipped);
    ASSERT_NE(pl, nullptr);
    expectReachedInTheTile(reach(pl, [](gm20b::RenderEncoder &e, const char **why) {
        e.setDepthClamp(true);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, why);
    }));
}

TEST_F(DeviceReachForms, ADrawBoundAtAStridePastTheStreamsFieldReachesThroughItsStrideTwin)
{
    gm20b::RuntimeRecord strided = state(0x7e03), twin = state(0x7e04);
    strided.pipeline.vertexSlot = 1;
    strided.pipeline.vertexStrideDynamic = 1;
    strided.pipeline.pulledStride[0][0] = 2;
    strided.pipeline.pulledStride[0][1] = 5;
    strided.pipeline.pushMask = 1u << 4;
    twin.pipeline.vertexSlot = 0xff;
    twin.pipeline.pulledStride[0][0] = 1;
    twin.pipeline.pulledStride[0][1] = 5;
    twin.pipeline.pushMask = 1u << 4;
    strided.strideTwin = std::make_shared<gm20b::RuntimeRecord>(twin);
    gm20b::Pipeline *pl = made(strided);
    ASSERT_NE(pl, nullptr);
    ASSERT_NE(pl->strideTwin, nullptr);
    expectReachedInTheTile(reach(pl, [&](gm20b::RenderEncoder &e, const char **why) {
        e.setBuffer(1, 1, &vertices, 0);
        e.setVertexStride(1, 5000);
        return e.draw(kTriangleStrip, 0, 4, 1, 0, why);
    }));
}

TEST_F(DeviceReachForms, AStateOfNoFragmentFunctionReachesAsADiscardingOne)
{
    gm20b::RuntimeRecord depthOnly = state(0x7e05);
    depthOnly.pipeline.fs = gm20b::kNoProgram;
    gm20b::Pipeline *pl = made(depthOnly);
    ASSERT_NE(pl, nullptr);
    expectReachedInTheTile(reach(pl, [](gm20b::RenderEncoder &e, const char **why) {
        return e.draw(kTriangleStrip, 0, 4, 1, 0, why);
    }));
}

// Metal rasterizes none of such a state's draws (its vertex function returns void and may store to memory)
TEST_F(DeviceReachForms, AStateWithRasterizationOffReachesNothing)
{
    for (const bool stores : {false, true}) {
        gm20b::RuntimeRecord off = state(stores ? 0x7e07 : 0x7e06, "samples=1 c0=70:15:0 ds=0/0 rast=0 ");
        off.pipeline.fs = gm20b::kNoProgram;
        // the device places a program once by its id (programSlotKey): each image its own
        off.programs[0].id = stores ? "reach-off-stores" : "reach-off";
        if (stores)
            off.programs[0].image[2] = 1;  // SPH DOES_GLOBAL_STORE, bit 16
        gm20b::Pipeline *pl = made(off);
        ASSERT_NE(pl, nullptr);
        ASSERT_EQ(pl->vertexStores, stores);
        const Reached r = reach(pl, [](gm20b::RenderEncoder &e, const char **why) {
            return e.draw(kTriangleStrip, 0, 4, 1, 0, why);
        });
        EXPECT_EQ(r.why, "") << stores;
        EXPECT_TRUE(r.calls.empty()) << stores;
    }
}

}  // namespace
