// SPDX-License-Identifier: MIT
// What a runtime-made state does on raw::Device (raw_device_harness.h) where the heap or local memory had no room for one of
// its programs: a split twin that found no room is asked again by the next draws that take it.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA16Float = 115, kStrip = 4;

// A glass shape state (raw_split_record_test.cpp's) whose split per-pixel program asks for shader local memory: its twin
// finds no room where the kernel has none to give. `split` false: the same state with no split
raw::RuntimeRecord splitRecord(bool split = true)
{
    const uint8_t ubo = uint8_t(raw::CbufUbo | (32 / 16) << 3);
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=115 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.programs[0].id = "vs";
    r.programs[1].id = "fs";
    r.programs[1].cbufTypes = {raw::CbufRoot, ubo};
    raw::PackPipeline &pp = r.pipeline;
    pp.keyHash = split ? 0xb0 : 0xb1;
    pp.entries = 1;
    pp.entry[0] = {32, raw::EntryUbo, 2, 1, uint8_t(17 << 2), 0, 0};
    pp.vertexSlot = 0xff;
    pp.pushMask = 1u << 16;
    r.blob = {0x1a00, 0};
    r.splitTried = r.split = split;
    raw::PackSplit &s = r.splitRecord;
    s.frontierBytes = 64;
    s.frontierCbuf = 2;
    s.prologueCbufs = 1;
    s.batchVersion = 1;
    s.batchThreads = 32;
    s.qmd[18] = 32u << 16;
    s.qmd[19] = 0x10001;
    r.splitPrograms[0] = r.programs[1];
    r.splitPrograms[0].id = "fs-split";
    r.splitPrograms[0].cbufTypes = {raw::CbufRoot, ubo, raw::CbufFrontier};
    const uint32_t localBytes = 0x400;
    memcpy(r.splitPrograms[0].image.data() + 4, &localBytes, sizeof localBytes);
    r.splitPrograms[1].id = "prologue";
    r.splitPrograms[1].image.assign(80 + 64, 0);
    return r;
}

class DeviceSplitRoom : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer uniforms;
    raw::Pipeline *pl = nullptr, *plain = nullptr;

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
    std::string draw(bool split, raw::Pipeline *state = nullptr)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        cb->forceSplit(split);
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = raw::ColorAttachment::Clear;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
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
