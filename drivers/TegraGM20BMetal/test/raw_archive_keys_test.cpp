// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "../raw/runtime/raw_archive_keys.h"
#include "../raw/runtime/raw_compute_key.h"

namespace {
using raw::ArchiveAnswer;
using raw::ArchiveKeys;

TEST(ArchiveKeys, WhatItWritesItReadsBack)
{
    ArchiveKeys keys;
    for (uint64_t key : {0x9e3779b97f4a7c15ull, 1ull, 0ull, 1ull})
        keys.add(key);
    const std::vector<uint8_t> bytes = keys.serialized();
    ASSERT_EQ(bytes.size(), raw::kArchiveHeaderBytes + 3 * 8);
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data()), 8), "SWBARCH1");
    ArchiveKeys read;
    ASSERT_TRUE(ArchiveKeys::parse(bytes.data(), bytes.size(), read));
    EXPECT_EQ(read.size(), 3u);
    for (uint64_t key : {0x9e3779b97f4a7c15ull, 1ull, 0ull})
        EXPECT_TRUE(read.holds(key));
    EXPECT_FALSE(read.holds(2));
}

TEST(ArchiveKeys, AFileOfAnotherFormatOrCutShortIsNone)
{
    ArchiveKeys keys;
    keys.add(7);
    std::vector<uint8_t> bytes = keys.serialized();
    ArchiveKeys read;
    EXPECT_FALSE(ArchiveKeys::parse(bytes.data(), bytes.size() - 1, read));
    EXPECT_FALSE(ArchiveKeys::parse(bytes.data(), 8, read));
    bytes[0] = 'M';
    EXPECT_FALSE(ArchiveKeys::parse(bytes.data(), bytes.size(), read));
    const uint8_t m2v[16] = {'M', '2', 'V', 'B', 'A', 'R', 'C', 'H'};
    EXPECT_FALSE(ArchiveKeys::parse(m2v, sizeof m2v, read));
}

std::vector<uint8_t> universal(std::vector<std::pair<uint32_t, std::vector<uint8_t>>> slices)
{
    std::vector<uint8_t> bytes;
    const auto big = [&](uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(uint8_t(v >> shift));
    };
    big(0xcafebabe);
    big(uint32_t(slices.size()));
    uint32_t offset = uint32_t(8 + 20 * slices.size());
    for (const auto &[cpu, body] : slices) {
        big(cpu);
        big(0);
        big(offset);
        big(uint32_t(body.size()));
        big(0);
        offset += uint32_t(body.size());
    }
    for (const auto &slice : slices)
        bytes.insert(bytes.end(), slice.second.begin(), slice.second.end());
    return bytes;
}

TEST(ArchiveFile, EveryFormatM2vsReaderTookOpensAndNothingElse)
{
    const std::vector<uint8_t> metallib = {'M', 'T', 'L', 'B', 1, 0, 2, 0};
    const std::vector<uint8_t> gpu = {0xcf, 0xfa, 0xed, 0xfe, 0x13, 0, 0, 0x01};
    const auto file = [](const std::vector<uint8_t> &b) {
        return raw::archiveFile(b.data(), b.size());
    };
    EXPECT_EQ(file(metallib), raw::ArchiveFile::Apple);
    EXPECT_EQ(file(gpu), raw::ArchiveFile::Apple);
    EXPECT_EQ(file(universal({{0x01000017, metallib}, {0x01000013, gpu}})), raw::ArchiveFile::Apple);
    EXPECT_EQ(file(universal({{0x01000017, gpu}})), raw::ArchiveFile::None);
    EXPECT_EQ(file(universal({{0x0100000c, gpu}})), raw::ArchiveFile::None);
    std::vector<uint8_t> cut = universal({{0x01000013, gpu}});
    cut.resize(cut.size() - 1);
    EXPECT_EQ(file(cut), raw::ArchiveFile::None);
    EXPECT_EQ(file(ArchiveKeys().serialized()), raw::ArchiveFile::Keys);
    EXPECT_EQ(file({'M', '2', 'V', 'B', 'A', 'R', 'C', 'H'}), raw::ArchiveFile::M2v);
    EXPECT_EQ(file({'n', 'o', ' ', 'a', 'r', 'c', 'h', 'i', 'v', 'e'}), raw::ArchiveFile::None);
    EXPECT_EQ(file({'M', 'T', 'L'}), raw::ArchiveFile::None);
}

TEST(ArchiveAnswer, OnlyAskedUnderTheOptionWithArchivesListed)
{
    const auto none = [](size_t) {
        return false;
    };
    EXPECT_EQ(raw::archiveAnswer(false, 2, none), ArchiveAnswer::NotAsked);
    EXPECT_EQ(raw::archiveAnswer(true, 0, none), ArchiveAnswer::NotAsked);
    EXPECT_EQ(raw::archiveAnswer(true, 2, none), ArchiveAnswer::Missed);
}

// gpu.raw.archive_miss on T-v1: HW Test's own state, held by the device, was made under an empty archive, where Metal
// (and an M4 Max's AGX driver) answers nil with its error
TEST(ArchiveAnswer, OnlyAListedArchiveHoldingTheStateIsAHit)
{
    EXPECT_EQ(raw::archiveAnswer(true, 1,
                                 [](size_t) {
                                     return false;
                                 }),
              ArchiveAnswer::Missed);
    EXPECT_EQ(raw::archiveAnswer(true, 3,
                                 [](size_t i) {
                                     return i == 2;
                                 }),
              ArchiveAnswer::Declared);
}

TEST(ArchiveKeys, ComputeKeysAreThePacks)
{
    EXPECT_EQ(raw::computeArchiveKey("CoreImage ci_kernel", "", 1024, true), "k CoreImage ci_kernel | max=1024 tgm=1");
    EXPECT_EQ(raw::computeArchiveKey("f", "c0=1", 0, false), "k f c0=1 | max=0 tgm=0");
}

uint64_t fnv(const std::string &s)
{
    return raw::Pack::fnv(s.data(), s.size());
}

// A pack whose archive list names `path` (pack/archives.py: the fnv64 of its inventory path) with `keys`, ascending
std::string writePackDeclaring(const std::string &path, std::vector<uint64_t> keys)
{
    std::sort(keys.begin(), keys.end());
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    const uint32_t counts[2] = {1, uint32_t(keys.size())};
    const raw::PackArchive archive = {fnv(path), 0, uint32_t(keys.size())};
    h.section[raw::PackArchives] = {0x200, uint32_t(sizeof counts + sizeof archive + 8 * keys.size())};
    memcpy(file.data(), &h, sizeof h);
    memcpy(file.data() + 0x200, counts, sizeof counts);
    memcpy(file.data() + 0x200 + sizeof counts, &archive, sizeof archive);
    memcpy(file.data() + 0x200 + sizeof counts + sizeof archive, keys.data(), 8 * keys.size());
    const std::string out = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/declaring.bin";
    FILE *f = fopen(out.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return out;
}

// An archive opened from an Apple file, through whichever mount, holds what the pack declares for that file and nothing else
TEST(ArchiveKeys, AnAppleArchiveHoldsWhatThePackDeclaresForItsFile)
{
    const std::string render = "a render state's RawPipelineKey";
    const std::string compute = raw::computeArchiveKey("QuartzCore kernel", "", 256, false);
    raw::Pack pack;
    std::string err;
    ASSERT_TRUE(pack.load(writePackDeclaring("/System/Library/Frameworks/QuartzCore.framework/default.metallib",
                                             {fnv(render), fnv(compute)})
                              .c_str(),
                          0, err))
        << err;
    const raw::PackArchive *declared =
        raw::declaredBy(pack, "/System/Cryptexes/OS/System/Library/Frameworks/QuartzCore.framework/default.metallib");
    ASSERT_NE(declared, nullptr);
    const auto answer = [&](const std::string &key) {
        return raw::archiveAnswer(true, 1, [&](size_t) {
            return pack.declares(*declared, fnv(key));
        });
    };
    EXPECT_EQ(answer(render), ArchiveAnswer::Declared);
    EXPECT_EQ(answer(compute), ArchiveAnswer::Declared);
    EXPECT_EQ(answer("another state"), ArchiveAnswer::Missed);
    EXPECT_EQ(raw::declaredBy(pack, "/System/Library/Frameworks/RenderBox.framework/archive.metallib"), nullptr);
}

TEST(ArchiveKeys, TileKeysNameFormatsSamplesAndTheTileMatch)
{
    EXPECT_EQ(raw::archiveTileKey("lib tile", "", {80, 0}, 1, true), "t lib tile | 80 0 s=1 m=1");
    EXPECT_NE(raw::archiveTileKey("lib tile", "", {80, 0}, 4, true),
              raw::archiveTileKey("lib tile", "", {80, 0}, 1, true));
}

}  // namespace
