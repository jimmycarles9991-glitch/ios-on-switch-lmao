// SPDX-License-Identifier: MIT
// A state whose stages spill push fields past their push windows into m2v's argument tables (raw_spill_table.h), on
// raw::Device (raw_device_harness.h): each stage's table holds its record's words with the bound offsets' residuals and the
// fragment table's dynamic words, and is uploaded again only where one of them moved, never per draw.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_compute_record.h"
#include "../raw/runtime/raw_record.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kTableWords = 8, kVertexWords = 4;
constexpr uint32_t kConstant = 0xc0ffee;

// A uniform entry of `stage` at set-0 byte `offset` whose residual lies in record word `word` (32 and on: its stage's table)
raw::PackEntry uniform(uint16_t offset, uint8_t stage, uint8_t slot, int word)
{
    return {offset, raw::EntryUbo, stage, slot, uint8_t((word + 1) << 2), 0, 0};
}

class DeviceSpill : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer buffer;
    raw::Pipeline *pl = nullptr;
    raw::CommandBuffer *cb = nullptr;
    raw::RenderEncoder *e = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, buffer));
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=70 ds=0/0 spill ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].cbufTypes = {raw::CbufRoot, raw::CbufSet, uint8_t(raw::CbufUbo | (64 / 16) << 3)};
        r.programs[1].cbufTypes = {raw::CbufRoot, raw::CbufSet, uint8_t(raw::CbufUbo | (32 / 16) << 3)};
        r.pipeline.keyHash = 0x5b111;
        r.pipeline.flags = raw::PipelinePushMetadata;
        r.pipeline.entries = 6;
        r.pipeline.entry[0] = uniform(0, 2, 13, 32);
        r.pipeline.entry[1] = uniform(16, 2, 14, 33);
        r.pipeline.entry[2] = {32, raw::EntrySpillTable, 2, 0, 0, 0, 0};
        r.pipeline.entry[3] = uniform(48, 2, 1, 4);
        r.pipeline.entry[4] = uniform(80, 1, 20, 32);
        r.pipeline.entry[5] = {64, raw::EntrySpillTable, 1, 0, 0, 0, 0};
        r.pipeline.setBytes = 96;
        r.pipeline.pushMask = 1u << 4;
        // the fragment table's words 4-7: the blend colour as the encoder set it (HalfBlendColor)
        r.pipeline.halfBlendColorWord = 32 + 4 + 1;
        r.pipeline.spillEntry[0] = 6;
        r.pipeline.spillEntry[1] = 3;
        r.pipeline.spillWords[0] = kVertexWords;
        r.pipeline.spillWords[1] = kTableWords;
        r.spill = {0, 0xaa, 0, 0, 0, 0, kConstant, 0, 1, 2, 3, 4};
        r.blob = {0x1a00, 0x1111};
        std::string why;
        pl = dev.addRecord(r, false, why);
        ASSERT_NE(pl, nullptr) << why;
    }

    void begin()
    {
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        e->setPipeline(pl);
        e->setBuffer(2, 13, &buffer, 20);
        e->setBuffer(2, 14, &buffer, 36);
        e->setBuffer(2, 1, &buffer, 300);
        e->setBuffer(1, 20, &buffer, 24);
    }

    std::string draw()
    {
        const char *why = nullptr;
        return e->draw(kTriangle, 0, 3, 1, 0, &why) ? "" : why ? why : "refused";
    }

    void end()
    {
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
    }

    uint64_t table(uint32_t k) const
    {
        return raw::DeviceHarness::spillTable(*e, k);
    }

    std::vector<uint32_t> words(uint32_t k) const
    {
        const uint32_t count = k ? kTableWords : kVertexWords;
        const uint8_t *at = raw::DeviceHarness::upload(dev, table(k), 4 * count);
        if (!at)
            return {};
        std::vector<uint32_t> out(count);
        memcpy(out.data(), at, 4 * count);
        return out;
    }
};

TEST_F(DeviceSpill, EachStagesTableHoldsItsRecordsWordsAndItsSpilledResiduals)
{
    ASSERT_TRUE(buffer.va % 256 == 0) << std::hex << buffer.va;
    begin();
    ASSERT_EQ(draw(), "");
    ASSERT_NE(table(0), 0u);
    ASSERT_NE(table(1), 0u);
    EXPECT_NE(table(0), table(1));
    EXPECT_EQ(table(1) % 256, 0u);
    EXPECT_EQ(words(0), (std::vector<uint32_t>{24, 0xaa, 0, 0}));
    EXPECT_EQ(words(1), (std::vector<uint32_t>{20, 36, kConstant, 0, 0, 0, 0, 0}));
    EXPECT_EQ(raw::DeviceHarness::rootPush(*cb, 4), 300u % 256);
    end();
}

TEST_F(DeviceSpill, DrawsThatMoveNothingUploadNoTable)
{
    begin();
    ASSERT_EQ(draw(), "");
    const uint64_t vertex = table(0), fragment = table(1);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(draw(), "");
        EXPECT_EQ(table(0), vertex);
        EXPECT_EQ(table(1), fragment);
    }
    end();
}

TEST_F(DeviceSpill, AMovedOffsetUploadsOnlyItsStagesTableAgain)
{
    begin();
    ASSERT_EQ(draw(), "");
    const uint64_t vertex = table(0), fragment = table(1);
    e->setBufferOffset(2, 13, 52);
    ASSERT_EQ(draw(), "");
    EXPECT_EQ(table(0), vertex);
    EXPECT_NE(table(1), fragment);
    EXPECT_EQ(words(1)[0], 52u);
    EXPECT_EQ(words(1)[1], 36u);
    e->setBufferOffset(1, 20, 40);
    ASSERT_EQ(draw(), "");
    EXPECT_NE(table(0), vertex);
    EXPECT_EQ(words(0)[0], 40u);
    end();
}

TEST_F(DeviceSpill, TheBlendColourMovesTheFragmentTablesDynamicWords)
{
    begin();
    ASSERT_EQ(draw(), "");
    const uint64_t fragment = table(1);
    const float rgba[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    e->setBlendColor(rgba);
    ASSERT_EQ(draw(), "");
    EXPECT_NE(table(1), fragment);
    uint32_t bits[4];
    memcpy(bits, rgba, sizeof bits);
    const std::vector<uint32_t> got = words(1);
    ASSERT_EQ(got.size(), kTableWords);
    EXPECT_EQ((std::vector<uint32_t>(got.begin() + 4, got.end())), (std::vector<uint32_t>(bits, bits + 4)));
    EXPECT_EQ(got[2], kConstant);
    end();
}

// A root residual word that held a slot's residual takes 0 when the slot is bound on its base alignment again
TEST_F(DeviceSpill, AnAlignedRebindClearsTheResidualAWordHeld)
{
    begin();
    ASSERT_EQ(draw(), "");
    EXPECT_EQ(raw::DeviceHarness::rootPush(*cb, 4), 44u);
    e->setBufferOffset(2, 1, 512);
    ASSERT_EQ(draw(), "");
    EXPECT_EQ(raw::DeviceHarness::rootPush(*cb, 4), 0u);
    e->setBufferOffset(2, 13, 256);
    ASSERT_EQ(draw(), "");
    EXPECT_EQ(words(1)[0], 0u);
    end();
}

// A kernel's table: its record's words with the residuals of its spilled bind offsets, made with its set 0
TEST_F(DeviceSpill, AKernelsTableHoldsItsSpilledResiduals)
{
    raw::ComputeRecord r;
    r.code.assign(64, 0);
    r.keyString = "spill kernel";
    r.kernel.keyHash = 0x5b112;
    r.kernel.entries = 2;
    r.kernel.entry[0] = {0, raw::KernelBuffer, 3, (32u + 1) << 24};
    r.kernel.entry[1] = {16, raw::KernelSpillTable, 0, 0};
    r.kernel.setBytes = 32;
    r.kernel.spillEntry = 2;
    r.kernel.spillWords = 4;
    r.spill = {0, 7, 8, 9};
    r.cbufs = 2;
    r.cbufType[0] = raw::CbufRoot;
    r.cbufType[1] = raw::CbufSet;
    const raw::ComputeState *k = raw::placeCompute(dev, r);
    ASSERT_NE(k, nullptr);
    ASSERT_NE(k->spill, nullptr);
    raw::CommandBuffer *c = dev.begin();
    ASSERT_NE(c, nullptr);
    raw::ComputeEncoder enc(*c, dev);
    enc.setState(k);
    enc.setBuffer(3, &buffer, 4);
    const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
    const char *why = nullptr;
    ASSERT_TRUE(enc.dispatch(groups, threads, &why)) << (why ? why : "");
    const uint32_t *set = raw::DeviceHarness::kernelSet(enc);
    ASSERT_NE(set, nullptr);
    const uint64_t va = uint64_t(set[5]) << 32 | set[4];
    EXPECT_EQ(set[6], 256u);
    const uint8_t *at = raw::DeviceHarness::upload(dev, va, 16);
    ASSERT_NE(at, nullptr);
    uint32_t got[4];
    memcpy(got, at, sizeof got);
    EXPECT_EQ((std::vector<uint32_t>(got, got + 4)), (std::vector<uint32_t>{4, 7, 8, 9}));
    enc.end();
    uint32_t seqno = 0;
    EXPECT_TRUE(c->commit(seqno));
}

}  // namespace
