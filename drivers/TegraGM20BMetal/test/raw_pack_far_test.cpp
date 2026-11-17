// SPDX-License-Identifier: MIT
// A pack record past kPackEntries set-0 entries reads the rest from its far block in the blob section, and a pack program
// its uniform buffers past set-0 byte 496 from its far table there (pack version 27). A record or program pointing past the
// blob section, or a record past every argument a render pipeline binds, fails the pack's load, not a draw.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_set_entries.h"

namespace {

constexpr uint32_t kBlob = 0x1000, kPipelines = 0x200, kPrograms = 0xA00;

struct WidePack {
    uint32_t entries = 40, farAt = 4, blobWords = 64, programFar = 0;
};

std::string write(const WidePack &w, const char *name)
{
    std::vector<uint8_t> file(kBlob + 4 * w.blobWords, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.pipelines = 1;
    h.programs = 1;
    h.section[raw::PackPipelines] = {kPipelines, sizeof(raw::PackPipeline)};
    h.section[raw::PackPrograms] = {kPrograms, sizeof(raw::PackProgram)};
    h.section[raw::PackBlob] = {kBlob, 4 * w.blobWords};
    memcpy(file.data(), &h, sizeof h);
    raw::PackPipeline pp = {};
    pp.keyHash = 7;
    raw::setEntryCount(pp, w.entries);
    pp.farEntriesAt[0] = uint8_t(w.farAt), pp.farEntriesAt[1] = uint8_t(w.farAt >> 8);
    pp.farEntriesAt[2] = uint8_t(w.farAt >> 16);
    for (uint32_t k = 0; k < raw::kPackEntries; k++)
        pp.entry[k] = {uint16_t(16 * k), raw::EntryUbo, 1, uint8_t(k), 1, 0, 0};
    pp.memoryEntries = 0x80000001u;
    memcpy(file.data() + kPipelines, &pp, sizeof pp);
    raw::PackProgram program = {};
    program.cbufs = 2;
    program.cbufType[0] = raw::CbufRoot;
    program.cbufType[1] = raw::CbufUbo;
    program.farUbos[0] = uint8_t(w.programFar), program.farUbos[1] = uint8_t(w.programFar >> 8);
    memcpy(file.data() + kPrograms, &program, sizeof program);
    const uint32_t far = raw::farEntries(w.entries);
    for (uint32_t k = 0; k < far && 4 * (w.farAt + 2 * k) + 8 <= 4 * w.blobWords; k++) {
        const raw::PackEntry e = {uint16_t(16 * (raw::kPackEntries + k)), raw::EntryUbo, 2, uint8_t(k), 1, 0, 0};
        memcpy(file.data() + kBlob + 4 * (w.farAt + 2 * k), &e, sizeof e);
    }
    const uint32_t masks = raw::farMaskWords(w.entries), bound = 0, memory = 1u << 7;
    if (w.farAt + raw::farWords(w.entries) <= w.blobWords && masks) {
        memcpy(file.data() + kBlob + 4 * (w.farAt + 2 * far), &bound, 4);
        memcpy(file.data() + kBlob + 4 * (w.farAt + 2 * far + masks), &memory, 4);
    }
    const uint16_t table[raw::kProgramCbufs] = {0, 640 / 16};
    if (w.programFar && w.programFar - 1 + raw::kProgramCbufs / 2 <= w.blobWords)
        memcpy(file.data() + kBlob + 4 * (w.programFar - 1), table, sizeof table);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(PackFar, ARecordPastThirtyTwoEntriesReadsTheRestFromItsFarBlock)
{
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(write({}, "far_entries.pack").c_str(), 0, err)) << err;
    const raw::PackPipeline &pp = *pack.section<raw::PackPipeline>(raw::PackPipelines);
    ASSERT_NE(pack.far(pp), nullptr);
    const raw::SetEntries entries(pp, pack.far(pp));
    ASSERT_EQ(entries.size(), 40u);
    EXPECT_TRUE(entries.wide());
    for (uint32_t k = 0; k < 40; k++)
        EXPECT_EQ(entries[k].offset, 16 * k) << k;
    EXPECT_EQ(entries[39].stage, 2u);
    EXPECT_TRUE(entries.memory(0));
    EXPECT_TRUE(entries.memory(31));
    EXPECT_FALSE(entries.memory(32));
    EXPECT_TRUE(entries.memory(39));
}

TEST(PackFar, AFarBlockPastTheBlobSectionFailsTheLoad)
{
    raw::Pack pack;
    std::string err;
    WidePack w;
    w.farAt = 64 - raw::farWords(40) + 1;
    EXPECT_FALSE(pack.load(write(w, "far_past.pack").c_str(), 0, err));
    EXPECT_EQ(err, "pipeline 0000000000000007 of 40 set-0 entries: its far entries past the blob section");
}

TEST(PackFar, ARecordPastEveryArgumentOfARenderPipelineFailsTheLoad)
{
    raw::Pack pack;
    std::string err;
    WidePack w;
    w.entries = raw::kSetEntriesMax + 1;
    w.blobWords = 1024;
    EXPECT_FALSE(pack.load(write(w, "far_bound.pack").c_str(), 0, err));
    EXPECT_EQ(err, "pipeline 0000000000000007 of 379 set-0 entries: past the 378 a record holds");
}

TEST(PackFar, AProgramsFarTableNamesItsUniformBuffersPastByte496)
{
    raw::Pack pack;
    std::string err;
    WidePack w;
    w.programFar = 31;
    ASSERT_TRUE(pack.load(write(w, "far_ubos.pack").c_str(), 0, err)) << err;
    const raw::PackProgram &program = *pack.section<raw::PackProgram>(raw::PackPrograms);
    ASSERT_NE(pack.farUbos(program), nullptr);
    EXPECT_EQ(raw::cbufUboOffset(program.cbufType, pack.farUbos(program), 1), 640u);
}

TEST(PackFar, AProgramsFarTablePastTheBlobSectionFailsTheLoad)
{
    raw::Pack pack;
    std::string err;
    WidePack w;
    w.programFar = 60;
    EXPECT_FALSE(pack.load(write(w, "far_ubos_past.pack").c_str(), 0, err));
    EXPECT_EQ(err, "program 0's far uniform buffers past the blob section");
}

TEST(PackFar, ARecordOfThirtyTwoEntriesOrFewerHasNoFarBlock)
{
    raw::Pack pack;
    std::string err;
    WidePack w;
    w.entries = 32;
    w.farAt = 0;
    ASSERT_TRUE(pack.load(write(w, "far_none.pack").c_str(), 0, err)) << err;
    const raw::PackPipeline &pp = *pack.section<raw::PackPipeline>(raw::PackPipelines);
    EXPECT_EQ(pack.far(pp), nullptr);
    EXPECT_FALSE(raw::SetEntries(pp, nullptr).wide());
    EXPECT_TRUE(raw::SetEntries(pp, nullptr).complete());
}

}  // namespace
