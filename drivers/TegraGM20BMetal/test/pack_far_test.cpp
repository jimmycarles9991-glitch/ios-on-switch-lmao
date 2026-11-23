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
#include "../pack.h"
#include "../set_entries.h"

namespace {

constexpr uint32_t kBlob = 0x1000, kPipelines = 0x200, kPrograms = 0xA00;

struct WidePack {
    uint32_t entries = 40, farAt = 4, blobWords = 64, programFar = 0;
};

std::string write(const WidePack &w, const char *name)
{
    std::vector<uint8_t> file(kBlob + 4 * w.blobWords, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.pipelines = 1;
    h.programs = 1;
    h.section[gm20b::PackPipelines] = {kPipelines, sizeof(gm20b::PackPipeline)};
    h.section[gm20b::PackPrograms] = {kPrograms, sizeof(gm20b::PackProgram)};
    h.section[gm20b::PackBlob] = {kBlob, 4 * w.blobWords};
    memcpy(file.data(), &h, sizeof h);
    gm20b::PackPipeline pp = {};
    pp.keyHash = 7;
    gm20b::setEntryCount(pp, w.entries);
    pp.farEntriesAt[0] = uint8_t(w.farAt), pp.farEntriesAt[1] = uint8_t(w.farAt >> 8);
    pp.farEntriesAt[2] = uint8_t(w.farAt >> 16);
    for (uint32_t k = 0; k < gm20b::kPackEntries; k++)
        pp.entry[k] = {uint16_t(16 * k), gm20b::EntryUbo, 1, uint8_t(k), 1, 0, 0};
    pp.memoryEntries = 0x80000001u;
    memcpy(file.data() + kPipelines, &pp, sizeof pp);
    gm20b::PackProgram program = {};
    program.cbufs = 2;
    program.cbufType[0] = gm20b::CbufRoot;
    program.cbufType[1] = gm20b::CbufUbo;
    program.farUbos[0] = uint8_t(w.programFar), program.farUbos[1] = uint8_t(w.programFar >> 8);
    memcpy(file.data() + kPrograms, &program, sizeof program);
    const uint32_t far = gm20b::farEntries(w.entries);
    for (uint32_t k = 0; k < far && 4 * (w.farAt + 2 * k) + 8 <= 4 * w.blobWords; k++) {
        const gm20b::PackEntry e = {uint16_t(16 * (gm20b::kPackEntries + k)), gm20b::EntryUbo, 2, uint8_t(k), 1, 0, 0};
        memcpy(file.data() + kBlob + 4 * (w.farAt + 2 * k), &e, sizeof e);
    }
    const uint32_t masks = gm20b::farMaskWords(w.entries), bound = 0, memory = 1u << 7;
    if (w.farAt + gm20b::farWords(w.entries) <= w.blobWords && masks) {
        memcpy(file.data() + kBlob + 4 * (w.farAt + 2 * far), &bound, 4);
        memcpy(file.data() + kBlob + 4 * (w.farAt + 2 * far + masks), &memory, 4);
    }
    const uint16_t table[gm20b::kProgramCbufs] = {0, 640 / 16};
    if (w.programFar && w.programFar - 1 + gm20b::kProgramCbufs / 2 <= w.blobWords)
        memcpy(file.data() + kBlob + 4 * (w.programFar - 1), table, sizeof table);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(PackFar, ARecordPastThirtyTwoEntriesReadsTheRestFromItsFarBlock)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(write({}, "far_entries.pack").c_str(), 0, err)) << err;
    const gm20b::PackPipeline &pp = *pack.section<gm20b::PackPipeline>(gm20b::PackPipelines);
    ASSERT_NE(pack.far(pp), nullptr);
    const gm20b::SetEntries entries(pp, pack.far(pp));
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
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.farAt = 64 - gm20b::farWords(40) + 1;
    EXPECT_FALSE(pack.load(write(w, "far_past.pack").c_str(), 0, err));
    EXPECT_EQ(err, "pipeline 0000000000000007 of 40 set-0 entries: its far entries past the blob section");
}

TEST(PackFar, ARecordPastEveryArgumentOfARenderPipelineFailsTheLoad)
{
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.entries = gm20b::kSetEntriesMax + 1;
    w.blobWords = 1024;
    EXPECT_FALSE(pack.load(write(w, "far_bound.pack").c_str(), 0, err));
    EXPECT_EQ(err, "pipeline 0000000000000007 of 371 set-0 entries: past the 370 a record holds");
}

TEST(PackFar, AProgramsFarTableNamesItsUniformBuffersPastByte496)
{
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.programFar = 31;
    ASSERT_TRUE(pack.load(write(w, "far_ubos.pack").c_str(), 0, err)) << err;
    const gm20b::PackProgram &program = *pack.section<gm20b::PackProgram>(gm20b::PackPrograms);
    ASSERT_NE(pack.farUbos(program), nullptr);
    EXPECT_EQ(gm20b::cbufUboOffset(program.cbufType, pack.farUbos(program), 1), 640u);
}

TEST(PackFar, AProgramsFarTablePastTheBlobSectionFailsTheLoad)
{
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.programFar = 60;
    EXPECT_FALSE(pack.load(write(w, "far_ubos_past.pack").c_str(), 0, err));
    EXPECT_EQ(err, "program 0's far uniform buffers past the blob section");
}

TEST(PackFar, ARecordOfThirtyTwoEntriesOrFewerHasNoFarBlock)
{
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.entries = 32;
    w.farAt = 0;
    ASSERT_TRUE(pack.load(write(w, "far_none.pack").c_str(), 0, err)) << err;
    const gm20b::PackPipeline &pp = *pack.section<gm20b::PackPipeline>(gm20b::PackPipelines);
    EXPECT_EQ(pack.far(pp), nullptr);
    EXPECT_FALSE(gm20b::SetEntries(pp, nullptr).wide());
}

}  // namespace
