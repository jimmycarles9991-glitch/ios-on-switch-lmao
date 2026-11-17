// SPDX-License-Identifier: MIT
// A record past kPackEntries set-0 entries (raw_set_entries.h): its entries past them and their bound and memory bits in its
// far block, up to every argument a render pipeline binds; a set past 512 bytes; and a uniform buffer past set-0 byte 496,
// which NVK's code names CbufUnbindable and its stage reports the offset of (raw_direct_encode.h capturedCbufs), held in the
// program's far table. A uniform buffer of another set stays unbindable and the state is refused by name.
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "../raw/runtime/raw_direct_encode.h"
#include "../raw/runtime/raw_record_parts.h"
#include "../raw/runtime/raw_set_entries.h"

namespace {

constexpr char kKey[] = "c0=80 | v vs | f fs";

raw::LayoutEntry storageBuffer(uint8_t stage, uint8_t slot, uint32_t offset)
{
    raw::LayoutEntry e;
    e.offset = offset;
    e.buffer = true;
    e.stage = stage;
    e.slot = slot;
    return e;
}

struct Parts {
    raw::CapturedProgram vs, fs;
    raw::RecordParts parts;

    explicit Parts(uint32_t buffers)
    {
        vs.image.assign(96, 0);
        fs.image.assign(96, 0);
        vs.cbufTypes = fs.cbufTypes = {raw::CbufRoot};
        parts.programs[0] = &vs;
        parts.programs[1] = &fs;
        for (uint32_t i = 0; i < buffers; i++) {
            const uint32_t offset = 16 * i;
            parts.layout.push_back(storageBuffer(uint8_t(i % 2 ? 2 : 1), uint8_t(i / 2), offset));
            parts.sizes16[offset] = 16;
            parts.sizes4096[offset] = 4096;
        }
        parts.setBytes = uint16_t(16 * buffers);
    }
};

TEST(RecordFar, ARecordPastThirtyTwoEntriesKeepsTheRestInItsFarBlock)
{
    Parts p(40);
    raw::RuntimeRecord out;
    std::string why;
    ASSERT_TRUE(raw::assembleRecord(p.parts, kKey, raw::PackRules{}, out, why)) << why;
    EXPECT_EQ(raw::entryCount(out.pipeline), 40u);
    EXPECT_EQ(out.pipeline.entriesLow, 40u);
    EXPECT_EQ(out.pipeline.entriesHigh, 0u);
    ASSERT_EQ(out.far.size(), raw::farWords(40));
    EXPECT_EQ(out.far.size(), 2u * 8 + 2);
    const raw::SetEntries entries = raw::entriesOf(out);
    ASSERT_TRUE(entries.complete());
    for (uint32_t k = 0; k < 40; k++) {
        EXPECT_EQ(entries[k].offset, 16 * k) << k;
        EXPECT_EQ(entries[k].kind, raw::EntryUbo) << k;
        EXPECT_EQ(entries[k].slot, k / 2) << k;
    }
}

TEST(RecordFar, ASetPastTheEncodersFiveHundredTwelveBytesIsMade)
{
    Parts p(40);
    raw::RuntimeRecord out;
    std::string why;
    EXPECT_EQ(p.parts.setBytes, 640u);
    EXPECT_TRUE(raw::assembleRecord(p.parts, kKey, raw::PackRules{}, out, why)) << why;
    EXPECT_EQ(out.pipeline.setBytes, 640u);
}

TEST(RecordFar, EveryArgumentOfTheWidestRenderPipelineFitsAndOneMoreIsRefused)
{
    EXPECT_EQ(raw::kSetEntriesMax, 350u + 28u);
    Parts held(raw::kSetEntriesMax), past(raw::kSetEntriesMax + 1);
    raw::RuntimeRecord out;
    std::string why;
    ASSERT_TRUE(raw::assembleRecord(held.parts, kKey, raw::PackRules{}, out, why)) << why;
    EXPECT_EQ(raw::entryCount(out.pipeline), raw::kSetEntriesMax);
    EXPECT_EQ(out.pipeline.entriesHigh, raw::kSetEntriesMax >> 8);
    EXPECT_EQ(raw::entriesOf(out)[raw::kSetEntriesMax - 1].offset, 16 * (raw::kSetEntriesMax - 1));
    EXPECT_FALSE(raw::assembleRecord(past.parts, kKey, raw::PackRules{}, out, why));
    EXPECT_EQ(why, "more than 378 set-0 entries, every argument a render pipeline binds");
}

TEST(RecordFar, AnArgumentTablePastEntry254IsNamedWithItsHighByte)
{
    raw::PackPipeline p = {};
    raw::setSpillEntry(p, 1, 300);
    raw::setSpillEntry(p, 0, 7);
    EXPECT_EQ(raw::spillEntryOf(p, 1), 300u);
    EXPECT_EQ(raw::spillEntryOf(p, 0), 7u);
    EXPECT_EQ(p.spillEntry[1], 300u & 0xFF);
    EXPECT_EQ(p.spillEntryHigh, 1u << 8);
}

// A fragment program reading the uniform buffer whose descriptor is entry 40 (set-0 byte 640) as constant buffer 1
TEST(RecordFar, AUniformBufferPastByte496IsItsProgramsFarConstantBuffer)
{
    Parts p(40);
    p.parts.layout.push_back(storageBuffer(2, 20, 640));
    p.parts.sizes16[640] = 256;
    p.parts.sizes4096[640] = 4096;
    p.parts.setBytes = 656;
    p.fs.cbufTypes = {raw::CbufRoot, raw::uboCbufType(640)};
    p.fs.farUbos = {0, 640 / 16};
    EXPECT_EQ(p.fs.cbufTypes[1], raw::CbufUbo);
    EXPECT_EQ(raw::cbufUboOffset(p.fs.cbufTypes.data(), raw::farUbosOf(p.fs), 1), 640u);
    raw::RuntimeRecord out;
    std::string why;
    ASSERT_TRUE(raw::assembleRecord(p.parts, kKey, raw::PackRules{}, out, why)) << why;
    EXPECT_EQ(out.programs[1].farUbos, (std::vector<uint16_t>{0, 40}));
}

TEST(RecordFar, AUniformBufferPastItsSetIsRefusedByOffset)
{
    Parts p(40);
    p.fs.cbufTypes = {raw::CbufRoot, raw::uboCbufType(1024)};
    p.fs.farUbos = {0, 1024 / 16};
    raw::RuntimeRecord out;
    std::string why;
    EXPECT_FALSE(raw::assembleRecord(p.parts, kKey, raw::PackRules{}, out, why));
    EXPECT_EQ(why, "a program reads a uniform buffer descriptor at set-0 byte 1024, past its set of 640 bytes");
}

raw::NvkStage stage(std::vector<std::pair<uint32_t, uint32_t>> setAndOffset)
{
    raw::NvkStage s = {};
    s.cbufTypes[0] = raw::CbufRoot;
    s.cbufCount = uint32_t(1 + setAndOffset.size());
    for (uint32_t c = 1; c < s.cbufCount; c++) {
        const auto [set, offset] = setAndOffset[c - 1];
        const bool inline_ = set == 0 && offset < raw::kInlineUboBytes && offset % 16 == 0;
        s.cbufTypes[c] = inline_ ? raw::uboCbufType(offset) : uint8_t(raw::CbufUnbindable);
        s.cbufSets[c] = uint8_t(set);
        s.cbufOffsets[c] = offset;
    }
    return s;
}

// Two uniform buffers past byte 496 in one stage: each keeps its own offset, whatever their order
TEST(RecordFar, EachUniformBufferPastByte496KeepsTheOffsetNvkCompiledItAt)
{
    raw::CapturedProgram p;
    raw::capturedCbufs(stage({{0, 640}, {0, 32}, {0, 1008}}), p);
    EXPECT_EQ(p.cbufTypes, (std::vector<uint8_t>{raw::CbufRoot, raw::CbufUbo, raw::uboCbufType(32), raw::CbufUbo}));
    EXPECT_EQ(p.farUbos, (std::vector<uint16_t>{0, 40, 0, 63}));
    EXPECT_EQ(raw::cbufUboOffset(p.cbufTypes.data(), raw::farUbosOf(p), 1), 640u);
    EXPECT_EQ(raw::cbufUboOffset(p.cbufTypes.data(), raw::farUbosOf(p), 2), 32u);
    EXPECT_EQ(raw::cbufUboOffset(p.cbufTypes.data(), raw::farUbosOf(p), 3), 1008u);
    raw::CapturedProgram near;
    raw::capturedCbufs(stage({{0, 496}}), near);
    EXPECT_TRUE(near.farUbos.empty()) << "a program of 32 or fewer entries keeps its code bytes";
}

// A two-plane texture's set-0 binding past 63 (a wide state's fragment textures after its buffers) is in the mask the stage
// compile joins its planes by, whatever word it falls in
TEST(RecordFar, ATwoPlaneTexturePastBinding63IsInTheStageCompilesMask)
{
    std::vector<NmbDirectWrite> writes(3);
    writes[0] = {0, 70, 0, raw::SampledImage, NmbDirectTexture, 2, 3, 0, 0, 1, 0, 0};
    writes[1] = {0, 300, 0, raw::SampledImage, NmbDirectTexture, 2, 9, 0, 0, 1, 0, 0};
    writes[2] = {0, 5, 0, raw::SampledImage, NmbDirectTexture, 2, 4, 0, 0, 1, 0, 0};
    NmbDirectDraw draw = {};
    draw.writes = writes.data();
    draw.writeCount = uint32_t(writes.size());
    const raw::XrPlaneBindings both = raw::xrPlaneBindings(draw, 1u << 3 | 1u << 9);
    EXPECT_EQ(both[1], uint64_t(1) << (70 - 64));
    EXPECT_EQ(both[4], uint64_t(1) << (300 - 256));
    EXPECT_EQ(both[0] | both[2] | both[3] | both[5], 0u);
    EXPECT_EQ(raw::xrPlaneBindings(draw, 1u << 4)[0], uint64_t(1) << 5);
    EXPECT_EQ(raw::xrPlaneBindings(draw, 1u << 3 | 1u << 7), raw::XrPlaneBindings{}) << "slot 7 has no write";
}

// A uniform buffer of set 1, or off 16 bytes, stays CbufUnbindable, and the state that reads it is refused by name
TEST(RecordFar, AUniformBufferOfAnotherSetIsRefusedNamingTheState)
{
    Parts p(4);
    raw::capturedCbufs(stage({{1, 640}}), p.fs);
    EXPECT_EQ(p.fs.cbufTypes[1], raw::CbufUnbindable);
    EXPECT_TRUE(p.fs.farUbos.empty());
    raw::CapturedProgram misaligned;
    raw::capturedCbufs(stage({{0, 648}}), misaligned);
    EXPECT_EQ(misaligned.cbufTypes[1], raw::CbufUnbindable);
    raw::RuntimeRecord out;
    std::string why;
    EXPECT_FALSE(raw::assembleRecord(p.parts, kKey, raw::PackRules{}, out, why));
    EXPECT_EQ(why, "v vs + f fs: a program reads constant buffers [1, 7], the raw path binds root, shader data, set 0, "
                   "its uniform buffers and a heap's set 1");
}

}  // namespace
