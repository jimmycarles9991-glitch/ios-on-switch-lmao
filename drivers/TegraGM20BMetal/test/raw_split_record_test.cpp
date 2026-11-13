// SPDX-License-Identifier: MIT
// A split draw's prologue reads that draw's own push words: each draw of one state in a command buffer, at its own
// uniform buffer offset, gets a batch record holding its own residual push word and a frontier of its own, whatever the
// draws before it bound (raw_split.cpp launchSplit; the shape states' frontier is that residual plus constants).
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA16Float = 115, kStrip = 4, kResidualWord = 16, kUboSetOffset = 32;

// The glass shape states' layout: the fragment program reads one uniform buffer, fragment slot 1, through residual push
// word 16; its split is a version-1 batch with a 64-byte frontier at constant buffer 2
raw::RuntimeRecord shapeRecord(uint64_t key)
{
    const uint8_t ubo = uint8_t(raw::CbufUbo | (kUboSetOffset / 16) << 3);
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
    pp.keyHash = key;
    pp.entries = 1;
    pp.entry[0] = {kUboSetOffset, raw::EntryUbo, 2, 1, uint8_t((kResidualWord + 1) << 2), 0, 0};
    pp.vertexSlot = 0xff;
    pp.pushMask = 1u << kResidualWord;
    r.blob = {0x1a00, 0};
    r.splitTried = r.split = true;
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
    r.splitPrograms[1].id = "prologue";
    r.splitPrograms[1].image.assign(80 + 64, 0);
    return r;
}

class SplitRecords : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer uniforms;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(256, 256, kRGBA16Float, target));
        ASSERT_TRUE(dev.createBuffer(0x4000, uniforms));
    }
};

}  // namespace

TEST_F(SplitRecords, EachDrawsPrologueReadsItsOwnResidual)
{
    std::string why;
    raw::Pipeline *pl = dev.addRecord(shapeRecord(0x618b45175101cd20ull), false, why);
    ASSERT_NE(pl, nullptr) << why;
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->forceSplit(true);
    raw::PassDesc pass;
    pass.color[0].texture = &target;
    pass.color[0].load = raw::ColorAttachment::Clear;
    pass.count = 1;
    raw::RenderEncoder *e = cb->render(pass);
    ASSERT_NE(e, nullptr);
    e->setViewport(0, 0, 256, 256, 0, 1);
    e->setScissor(0, 0, 256, 256);
    e->setPipeline(pl);
    // unaligned offsets (residuals 0x40 and 0xc8), the second's offset changed after the first draw, then an aligned one
    // (residual 0, so the word keeps the state's own push word: 0)
    const uint64_t offsets[] = {0x1040, 0x10c8, 0x1100, 0x10c8};
    for (uint64_t offset : offsets) {
        e->setBuffer(2, 1, &uniforms, offset);
        const char *refused = nullptr;
        ASSERT_TRUE(e->draw(kStrip, 0, 4, 1, 0, &refused)) << (refused ? refused : "?");
    }
    e->end();
    const std::vector<raw::SplitBatchRecord> &records = cb->splitRecords();
    // the fourth draw's inputs equal the second's: it reuses that frontier and takes no record
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].push[kResidualWord], 0x40u);
    EXPECT_EQ(records[1].push[kResidualWord], 0xc8u);
    EXPECT_EQ(records[2].push[kResidualWord], 0u);
    EXPECT_NE(records[0].frontier, records[1].frontier);
    EXPECT_NE(records[1].frontier, records[2].frontier);
    EXPECT_NE(records[0].frontier, records[2].frontier);
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
}
