// SPDX-License-Identifier: MIT
// A buffer entry's residual word (record_parts.cpp bufferEntry): the word m2v's plan places the slot's SlotBindOffset
// residual in, root or argument table, even where two storage slots share the probe's code; the probe's own naming only
// for records made without a plan
#include <gtest/gtest.h>
#include <vector>
#include "../pack/probe_bind.h"
#include "../record_parts.h"
#include "../spill_table.h"

namespace {

constexpr uint8_t kFragment = 2;

gm20b::CapturedProgram program(const char *id)
{
    gm20b::CapturedProgram p;
    p.id = id;
    p.image.assign(96, 0);
    p.cbufTypes = {gm20b::CbufRoot};
    return p;
}

gm20b::LayoutEntry storage(uint32_t offset, uint8_t slot)
{
    gm20b::LayoutEntry e;
    e.offset = offset;
    e.buffer = true;
    e.stage = kFragment;
    e.slot = slot;
    return e;
}

// Fragment storage buffers at slots 0 and 12, whose probe codes are both 1
gm20b::RecordParts sharedCodeParts()
{
    static const gm20b::CapturedProgram vs = program("vs"), fs = program("fs");
    EXPECT_EQ(gm20b::probeStorageCode(kFragment, 0), gm20b::probeStorageCode(kFragment, 12));
    gm20b::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    parts.layout = {storage(0, 0), storage(16, 12)};
    for (const gm20b::LayoutEntry &e : parts.layout) {
        parts.sizes16[e.offset] = 16;
        parts.sizes4096[e.offset] = 4096;
    }
    parts.residuals[gm20b::probeStorageCode(kFragment, 0)] = 0x704;
    return parts;
}

std::vector<uint32_t> residualWords(const gm20b::RecordParts &parts)
{
    gm20b::RuntimeRecord out;
    std::string why;
    EXPECT_TRUE(gm20b::assembleRecord(parts, "c0=80 | v vs | f fs", gm20b::PackRules{}, out, why)) << why;
    std::vector<uint32_t> words;
    for (uint32_t k = 0; k < gm20b::entryCount(out.pipeline); k++) {
        EXPECT_EQ(out.pipeline.entry[k].samplerKind & 1, 1u) << k;
        words.push_back(out.pipeline.entry[k].samplerKind >> 2);
    }
    return words;
}

TEST(RecordResiduals, StorageSlotsSharingACodeTakeThePlansRootAndTableWords)
{
    gm20b::RecordParts parts = sharedCodeParts();
    parts.residualsPlanned = true;
    parts.residualWord[1][0] = 3;
    parts.residualWord[1][12] = gm20b::kRootPushWords + 8;
    EXPECT_EQ(residualWords(parts), (std::vector<uint32_t>{3, gm20b::kRootPushWords + 8}));
}

TEST(RecordResiduals, ThePlanOutranksTheProbesWord)
{
    gm20b::RecordParts parts = sharedCodeParts();
    parts.layout.pop_back();
    parts.residualsPlanned = true;
    parts.residualWord[1][0] = 7;
    EXPECT_EQ(residualWords(parts), std::vector<uint32_t>{7});
    parts.residualWord[1][0] = 0;
    EXPECT_EQ(residualWords(parts), std::vector<uint32_t>{0});
}

TEST(RecordResiduals, WithoutAPlanASharedCodeNamesNoWord)
{
    gm20b::RecordParts parts = sharedCodeParts();
    EXPECT_EQ(residualWords(parts), (std::vector<uint32_t>{0, 0}));
    parts.layout.pop_back();
    EXPECT_EQ(residualWords(parts), std::vector<uint32_t>{2});
}

}  // namespace
