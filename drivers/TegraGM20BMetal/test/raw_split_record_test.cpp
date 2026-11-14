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

// A split prologue runs ahead of all of its command buffer's 3D work, so a draw keeps its whole program only where work
// before it in the command buffer wrote the uniform memory the prologue reads: a fill of another buffer leaves the twin
TEST_F(SplitRecords, OnlyAWriteOfTheBoundUniformsKeepsTheWholeProgram)
{
    std::string why;
    raw::RuntimeRecord record = shapeRecord(0x618b45175101cd21ull);
    record.splitRecord.uboCount = 1;  // its prologue reads the fragment program's uniform buffer, constant buffer 1
    record.splitRecord.uboCbuf[0] = 1;
    raw::Pipeline *pl = dev.addRecord(record, false, why);
    ASSERT_NE(pl, nullptr) << why;
    raw::Buffer other;
    ASSERT_TRUE(dev.createBuffer(0x4000, other));
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->forceSplit(true);
    const auto drawOnce = [&](uint64_t offset) {
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = raw::ColorAttachment::Clear;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        e->setViewport(0, 0, 256, 256, 0, 1);
        e->setScissor(0, 0, 256, 256);
        e->setPipeline(pl);
        e->setBuffer(2, 1, &uniforms, offset);
        const char *refused = nullptr;
        ASSERT_TRUE(e->draw(kStrip, 0, 4, 1, 0, &refused)) << (refused ? refused : "?");
        e->end();
    };
    raw::BlitEncoder *b = cb->blit();
    b->fillBytes(other.va, 0x1000, 0x5a);
    b->end();
    drawOnce(0x1040);
    EXPECT_EQ(cb->splitRecords().size(), 1u) << "another buffer's fill";
    b = cb->blit();
    b->fillBytes(uniforms.va + 0x1000, 0x100, 0x5a);
    b->end();
    drawOnce(0x10c8);
    EXPECT_EQ(cb->splitRecords().size(), 1u) << "a fill of the bound uniforms' memory";
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
}

// A state whose fragment root table holds [[buffer(1)]] (m2v's Psb slot, as Core Animation's states reach its uniform
// ring), its program `stores` or not (an STG in every instruction slot)
raw::RuntimeRecord rootSlotRecord(uint64_t key, bool stores)
{
    raw::RuntimeRecord r;
    r.keyString = stores ? "samples=1 c0=115 ds=0/0 slot stores " : "samples=1 c0=115 ds=0/0 slot ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {raw::CbufRoot, raw::CbufSet};
    }
    r.programs[0].id = "root-vs";
    r.programs[1].id = stores ? "root-fs-stores" : "root-fs";
    if (stores)
        for (size_t at = 80; at + 8 <= r.programs[1].image.size(); at += 8)
            r.programs[1].image[at + 7] = 0xee, r.programs[1].image[at + 6] = 0xd8;
    r.pipeline.keyHash = key;
    r.pipeline.rootTableWord[1] = 17;
    r.pipeline.statusWord[1] = 19;
    r.pipeline.rootSlots[1] = 1u << 1;
    r.pipeline.pushMask = 0xF << 16;
    r.blob = {0x1a00, 0x1111};
    return r;
}

// Core Animation's frame: draws whose root tables reach its uniform ring, then the glass state's at a later offset of the
// same ring. A program that stores nothing writes none of the ring, so the twin's prologue reads it as the draw would: the
// glass state takes its twin in every frame; after a draw of a program that stores, it draws whole
TEST_F(SplitRecords, TheGlassStateTakesItsTwinAfterDrawsReachingTheRingThroughTheirRootTables)
{
    std::string why;
    raw::RuntimeRecord glassRecord = shapeRecord(0x4c33d27bde9b894aull);
    glassRecord.splitRecord.uboCount = 1;
    glassRecord.splitRecord.uboCbuf[0] = 1;
    raw::Pipeline *glass = dev.addRecord(glassRecord, false, why);
    ASSERT_NE(glass, nullptr) << why;
    raw::Pipeline *reader = dev.addRecord(rootSlotRecord(0xca01, false), false, why);
    ASSERT_NE(reader, nullptr) << why;
    raw::Pipeline *writer = dev.addRecord(rootSlotRecord(0xca02, true), false, why);
    ASSERT_NE(writer, nullptr) << why;
    EXPECT_FALSE(reader->writesMemory[1]);
    EXPECT_TRUE(writer->writesMemory[1]);
    const auto frame = [&](raw::Pipeline *before, uint32_t frame) {
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        cb->forceSplit(true);
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.color[0].load = raw::ColorAttachment::Clear;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        e->setViewport(0, 0, 256, 256, 0, 1);
        e->setScissor(0, 0, 256, 256);
        const char *refused = nullptr;
        e->setPipeline(before);
        for (uint64_t d = 0; d < 4; d++) {
            e->setBuffer(2, 1, &uniforms, 0x100 * d + 0x400 * frame);
            EXPECT_TRUE(e->draw(kStrip, 0, 4, 1, 0, &refused)) << (refused ? refused : "?");
        }
        e->setPipeline(glass);
        e->setBuffer(2, 1, &uniforms, 0x2000 + 0x40 * frame);
        EXPECT_TRUE(e->draw(kStrip, 0, 4, 1, 0, &refused)) << (refused ? refused : "?");
        e->end();
        const size_t twins = cb->splitRecords().size();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return twins;
    };
    for (uint32_t f = 0; f < 3; f++)
        EXPECT_EQ(frame(reader, f), 1u) << "frame " << f;
    EXPECT_EQ(frame(writer, 3), 0u) << "a program that stores may write the ring";
}
