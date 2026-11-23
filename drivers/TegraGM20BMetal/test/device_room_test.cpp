// SPDX-License-Identifier: MIT
// What a runtime-made state does on gm20b::Device (device_harness.h) where the heap or local memory had no room for one of
// its programs: its turned variant is placed at its first turned draw, and a turned draw that finds no room is refused
// alone, the next asking again; a split twin that found no room is asked again by the next draws that take it.
#include "device_harness.h"
#include "../pack.h"
#include "../record.h"
#include "../rotated_pass.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kRGBA16Float = 115, kTriangle = 3, kStrip = 4;

class DeviceRoom : public harness::DeviceTest {
protected:
    gm20b::Texture target;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    }

    static gm20b::CapturedProgram program(uint32_t id, uint32_t localBytes = 0)
    {
        gm20b::CapturedProgram p;
        p.image.assign(80 + 64, 0);
        p.image[0] = uint8_t(id);
        memcpy(p.image.data() + 4, &localBytes, sizeof localBytes);  // SPH SHADER_LOCAL_MEMORY_LOW_SIZE
        p.id = "program " + std::to_string(id);
        p.gprs = 8;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
        return p;
    }

    // A state whose fragment program has a turned variant that asks for shader local memory
    gm20b::Pipeline *turnedState()
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 ";
        r.programs[0] = program(1);
        r.programs[1] = program(2);
        r.pipeline.keyHash = 0xae;
        r.pipeline.variantCount[1] = 1;
        r.variants.push_back({gm20b::AssumeTurned, program(3, 0x400)});
        r.blob = {0x1a00, 0x1111};
        std::string why;
        gm20b::Pipeline *made = dev.addRecord(r, false, why);
        EXPECT_NE(made, nullptr) << why;
        return made;
    }

    // One draw into the target, turned: its plane rotated a quarter as Metal's rotation 1
    std::string draw(gm20b::Pipeline *state, bool turned)
    {
        target.rotation = turned ? 1 : 0;
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(state);
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

TEST_F(DeviceRoom, ATurnedVariantWithNoRoomAtItsStatesCreationIsPlacedAtItsFirstTurnedDraw)
{
    kernel_model::model.refuseAllocs = true;
    gm20b::Pipeline *pl = turnedState();
    kernel_model::model.refuseAllocs = false;
    ASSERT_NE(pl, nullptr);
    EXPECT_FALSE(pl->turnRefused);
    EXPECT_TRUE(pl->turnedVariant);
    EXPECT_EQ(draw(pl, false), "");
    EXPECT_EQ(pl->turnedTwin, nullptr) << "an upright draw places no turned variant";
    EXPECT_EQ(draw(pl, true), "");
    ASSERT_NE(pl->turnedTwin, nullptr);
    EXPECT_EQ(draw(pl, true), "");
}

TEST_F(DeviceRoom, ATurnedDrawThatFindsNoRoomIsRefusedAloneAndTheNextAsksAgain)
{
    gm20b::Pipeline *pl = turnedState();
    ASSERT_NE(pl, nullptr);
    ASSERT_EQ(draw(pl, false), "");
    kernel_model::model.refuseAllocs = true;
    EXPECT_EQ(draw(pl, true), gm20b::rotatedRefusalText(gm20b::RotatedRefusal::Variant));
    kernel_model::model.refuseAllocs = false;
    EXPECT_EQ(draw(pl, false), "") << "upright draws go on";
    EXPECT_EQ(draw(pl, true), "");
    EXPECT_NE(pl->turnedTwin, nullptr);
}

// Every turned draw that finds no room is refused alone and places the variant afresh: no count, no latch
TEST_F(DeviceRoom, EveryTurnedDrawThatFindsNoRoomPlacesTheVariantAfresh)
{
    gm20b::Pipeline *pl = turnedState();
    ASSERT_NE(pl, nullptr);
    ASSERT_EQ(draw(pl, false), "");
    kernel_model::model.refuseAllocs = true;
    const uint64_t full = dev.stats.heapFull;
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(draw(pl, true), gm20b::rotatedRefusalText(gm20b::RotatedRefusal::Variant)) << i;
    EXPECT_EQ(dev.stats.heapFull - full, 6u);
    EXPECT_FALSE(pl->turnRefused);
    kernel_model::model.refuseAllocs = false;
    EXPECT_EQ(draw(pl, true), "");
    EXPECT_NE(pl->turnedTwin, nullptr);
}

// A glass shape state (split_record_test.cpp's) whose split per-pixel program asks for shader local memory: its twin
// finds no room where the kernel has none to give. `split` false: the same state with no split
gm20b::RuntimeRecord splitRecord(bool split = true)
{
    const uint8_t ubo = uint8_t(gm20b::CbufUbo | (32 / 16) << 3);
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=115 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    r.programs[1].cbufTypes = {gm20b::CbufRoot, ubo};
    gm20b::PackPipeline &pp = r.pipeline;
    pp.keyHash = split ? 0xb0 : 0xb1;
    gm20b::setEntryCount(pp, 1);
    pp.entry[0] = {32, gm20b::EntryUbo, 2, 1, uint8_t(17 << 2), 0, 0};
    pp.vertexSlot = 0xff;
    pp.pushMask = 1u << 16;
    r.blob = {0x1a00, 0};
    r.splitTried = r.split = split;
    gm20b::PackSplit &s = r.splitRecord;
    s.frontierBytes = 64;
    s.frontierCbuf = 2;
    s.prologueCbufs = 1;
    s.batchVersion = 1;
    s.batchThreads = 32;
    s.qmd[18] = 32u << 16;
    s.qmd[19] = 0x10001;
    r.splitPrograms[0] = r.programs[1];
    r.splitPrograms[0].id = "fs-split";
    r.splitPrograms[0].cbufTypes = {gm20b::CbufRoot, ubo, gm20b::CbufFrontier};
    const uint32_t localBytes = 0x400;
    memcpy(r.splitPrograms[0].image.data() + 4, &localBytes, sizeof localBytes);
    r.splitPrograms[1].id = "prologue";
    r.splitPrograms[1].image.assign(80 + 64, 0);
    return r;
}

class DeviceSplitRoom : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    gm20b::Buffer uniforms;
    gm20b::Pipeline *pl = nullptr, *plain = nullptr;

    // the device's command buffer memory made by a draw of a state with no split, before any refusal
    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(256, 256, kRGBA16Float, target));
        ASSERT_TRUE(dev.createBuffer(0x4000, uniforms));
        std::string why;
        pl = dev.addRecord(splitRecord(), false, why);
        ASSERT_NE(pl, nullptr) << why;
        plain = dev.addRecord(splitRecord(false), false, why);
        ASSERT_NE(plain, nullptr) << why;
        ASSERT_EQ(draw(false, plain), "");
        ASSERT_EQ(pl->split, nullptr);
    }

    // One draw of `state` (the split one) over the whole target; `split`: its twin taken whatever the pass's plan says
    std::string draw(bool split, gm20b::Pipeline *state = nullptr)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        cb->forceSplit(split);
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = gm20b::ColorAttachment::Clear;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setViewport(0, 0, 256, 256, 0, 1);
        e->setScissor(0, 0, 256, 256);
        e->setPipeline(state ? state : pl);
        e->setBuffer(2, 1, &uniforms, 0x1040);
        const char *why = nullptr;
        const bool ok = e->draw(kStrip, 0, 4, 1, 0, &why);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        return "";
    }
};

TEST_F(DeviceSplitRoom, ATwinThatFoundNoRoomIsMadeByALaterDrawThatTakesIt)
{
    kernel_model::model.refuseAllocs = true;
    EXPECT_EQ(draw(true), "") << "the draw draws whole";
    EXPECT_EQ(pl->split, nullptr);
    kernel_model::model.refuseAllocs = false;
    ASSERT_NE(pl->splitSource, nullptr) << "a twin with no room keeps its source";
    EXPECT_EQ(draw(true), "");
    EXPECT_NE(pl->split, nullptr);
}

// Each draw that takes a twin with no room makes it afresh: no count, no latch; its slots placed once whatever the draws
TEST_F(DeviceSplitRoom, EveryDrawThatTakesATwinWithNoRoomMakesItAfresh)
{
    kernel_model::model.refuseAllocs = true;
    const uint64_t full = dev.stats.heapFull, placed = dev.stats.programsPlaced;
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(draw(true), "") << i;
    kernel_model::model.refuseAllocs = false;
    EXPECT_EQ(dev.stats.heapFull - full, 6u);
    EXPECT_EQ(dev.stats.splitRoomRefusals.load(), 6u);
    EXPECT_EQ(dev.stats.programsPlaced - placed, 2u) << "its slots are placed once whatever the draws";
    EXPECT_NE(pl->splitSource, nullptr);
    EXPECT_EQ(draw(true), "");
    EXPECT_NE(pl->split, nullptr);
}

}  // namespace
