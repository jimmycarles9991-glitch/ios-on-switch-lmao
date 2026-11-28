// SPDX-License-Identifier: MIT
// A device-made state's full form (Device::attachFullForm) on gm20b::Device (device_harness.h): its split and program
// variants given to a placed state of its layout and its stride twin's to that state's twin, and refused with why for a
// state of any other part of the layout (record_layout.h) or none placed. Its turned variant is no choice of the variant
// table: the state's first turned draw places it, as for a state of no full form.
#include "device_harness.h"
#include "../record.h"

#include <functional>
#include <utility>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kResidualWord = 16;
constexpr uint64_t kKey = 0x5f11f0e5a11ull, kTwinKey = kKey + 1;

gm20b::CapturedProgram program(const char *id, uint8_t mark)
{
    gm20b::CapturedProgram p;
    p.image.assign(80 + 64, 0);
    p.image[80] = mark;
    p.id = id;
    p.gprs = 8;
    p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    return p;
}

// A state reading a fragment uniform buffer through residual push word kResidualWord, of `entries` set-0 entries in all
// (past kPackEntries: a far block), the rest storage buffers
gm20b::RuntimeRecord placedRecord(uint64_t key, uint32_t entries = 1)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 full " + std::to_string(key);
    r.programs[0] = program("vs", 1);
    r.programs[1] = program("fs", 2);
    gm20b::PackPipeline &pp = r.pipeline;
    pp.keyHash = key;
    pp.vertexSlot = 0xff;
    pp.setBytes = uint16_t(16 * entries);
    pp.pushMask = 1u << kResidualWord;
    for (gm20b::PackUniformRange &u : pp.uniform)
        u.entry = gm20b::kNoUniform;
    gm20b::setEntryCount(pp, entries);
    r.far.assign(gm20b::farWords(entries), 0);
    gm20b::entryAt(pp, r.far, 0) = {0, gm20b::EntryUbo, 2, 1, uint8_t((kResidualWord + 1) << 2), 0, 0};
    for (uint32_t k = 1; k < entries; k++)
        gm20b::entryAt(pp, r.far,
                       k) = {uint16_t(16 * k), gm20b::EntryUbo, uint8_t(k % 2 ? 2 : 1), uint8_t(k / 2), 1, 0, 0};
    pp.uniform[0] = {0, 1, 0, 0};
    pp.memoryEntries = ~0u;
    r.blob = {0x1a00, 0x1111};
    return r;
}

// `r` with what its full form adds: the fragment program's aligned variant and, with `split`, split_record_test.cpp's
// batch split
gm20b::RuntimeRecord fullOf(gm20b::RuntimeRecord r, bool split = true)
{
    r.variants.push_back({gm20b::AssumeUniformAligned, program("fs-aligned", 3)});
    r.pipeline.variantCount[1]++;
    r.splitTried = true;
    if (!split)
        return r;
    r.split = true;
    gm20b::PackSplit &s = r.splitRecord;
    s.frontierBytes = 64;
    s.frontierCbuf = 2;
    s.prologueCbufs = 1;
    s.batchVersion = 1;
    s.batchThreads = 32;
    s.qmd[18] = 32u << 16;
    s.qmd[19] = 0x10001;
    r.splitPrograms[0] = program("fs-split", 4);
    r.splitPrograms[0].cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet, gm20b::CbufFrontier};
    r.splitPrograms[1] = program("prologue", 0);
    return r;
}

class DeviceFullForm : public harness::DeviceTest {
protected:
    gm20b::Texture target;
    std::string why;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    }

    gm20b::Pipeline *placed(const gm20b::RuntimeRecord &r)
    {
        gm20b::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
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
        const char *refused = nullptr;
        const bool ok = e->draw(kTriangle, 0, 3, 1, 0, &refused);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return refused ? refused : "not committed";
        }
        return "";
    }
};

}  // namespace

TEST_F(DeviceFullForm, AFullFormOfThePlacedLayoutGivesItsVariantsAndSplit)
{
    gm20b::Pipeline *pl = placed(placedRecord(kKey));
    ASSERT_NE(pl, nullptr);
    EXPECT_EQ(pl->variantTable, nullptr);
    ASSERT_TRUE(dev.attachFullForm(fullOf(placedRecord(kKey)), why)) << why;
    ASSERT_NE(pl->variantTable, nullptr);
    EXPECT_NE(dev.variant(*pl, gm20b::AssumeUniformAligned), pl);
    EXPECT_NE(pl->splitSource, nullptr);
}

TEST_F(DeviceFullForm, AFullFormOfAnotherLayoutIsRefusedWithWhy)
{
    const uint32_t wide = gm20b::kPackEntries + 1;
    gm20b::Pipeline *pl = placed(placedRecord(kKey, wide));
    ASSERT_NE(pl, nullptr);
    const std::pair<const char *, std::function<void(gm20b::RuntimeRecord &)>> others[] = {
        {"pipeline record",
         [](gm20b::RuntimeRecord &r) {
             r.pipeline.pushMask |= 1;
         }},
        {"register list",
         [](gm20b::RuntimeRecord &r) {
             r.blob[1] = 0x2222;
         }},
        {"extra register list",
         [](gm20b::RuntimeRecord &r) {
             r.extras.push_back({0x1234, 1});
         }},
        {"constexpr sampler list",
         [](gm20b::RuntimeRecord &r) {
             r.samplers.push_back({});
         }},
        {"far block",
         [](gm20b::RuntimeRecord &r) {
             r.far.back() ^= 1;
         }},
        {"program pair",
         [](gm20b::RuntimeRecord &r) {
             r.programs[1].id = "fs of another compile";
         }},
    };
    for (const auto &[part, other] : others) {
        gm20b::RuntimeRecord full = fullOf(placedRecord(kKey, wide));
        other(full);
        EXPECT_FALSE(dev.attachFullForm(full, why)) << part;
        EXPECT_EQ(why, std::string("it is of another ") + part + " than its placed record");
    }
    EXPECT_EQ(pl->variantTable, nullptr);
    EXPECT_EQ(pl->splitSource, nullptr);
    EXPECT_TRUE(dev.attachFullForm(fullOf(placedRecord(kKey, wide)), why)) << why;
    EXPECT_NE(pl->variantTable, nullptr);
}

TEST_F(DeviceFullForm, AFullFormOfOtherArgumentTablesIsRefused)
{
    gm20b::RuntimeRecord r = placedRecord(kKey);
    gm20b::setEntryCount(r.pipeline, 2);
    r.pipeline.entry[1] = {16, gm20b::EntrySpillTable, 2, 0, 0, 0, 0};
    r.pipeline.setBytes = 32;
    r.pipeline.spillEntry[1] = 2;
    r.pipeline.spillWords[1] = 4;
    r.spill = {0, 0, 0, 0};
    gm20b::Pipeline *pl = placed(r);
    ASSERT_NE(pl, nullptr);
    gm20b::RuntimeRecord full = fullOf(r);
    full.spill[0] = 1;
    EXPECT_FALSE(dev.attachFullForm(full, why));
    EXPECT_EQ(why, "it is of another argument table than its placed record");
    EXPECT_EQ(pl->variantTable, nullptr);
}

TEST_F(DeviceFullForm, AFullFormOfNoPlacedStateIsRefused)
{
    ASSERT_NE(placed(placedRecord(kKey)), nullptr);
    EXPECT_FALSE(dev.attachFullForm(fullOf(placedRecord(kKey + 2)), why));
    EXPECT_EQ(why, "no state of its key is placed");
}

TEST_F(DeviceFullForm, AFullFormReachesItsStatesStrideTwin)
{
    gm20b::RuntimeRecord r = placedRecord(kKey);
    r.pipeline.strideTwin = kTwinKey;
    r.strideTwin = std::make_shared<gm20b::RuntimeRecord>(placedRecord(kTwinKey));
    gm20b::Pipeline *pl = placed(r);
    ASSERT_NE(pl, nullptr);
    ASSERT_NE(pl->strideTwin, nullptr);
    gm20b::RuntimeRecord full = fullOf(r);
    gm20b::RuntimeRecord twin = fullOf(*r.strideTwin);
    twin.blob[1] = 0x2222;
    full.strideTwin = std::make_shared<gm20b::RuntimeRecord>(twin);
    EXPECT_FALSE(dev.attachFullForm(full, why));
    EXPECT_EQ(why, "its stride twin is of another register list than its placed record");
    EXPECT_EQ(pl->variantTable, nullptr);
    full.strideTwin.reset();
    EXPECT_FALSE(dev.attachFullForm(full, why));
    EXPECT_EQ(why, "no stride twin in its full form");
    EXPECT_EQ(pl->variantTable, nullptr);
    full.strideTwin = std::make_shared<gm20b::RuntimeRecord>(fullOf(*r.strideTwin));
    ASSERT_TRUE(dev.attachFullForm(full, why)) << why;
    EXPECT_NE(pl->variantTable, nullptr);
    ASSERT_NE(pl->strideTwin->variantTable, nullptr);
    EXPECT_NE(dev.variant(*pl->strideTwin, gm20b::AssumeUniformAligned), pl->strideTwin);
    EXPECT_NE(pl->strideTwin->splitSource, nullptr);
}

TEST_F(DeviceFullForm, ATurnedVariantIsPlacedAtTheStatesFirstTurnedDraw)
{
    gm20b::RuntimeRecord r = fullOf(placedRecord(kKey), false);
    r.variants.insert(r.variants.begin() + r.pipeline.variantCount[0], {gm20b::AssumeTurned, program("fs-turned", 5)});
    r.pipeline.variantCount[1]++;
    gm20b::Pipeline *pl = placed(r);
    ASSERT_NE(pl, nullptr);
    ASSERT_NE(pl->variantTable, nullptr);
    EXPECT_EQ(pl->variantTable->count[1], 1u);
    const uint64_t programs = dev.stats.programsPlaced;
    EXPECT_EQ(draw(pl, false), "");
    EXPECT_EQ(dev.stats.programsPlaced, programs) << "an upright draw takes the aligned variant, placed with its state";
    EXPECT_EQ(pl->turnedTwin, nullptr);
    EXPECT_EQ(draw(pl, true), "");
    ASSERT_NE(pl->turnedTwin, nullptr);
    EXPECT_EQ(dev.stats.programsPlaced, programs + 1) << "the first turned draw places the turned variant";
    EXPECT_EQ(draw(pl, true), "");
    EXPECT_EQ(dev.stats.programsPlaced, programs + 1);
}

TEST_F(DeviceFullForm, ARuntimeVariantAddsItsOwnSetMemoryReads)
{
    gm20b::RuntimeRecord r = placedRecord(kKey, 2);
    r.pipeline.memoryEntries = 1;
    gm20b::Pipeline *pl = placed(r);
    ASSERT_NE(pl, nullptr);
    gm20b::RuntimeRecord full = fullOf(r, false);
    // LDC R0, c[1][0x10] (bank 1 = CbufSet, offset 16 = entry 1, Ra = RZ = 255) in slot 1 of group 0 (after sched word)
    const uint64_t ldcEntry1 =
        (uint64_t(0xef94u) << 48) | (uint64_t(1u) << 36) | (uint64_t(16u) << 20) | (uint64_t(255u) << 8);
    memcpy(full.variants[0].program.image.data() + 80 + 8, &ldcEntry1, sizeof ldcEntry1);
    ASSERT_TRUE(dev.attachFullForm(full, why)) << why;
    gm20b::Pipeline *aligned = dev.variant(*pl, gm20b::AssumeUniformAligned);
    ASSERT_NE(aligned, pl);
    EXPECT_EQ(pl->memoryEntries, 1u);
    EXPECT_EQ(aligned->memoryEntries, 3u);
}

TEST_F(DeviceFullForm, AStorageFormsFullFormGetsVariantsWithoutSplit)
{
    gm20b::RuntimeRecord r = placedRecord(kKey, 2);
    gm20b::entryAt(r.pipeline, r.far, 1) = {16, gm20b::EntryFetch, 2, 0, gm20b::SamplerNone, 0, 0};
    r.pipeline.flags |= gm20b::PipelineFetches | gm20b::PipelineStorageMetadata;
    r.pipeline.storageMask = r.pipeline.storageWriteMask = 1;
    r.pipeline.storageSamples = 1;
    r.pipeline.storageTic[0] = gm20b::formatInfo(kRGBA8)->tic0;
    r.blob = {0x1224, 1, 0x1a00, 0};
    gm20b::Pipeline *pl = placed(r);
    ASSERT_NE(pl, nullptr);
    ASSERT_TRUE(dev.attachFullForm(fullOf(r), why)) << why;
    ASSERT_NE(pl->variantTable, nullptr);
    EXPECT_NE(dev.variant(*pl, gm20b::AssumeUniformAligned), pl);
    EXPECT_EQ(pl->splitSource, nullptr);
}
