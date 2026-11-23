// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include "../record_parts.h"

namespace {

gm20b::PackEntry fetch(uint16_t offset, uint8_t colour)
{
    return {offset, gm20b::EntryFetch, 2, colour, gm20b::SamplerNone, 0, 0};
}

// A storage form fetching colours 0 and 1 (entries 0 and 1), writing colour 0, its programs read `reads` of set 0
gm20b::RuntimeRecord storageForm(uint32_t reads)
{
    gm20b::RuntimeRecord rec;
    gm20b::PackPipeline &p = rec.pipeline;
    p.flags = gm20b::PipelineFetches | gm20b::PipelineStorageMetadata;
    gm20b::setEntryCount(p, 2);
    p.entry[0] = fetch(32, 0);
    p.entry[1] = fetch(36, 1);
    p.storageMask = 3;
    p.storageWriteMask = 1;
    p.storageTic[0] = 0x11;
    p.storageTic[1] = 0x22;
    p.fetchRung[0] = gm20b::FetchInterlock;
    p.fetchRung[1] = gm20b::FetchBarrier;
    p.memoryEntries = reads;
    return rec;
}

TEST(GM20BRecordLiveness, AFetchNoProgramReadsLeavesTheRecordWithItsStorageView)
{
    gm20b::RuntimeRecord rec = storageForm(1u << 0);
    EXPECT_EQ(gm20b::dropUnreadFetches(rec), 1u << 1);
    const gm20b::PackPipeline &p = rec.pipeline;
    EXPECT_EQ(p.entry[1].kind, gm20b::EntrySamplerOnly);
    EXPECT_EQ(p.entry[1].offset, 36);
    EXPECT_EQ(p.entry[0].kind, gm20b::EntryFetch);
    EXPECT_EQ(p.storageMask, 1);
    EXPECT_EQ(p.storageWriteMask, 1);
    EXPECT_EQ(p.storageTic[1], 0u);
    EXPECT_EQ(p.fetchRung[1], gm20b::FetchNone);
    EXPECT_TRUE(p.flags & gm20b::PipelineFetches);
}

TEST(GM20BRecordLiveness, ARecordWhoseProgramsReadNoFetchFetchesNothing)
{
    gm20b::RuntimeRecord rec = storageForm(0);
    EXPECT_EQ(gm20b::dropUnreadFetches(rec), 3);
    EXPECT_FALSE(rec.pipeline.flags & gm20b::PipelineFetches);
    EXPECT_EQ(rec.pipeline.storageMask, 0);
}

TEST(GM20BRecordLiveness, ReadsThatNoAnalysisCouldNameKeepEveryFetch)
{
    gm20b::RuntimeRecord rec = storageForm(~0u);
    EXPECT_EQ(gm20b::dropUnreadFetches(rec), 0);
    EXPECT_EQ(rec.pipeline.storageMask, 3);
}

TEST(GM20BRecordLiveness, AColourOneOfWhoseEntriesIsReadStays)
{
    gm20b::RuntimeRecord rec = storageForm(1u << 2);
    gm20b::setEntryCount(rec.pipeline, 3);
    rec.pipeline.entry[2] = fetch(40, 1);
    EXPECT_EQ(gm20b::dropUnreadFetches(rec), 1u << 0);
    EXPECT_EQ(rec.pipeline.storageMask, 1u << 1);
    EXPECT_EQ(rec.pipeline.entry[1].kind, gm20b::EntryFetch);
}

// A program of one instruction group after its 0x50-byte header, `word` at the group's slot `slot` (0: its scheduling
// word)
gm20b::CapturedProgram programWith(uint64_t word, size_t slot)
{
    gm20b::CapturedProgram p;
    p.image.assign(0x50 + 32, 0);
    memcpy(p.image.data() + 0x50 + 8 * slot, &word, sizeof word);
    return p;
}

constexpr uint64_t kSuld = 0xeb00ull << 48, kSust = 0xeb20ull << 48;

TEST(GM20BRecordLiveness, AStorageFormLoadsAColourOnlyWithASurfaceLoad)
{
    gm20b::RuntimeRecord rec;
    rec.programs[1] = programWith(kSust, 1);
    EXPECT_FALSE(gm20b::loadsSurface(rec));
    rec.programs[1] = programWith(kSuld, 0);
    EXPECT_FALSE(gm20b::loadsSurface(rec));
    rec.programs[1] = programWith(kSuld, 2);
    EXPECT_TRUE(gm20b::loadsSurface(rec));
}

TEST(GM20BRecordLiveness, AVariantsSurfaceLoadIsTheRecords)
{
    gm20b::RuntimeRecord rec;
    rec.programs[1] = programWith(kSust, 1);
    rec.variants.push_back({1, programWith(kSuld, 3)});
    EXPECT_TRUE(gm20b::loadsSurface(rec));
}

// NRFV1's Fusion states draw to c0, c2 and c3: c3's write mask and blend are theirs, c1's no state's
TEST(GM20BRecordTargets, ATargetBetweenTwoAStateHasIsForeignToIt)
{
    const uint32_t mask = gm20b::targets("samples=1 c0=25:15 c2=25:15 c3=25:15 | v FusionVert");
    EXPECT_EQ(mask, 0b1101u);
    EXPECT_FALSE(gm20b::foreign(0x1A0C, mask));
    EXPECT_FALSE(gm20b::foreign(0x1E64, mask));
    EXPECT_TRUE(gm20b::foreign(0x1A04, mask));
    EXPECT_TRUE(gm20b::foreign(0x1E24, mask));
    EXPECT_TRUE(gm20b::foreign(0x1A10, mask));
}

// A GM20B stage binds 18 constant buffers (BIND_GROUP_CONSTANT_BUFFER); a pack program records each one's type
TEST(GM20BRecordPrograms, AProgramOfMoreConstantBuffersThanAPackProgramRecordsIsSetAside)
{
    gm20b::CapturedProgram vs, fs;
    vs.image.assign(96, 0);
    fs.image.assign(96, 0);
    fs.cbufTypes = {gm20b::CbufRoot};
    vs.cbufTypes = {gm20b::CbufRoot};
    for (uint32_t ubo = 0; ubo < 18; ubo++)
        vs.cbufTypes.push_back(uint8_t(gm20b::CbufUbo | ubo << 3));
    gm20b::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    std::string why;
    EXPECT_FALSE(gm20b::programsBindable(parts, why));
    EXPECT_EQ(why, "a program reads 19 constant buffers, past the 18 a pack program records");
    vs.cbufTypes.pop_back();
    EXPECT_TRUE(gm20b::programsBindable(parts, why)) << why;
}

// A render state's set 0 takes its NVK layout's size in 256-byte blocks; past two, the encoder's wide path draws it
// (descriptors.h WideDescriptors)
TEST(GM20BRecordSet, ASetPastTheEncodersTwoBlocksIsMade)
{
    EXPECT_EQ(gm20b::renderSetBytes(0), 256u);
    EXPECT_EQ(gm20b::renderSetBytes(260), 512u);
    EXPECT_EQ(gm20b::renderSetBytes(520), 768u);
    gm20b::CapturedProgram vs, fs;
    vs.image.assign(96, 0);
    fs.image.assign(96, 0);
    vs.cbufTypes = fs.cbufTypes = {gm20b::CbufRoot};
    gm20b::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    gm20b::RuntimeRecord out;
    std::string why;
    parts.setBytes = 520;
    EXPECT_TRUE(gm20b::assembleRecord(parts, "c0=80 | v vs | f fs", gm20b::PackRules{}, out, why)) << why;
}

// Every uniform buffer a program reads is an entry of the record's set 0: the draw's check of it (RenderEncoder::prepare,
// RefUnnamedUniform) is the driver's error for a record made otherwise
TEST(GM20BRecordSet, AProgramReadingAUniformBufferNoEntryNamesIsSetAside)
{
    gm20b::CapturedProgram vs, fs;
    vs.image.assign(96, 0);
    fs.image.assign(96, 0);
    vs.cbufTypes = {gm20b::CbufRoot};
    fs.cbufTypes = {gm20b::CbufRoot, uint8_t(gm20b::CbufUbo | (32 / 16) << 3)};
    gm20b::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    gm20b::RuntimeRecord out;
    std::string why;
    EXPECT_FALSE(gm20b::assembleRecord(parts, "c0=80 | v vs | f fs", gm20b::PackRules{}, out, why));
    EXPECT_EQ(why, "a program reads a set-0 uniform buffer the layout does not name");
}

}  // namespace
