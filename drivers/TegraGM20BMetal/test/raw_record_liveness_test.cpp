// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstring>
#include "../raw/runtime/raw_record_parts.h"

namespace {

raw::PackEntry fetch(uint16_t offset, uint8_t colour)
{
    return {offset, raw::EntryFetch, 2, colour, raw::SamplerNone, 0, 0};
}

// A storage form fetching colours 0 and 1 (entries 0 and 1), writing colour 0, its programs read `reads` of set 0
raw::RuntimeRecord storageForm(uint32_t reads)
{
    raw::RuntimeRecord rec;
    raw::PackPipeline &p = rec.pipeline;
    p.flags = raw::PipelineFetches | raw::PipelineStorageMetadata;
    p.entries = 2;
    p.entry[0] = fetch(32, 0);
    p.entry[1] = fetch(36, 1);
    p.storageMask = 3;
    p.storageWriteMask = 1;
    p.storageTic[0] = 0x11;
    p.storageTic[1] = 0x22;
    p.fetchRung[0] = raw::FetchInterlock;
    p.fetchRung[1] = raw::FetchBarrier;
    p.memoryEntries = reads;
    return rec;
}

TEST(RawRecordLiveness, AFetchNoProgramReadsLeavesTheRecordWithItsStorageView)
{
    raw::RuntimeRecord rec = storageForm(1u << 0);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 1u << 1);
    const raw::PackPipeline &p = rec.pipeline;
    EXPECT_EQ(p.entry[1].kind, raw::EntrySamplerOnly);
    EXPECT_EQ(p.entry[1].offset, 36);
    EXPECT_EQ(p.entry[0].kind, raw::EntryFetch);
    EXPECT_EQ(p.storageMask, 1);
    EXPECT_EQ(p.storageWriteMask, 1);
    EXPECT_EQ(p.storageTic[1], 0u);
    EXPECT_EQ(p.fetchRung[1], raw::FetchNone);
    EXPECT_TRUE(p.flags & raw::PipelineFetches);
}

TEST(RawRecordLiveness, ARecordWhoseProgramsReadNoFetchFetchesNothing)
{
    raw::RuntimeRecord rec = storageForm(0);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 3);
    EXPECT_FALSE(rec.pipeline.flags & raw::PipelineFetches);
    EXPECT_EQ(rec.pipeline.storageMask, 0);
}

TEST(RawRecordLiveness, ReadsThatNoAnalysisCouldNameKeepEveryFetch)
{
    raw::RuntimeRecord rec = storageForm(~0u);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 0);
    EXPECT_EQ(rec.pipeline.storageMask, 3);
}

TEST(RawRecordLiveness, AColourOneOfWhoseEntriesIsReadStays)
{
    raw::RuntimeRecord rec = storageForm(1u << 2);
    rec.pipeline.entries = 3;
    rec.pipeline.entry[2] = fetch(40, 1);
    EXPECT_EQ(raw::dropUnreadFetches(rec), 1u << 0);
    EXPECT_EQ(rec.pipeline.storageMask, 1u << 1);
    EXPECT_EQ(rec.pipeline.entry[1].kind, raw::EntryFetch);
}

// A program of one instruction group after its 0x50-byte header, `word` at the group's slot `slot` (0: its scheduling
// word)
raw::CapturedProgram programWith(uint64_t word, size_t slot)
{
    raw::CapturedProgram p;
    p.image.assign(0x50 + 32, 0);
    memcpy(p.image.data() + 0x50 + 8 * slot, &word, sizeof word);
    return p;
}

constexpr uint64_t kSuld = 0xeb00ull << 48, kSust = 0xeb20ull << 48;

TEST(RawRecordLiveness, AStorageFormLoadsAColourOnlyWithASurfaceLoad)
{
    raw::RuntimeRecord rec;
    rec.programs[1] = programWith(kSust, 1);
    EXPECT_FALSE(raw::loadsSurface(rec));
    rec.programs[1] = programWith(kSuld, 0);
    EXPECT_FALSE(raw::loadsSurface(rec));
    rec.programs[1] = programWith(kSuld, 2);
    EXPECT_TRUE(raw::loadsSurface(rec));
}

TEST(RawRecordLiveness, AVariantsSurfaceLoadIsTheRecords)
{
    raw::RuntimeRecord rec;
    rec.programs[1] = programWith(kSust, 1);
    rec.variants.push_back({1, programWith(kSuld, 3)});
    EXPECT_TRUE(raw::loadsSurface(rec));
}

// NRFV1's Fusion states draw to c0, c2 and c3: c3's write mask and blend are theirs, c1's no state's
TEST(RawRecordTargets, ATargetBetweenTwoAStateHasIsForeignToIt)
{
    const uint32_t mask = raw::targets("samples=1 c0=25:15 c2=25:15 c3=25:15 | v FusionVert");
    EXPECT_EQ(mask, 0b1101u);
    EXPECT_FALSE(raw::foreign(0x1A0C, mask));
    EXPECT_FALSE(raw::foreign(0x1E64, mask));
    EXPECT_TRUE(raw::foreign(0x1A04, mask));
    EXPECT_TRUE(raw::foreign(0x1E24, mask));
    EXPECT_TRUE(raw::foreign(0x1A10, mask));
}

// A GM20B stage binds 18 constant buffers (BIND_GROUP_CONSTANT_BUFFER); a pack program records each one's type
TEST(RawRecordPrograms, AProgramOfMoreConstantBuffersThanAPackProgramRecordsIsSetAside)
{
    raw::CapturedProgram vs, fs;
    vs.image.assign(96, 0);
    fs.image.assign(96, 0);
    fs.cbufTypes = {raw::CbufRoot};
    vs.cbufTypes = {raw::CbufRoot};
    for (uint32_t ubo = 0; ubo < 18; ubo++)
        vs.cbufTypes.push_back(uint8_t(raw::CbufUbo | ubo << 3));
    raw::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    std::string why;
    EXPECT_FALSE(raw::programsBindable(parts, why));
    EXPECT_EQ(why, "a program reads 19 constant buffers, past the 18 a pack program records");
    vs.cbufTypes.pop_back();
    EXPECT_TRUE(raw::programsBindable(parts, why)) << why;
}

// A render state's set 0 takes its NVK layout's size in 256-byte blocks, two at most (raw_descriptors.h)
TEST(RawRecordSet, ASetPastTheEncodersRoomIsSetAside)
{
    EXPECT_EQ(raw::renderSetBytes(0), 256u);
    EXPECT_EQ(raw::renderSetBytes(260), 512u);
    raw::CapturedProgram vs, fs;
    vs.image.assign(96, 0);
    fs.image.assign(96, 0);
    vs.cbufTypes = fs.cbufTypes = {raw::CbufRoot};
    raw::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    raw::RuntimeRecord out;
    std::string why;
    parts.setBytes = 512;
    EXPECT_TRUE(raw::assembleRecord(parts, "c0=80 | v vs | f fs", raw::PackRules{}, out, why)) << why;
    parts.setBytes = 520;
    EXPECT_FALSE(raw::assembleRecord(parts, "c0=80 | v vs | f fs", raw::PackRules{}, out, why));
    EXPECT_EQ(why, "a set 0 of 520 bytes, past the render encoder's 512");
}

}  // namespace
