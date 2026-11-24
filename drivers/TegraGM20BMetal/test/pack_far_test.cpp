// SPDX-License-Identifier: MIT
// A pack record past kPackEntries set-0 entries reads the rest from its far block in the blob section, and a pack program
// its uniform buffers past set-0 byte 496 from its far table there (pack version 27). A record or program pointing past the
// blob section, or a record past every argument a render pipeline binds, is named by the generator's scan (pack_scan.h):
// the pack's load reads its header alone.
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>
#include "../pack.h"
#include "../pack_scan.h"
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

bool scanned(const gm20b::Pack &pack, const std::string &line)
{
    const std::vector<std::string> found = gm20b::packScan(pack);
    return std::find(found.begin(), found.end(), line) != found.end();
}

TEST(PackFar, AFarBlockPastTheBlobSectionIsTheScansNotTheLoads)
{
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.farAt = 64 - gm20b::farWords(40) + 1;
    ASSERT_TRUE(pack.load(write(w, "far_past.pack").c_str(), 0, err)) << err;
    EXPECT_TRUE(scanned(pack, "pipeline 0000000000000007: its far entries past the blob section"));
}

TEST(PackFar, ARecordPastEveryArgumentOfARenderPipelineIsTheScansNotTheLoads)
{
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.entries = gm20b::kSetEntriesMax + 1;
    w.blobWords = 1024;
    ASSERT_TRUE(pack.load(write(w, "far_bound.pack").c_str(), 0, err)) << err;
    EXPECT_TRUE(scanned(pack, "pipeline 0000000000000007: of more set-0 entries than a record holds"));
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

TEST(PackFar, AProgramsFarTablePastTheBlobSectionIsTheScansNotTheLoads)
{
    gm20b::Pack pack;
    std::string err;
    WidePack w;
    w.programFar = 60;
    ASSERT_TRUE(pack.load(write(w, "far_ubos_past.pack").c_str(), 0, err)) << err;
    EXPECT_TRUE(scanned(pack, "program 0000000000000000: its far uniform buffers past the blob section"));
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

// A pack of records of `keys` in that order, and `fast` as its PackFastKeys section; the first `launchRecords` records and
// `launchKeys` fast keys tier 0's
std::string writeKeys(const std::vector<uint64_t> &keys, const std::vector<gm20b::PackFastKey> &fast, const char *name,
                      uint64_t launchRecords = 0, uint64_t launchKeys = 0)
{
    const uint64_t fastAt = (kPipelines + keys.size() * sizeof(gm20b::PackPipeline) + 255) & ~uint64_t(255);
    std::vector<uint8_t> file(fastAt + std::max<size_t>(fast.size() * sizeof(gm20b::PackFastKey), 0x100), 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.pipelines = uint32_t(keys.size());
    h.section[gm20b::PackPipelines] = {kPipelines, keys.size() * sizeof(gm20b::PackPipeline)};
    h.section[gm20b::PackFastKeys] = {fastAt, fast.size() * sizeof(gm20b::PackFastKey)};
    h.tier0[gm20b::PackPipelines] = launchRecords * sizeof(gm20b::PackPipeline);
    h.tier0[gm20b::PackFastKeys] = launchKeys * sizeof(gm20b::PackFastKey);
    memcpy(file.data(), &h, sizeof h);
    for (size_t i = 0; i < keys.size(); i++) {
        gm20b::PackPipeline pp = {};
        pp.keyHash = keys[i];
        pp.fs = gm20b::kNoProgram;
        memcpy(file.data() + kPipelines + i * sizeof pp, &pp, sizeof pp);
    }
    memcpy(file.data() + fastAt, fast.data(), fast.size() * sizeof(gm20b::PackFastKey));
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(PackFastKeys, TheScanNamesRecordsOutOfOrderAndFastKeysOffTheirRecords)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(
        pack.load(writeKeys({9, 7, 7}, {{12, 1, 1}, {11, 0, 1}, {13, 2, 5}}, "fast_unordered.pack").c_str(), 0, err))
        << err;
    EXPECT_TRUE(scanned(pack, "pipeline 0000000000000007: out of keyHash order"));
    EXPECT_TRUE(scanned(pack, "fast key 000000000000000c: not its state's records alone and all of them"));
    EXPECT_TRUE(scanned(pack, "fast key 000000000000000b: out of order"));
    EXPECT_TRUE(scanned(pack, "fast key 000000000000000d: its records past the pipelines"));
}

bool scannedWith(const gm20b::Pack &pack, const char *words)
{
    const std::vector<std::string> found = gm20b::packScan(pack);
    return std::any_of(found.begin(), found.end(), [&](const std::string &line) {
        return line.find(words) != std::string::npos;
    });
}

// Tier 0's records and fast keys are in key order, and so are tier 1's after them: a state of either tier is found, and
// one of tier 0 by reading tier 0's part alone
TEST(PackTiers, AStateIsFoundInEitherTier)
{
    gm20b::Pack pack;
    std::string err;
    const std::vector<gm20b::PackFastKey> fast = {{20, 0, 1}, {21, 1, 1}, {5, 2, 2}, {8, 4, 1}};
    ASSERT_TRUE(pack.load(writeKeys({3, 9, 2, 2, 7}, fast, "tiers.pack", 2, 2).c_str(), 0, err)) << err;
    const gm20b::PackPipeline *p = pack.section<gm20b::PackPipeline>(gm20b::PackPipelines);
    for (const auto &[key, first, records] :
         std::vector<std::tuple<uint64_t, uint32_t, uint32_t>>{{3, 0, 1}, {9, 1, 1}, {2, 2, 2}, {7, 4, 1}}) {
        const gm20b::PackRun run = pack.run(key);
        EXPECT_EQ(run.first, p + first) << key;
        EXPECT_EQ(run.records, records) << key;
    }
    for (const gm20b::PackFastKey &k : fast) {
        const gm20b::PackRun run = pack.fastRun(k.fast);
        EXPECT_EQ(run.first, p + k.first) << k.fast;
        EXPECT_EQ(run.records, k.records) << k.fast;
    }
    EXPECT_EQ(pack.run(4).records, 0u);
    EXPECT_EQ(pack.fastRun(6).records, 0u);
    EXPECT_FALSE(scannedWith(pack, "out of"));
    EXPECT_FALSE(scannedWith(pack, "another tier"));
}

TEST(PackTiers, TheScanNamesAFastKeyOfAnotherTiersRecordsAndOrderBrokenWithinATier)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(
        pack.load(writeKeys({9, 3, 7}, {{20, 2, 1}, {5, 0, 1}, {8, 1, 1}}, "tiers_bad.pack", 2, 1).c_str(), 0, err))
        << err;
    EXPECT_TRUE(scanned(pack, "pipeline 0000000000000003: out of keyHash order"));
    EXPECT_TRUE(scanned(pack, "fast key 0000000000000014: its records of another tier than its own"));
    EXPECT_TRUE(scanned(pack, "fast key 0000000000000005: its records of another tier than its own"));
}

// A record of the launch set and its program, which stores to memory (an STG in its code) where `facts` says so or not, in
// tier 0's part of the heap or past it
std::string writeProgram(uint32_t facts, uint64_t launchHeap, const char *name)
{
    constexpr uint64_t kHeap = 2 * gm20b::kPackTierAlign;
    std::vector<uint8_t> file(kHeap + gm20b::kPackTierAlign, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.pipelines = h.programs = 1;
    h.section[gm20b::PackPipelines] = {kPipelines, sizeof(gm20b::PackPipeline)};
    h.section[gm20b::PackPrograms] = {kPrograms, sizeof(gm20b::PackProgram)};
    h.section[gm20b::PackHeap] = {kHeap, gm20b::kPackTierAlign};
    h.tier0[gm20b::PackPipelines] = sizeof(gm20b::PackPipeline);
    h.tier0[gm20b::PackPrograms] = sizeof(gm20b::PackProgram);
    h.tier0[gm20b::PackHeap] = launchHeap;
    memcpy(file.data(), &h, sizeof h);
    gm20b::PackPipeline pp = {};
    pp.keyHash = 7;
    pp.fs = gm20b::kNoProgram;
    memcpy(file.data() + kPipelines, &pp, sizeof pp);
    gm20b::PackProgram program = {};
    program.program = 0x30;
    program.bytes = 0x200;
    program.facts = facts;
    memcpy(file.data() + kPrograms, &program, sizeof program);
    const uint64_t stg = uint64_t(0xeed8) << 48;
    memcpy(file.data() + kHeap + 0x80 + 8, &stg, sizeof stg);
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

TEST(PackTiers, TheScanNamesAProgramWhoseFactsAreNotItsCodesAndOneTheLaunchSetDrawsPastTierZero)
{
    gm20b::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writeProgram(0, 0, "facts_wrong.pack").c_str(), 0, err)) << err;
    EXPECT_TRUE(scanned(pack, "program 0000000000000000: its facts are not its code's"));
    EXPECT_TRUE(scanned(pack, "program 0000000000000000: drawn by the launch set, outside tier 0"));
    gm20b::Pack right;
    ASSERT_TRUE(right.load(writeProgram(gm20b::kProgramWritesMemory, gm20b::kPackTierAlign, "facts_right.pack").c_str(),
                           0, err))
        << err;
    EXPECT_FALSE(scannedWith(right, "facts"));
    EXPECT_FALSE(scannedWith(right, "outside tier 0"));
    EXPECT_FALSE(scannedWith(right, "tier boundary"));
}

}  // namespace
