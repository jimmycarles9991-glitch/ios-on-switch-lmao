// SPDX-License-Identifier: MIT
#include "../SwitchSeedPackWriter.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

namespace {

namespace pack = switch_seed_pack;

std::string readFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void writeFile(const std::filesystem::path &path, const std::string &bytes)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

class SeedPack : public testing::Test {
protected:
    std::filesystem::path root;
    uint8_t identity[pack::kIdentityBytes] = {};
    std::vector<pack::Entry> entries;

    void SetUp() override
    {
        std::string pattern = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/seed-pack-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        identity[0] = 0x53;
        std::mt19937_64 random(7);
        for (unsigned i = 0; i < 300; ++i) {
            pack::Entry entry;
            for (unsigned b = 0; b < 8; ++b)
                entry.name += char(random());
            entry.tier = i % 5 == 0 ? 0 : 1;
            entry.bytes = "entry " + std::to_string(i) + "|" + std::string(random() % 900, char('a' + i % 26));
            entries.push_back(entry);
        }
    }

    void TearDown() override
    {
        std::filesystem::remove_all(root);
    }

    std::string write(const std::string &name, std::vector<pack::Entry> list, uint64_t bucket = 8192)
    {
        std::string error;
        const std::string directory = (root / name).string();
        EXPECT_TRUE(pack::write(directory, 8, identity, std::move(list), error, bucket)) << error;
        return directory;
    }

    std::string found(const pack::Seed &seed, const std::string &name)
    {
        pack::Bytes bytes;
        if (!seed.find(reinterpret_cast<const uint8_t *>(name.data()), bytes))
            return "<missing>";
        return std::string(reinterpret_cast<const char *>(bytes.data), bytes.size);
    }
};

TEST_F(SeedPack, EveryEntryReadsBackAndOthersMiss)
{
    const auto directory = write("seed", entries);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    EXPECT_GT(seed.packs(), 2u);
    for (const auto &entry : entries)
        EXPECT_EQ(found(seed, entry.name), entry.bytes);
    for (auto entry : entries) {
        entry.name[7] ^= 0x40;
        EXPECT_EQ(found(seed, entry.name), "<missing>");
    }
    EXPECT_EQ(found(seed, std::string(8, '\0')), "<missing>");
    EXPECT_EQ(found(seed, std::string(8, '\xff')), "<missing>");
}

TEST_F(SeedPack, FirstLastAndSharedPrefixRunsAreFound)
{
    std::vector<pack::Entry> edges;
    for (const char *name : {"\x00\x00\x00\x00\x00\x00\x00\x00", "\xff\xff\xff\xff\xff\xff\xff\xff"})
        edges.push_back({std::string(name, 8), 0, "", std::string(name, 1) + "edge"});
    for (unsigned i = 0; i < 40; ++i)
        edges.push_back(
            {std::string("\x80\x00\x00\x01", 4) + std::string(3, '\0') + char(i), 1, "", std::to_string(i)});
    edges.insert(edges.end(), entries.begin(), entries.begin() + 20);
    const auto directory = write("edges", edges);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    for (const auto &entry : edges)
        EXPECT_EQ(found(seed, entry.name), entry.bytes);
    EXPECT_EQ(found(seed, std::string("\x80\x00\x00\x01", 4) + std::string(3, '\0') + char(40)), "<missing>");
}

TEST_F(SeedPack, SameEntriesMakeTheSameBytesInAnyOrder)
{
    const auto first = write("first", entries);
    std::reverse(entries.begin(), entries.end());
    const auto second = write("second", entries);
    std::vector<std::string> a, b;
    for (const auto &file : std::filesystem::recursive_directory_iterator(first))
        if (file.is_regular_file())
            a.push_back(file.path().lexically_relative(first).string() + "=" + readFile(file.path()));
    for (const auto &file : std::filesystem::recursive_directory_iterator(second))
        if (file.is_regular_file())
            b.push_back(file.path().lexically_relative(second).string() + "=" + readFile(file.path()));
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    EXPECT_EQ(a, b);
}

TEST_F(SeedPack, ObservedEntriesFillTheFirstPacksAndOneMoreEntryRewritesOnePack)
{
    const auto directory = write("tiers", entries);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    ASSERT_GT(seed.observedPacks(), 0u);
    ASSERT_LT(seed.observedPacks(), seed.packs());
    std::vector<std::string> packs;
    for (uint32_t number = 0; number < seed.packs(); ++number)
        packs.push_back(readFile(pack::packPath(directory, number)));
    for (const auto &entry : entries) {
        const auto holder = std::find_if(packs.begin(), packs.end(), [&](const std::string &contents) {
            return contents.find(entry.bytes) != std::string::npos;
        });
        ASSERT_NE(holder, packs.end());
        EXPECT_EQ(uint32_t(holder - packs.begin()) < seed.observedPacks(), entry.tier == 0);
    }
    auto more = entries;
    more.push_back({std::string("\x40\x00\x00\x00\x00\x00\x00\x01", 8), 1, "", "one more"});
    const auto grown = write("grown", more);
    unsigned changed = 0;
    for (uint32_t number = 0; number < seed.packs(); ++number)
        changed += readFile(pack::packPath(directory, number)) != readFile(pack::packPath(grown, number));
    EXPECT_EQ(changed, 1u);
}

TEST_F(SeedPack, AnotherLayoutIdentityOrTruncationRefusesTheSeed)
{
    const auto directory = write("refused", entries);
    const auto index = readFile(directory + "/index");
    pack::Seed seed;
    uint8_t other[pack::kIdentityBytes] = {};
    EXPECT_FALSE(seed.open(directory, 8, other));
    EXPECT_FALSE(seed.open(directory, 32, identity));
    EXPECT_TRUE(seed.open(directory, 8, nullptr));
    for (size_t offset : {0, 8, 12, 16, 20, 24, 28, 32}) {
        auto corrupt = index;
        corrupt[offset] ^= 0x10;
        writeFile(directory + "/index", corrupt);
        EXPECT_FALSE(seed.open(directory, 8, identity)) << offset;
        EXPECT_FALSE(seed.opened());
    }
    for (size_t size : {size_t(0), pack::kHeaderBytes - 1, index.size() - 1}) {
        writeFile(directory + "/index", index.substr(0, size));
        EXPECT_FALSE(seed.open(directory, 8, identity)) << size;
    }
    writeFile(directory + "/index", index + "x");
    EXPECT_FALSE(seed.open(directory, 8, identity));
    writeFile(directory + "/index", index);
    const auto packed = readFile(pack::packPath(directory, 1));
    writeFile(pack::packPath(directory, 1), packed.substr(0, packed.size() - 1));
    EXPECT_FALSE(seed.open(directory, 8, identity));
    writeFile(pack::packPath(directory, 1), packed);
    EXPECT_TRUE(seed.open(directory, 8, identity));
    std::filesystem::remove(directory + "/index");
    EXPECT_FALSE(seed.open(directory, 8, identity));
    EXPECT_EQ(found(seed, entries[0].name), "<missing>");
}

TEST_F(SeedPack, AnUnstagedPackLeavesOnlyItsEntriesMissing)
{
    const auto directory = write("partial", entries);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    const uint32_t packs = seed.packs();
    seed.close();
    std::filesystem::remove(pack::packPath(directory, packs - 1));
    ASSERT_TRUE(seed.open(directory, 8, identity));
    EXPECT_EQ(seed.packs(), packs);
    unsigned missing = 0;
    for (const auto &entry : entries) {
        const auto bytes = found(seed, entry.name);
        missing += bytes == "<missing>";
        EXPECT_TRUE(bytes == entry.bytes || bytes == "<missing>");
    }
    EXPECT_GT(missing, 0u);
    EXPECT_LT(missing, entries.size());
}

TEST_F(SeedPack, ObservedPacksAloneServeTheObservedEntries)
{
    const auto directory = write("observed", entries);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    const uint32_t observed = seed.observedPacks(), packs = seed.packs();
    seed.close();
    for (uint32_t number = observed; number < packs; ++number)
        std::filesystem::remove(pack::packPath(directory, number));
    ASSERT_TRUE(seed.open(directory, 8, identity));
    EXPECT_EQ(seed.packs(), packs);
    for (const auto &entry : entries)
        EXPECT_EQ(found(seed, entry.name), entry.tier == 0 ? entry.bytes : "<missing>");
}

TEST_F(SeedPack, TheFirstAbsentDerivedPackLeavesEveryDerivedEntryMissing)
{
    const auto directory = write("derived", entries);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    const uint32_t observed = seed.observedPacks();
    ASSERT_GE(seed.packs() - observed, 2u);
    seed.close();
    std::filesystem::remove(pack::packPath(directory, observed));
    ASSERT_TRUE(seed.open(directory, 8, identity));
    for (const auto &entry : entries)
        EXPECT_EQ(found(seed, entry.name), entry.tier == 0 ? entry.bytes : "<missing>");
}

TEST_F(SeedPack, AnAbsentObservedPackLeavesTheOtherPacksRead)
{
    const auto directory = write("unobserved", entries);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    ASSERT_GE(seed.observedPacks(), 2u);
    seed.close();
    std::filesystem::remove(pack::packPath(directory, 0));
    ASSERT_TRUE(seed.open(directory, 8, identity));
    unsigned missing = 0;
    for (const auto &entry : entries) {
        const auto bytes = found(seed, entry.name);
        missing += bytes == "<missing>";
        EXPECT_TRUE(bytes == entry.bytes || (bytes == "<missing>" && entry.tier == 0));
    }
    EXPECT_GT(missing, 0u);
}

TEST_F(SeedPack, EntryBoundsOutsideTheirPackMiss)
{
    std::vector<pack::Entry> one = {{std::string(8, '\x11'), 0, "", "payload"}};
    const auto directory = write("bounds", one);
    auto index = readFile(directory + "/index");
    const size_t entry = index.size() - pack::kEntryTailBytes;
    index[entry + 4] = '\x7f';
    writeFile(directory + "/index", index);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    EXPECT_EQ(found(seed, one[0].name), "<missing>");
}

TEST_F(SeedPack, AnEmptySeedOpensAndFindsNothing)
{
    const auto directory = write("empty", {});
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    EXPECT_EQ(seed.packs(), 0u);
    EXPECT_EQ(found(seed, entries[0].name), "<missing>");
}

TEST_F(SeedPack, MisnamedOrDuplicateEntriesAreNotWritten)
{
    std::string error;
    auto twice = entries;
    twice.push_back(entries[3]);
    EXPECT_FALSE(pack::write((root / "twice").string(), 8, identity, twice, error));
    EXPECT_FALSE(pack::write((root / "short").string(), 9, identity, entries, error));
    auto untiered = entries;
    untiered[0].tier = 2;
    EXPECT_FALSE(pack::write((root / "untiered").string(), 8, identity, untiered, error));
    auto unreadable = entries;
    unreadable[0].path = (root / "nothing").string();
    EXPECT_FALSE(pack::write((root / "unreadable").string(), 8, identity, unreadable, error));
}

// An entry of whole pages starts on a page of its pack and shares none of its pages: a GPU client imports them and runs
// the program slot they hold in place (raw_program_pages.h)
TEST_F(SeedPack, AnEntryOfWholePagesHasItsPagesToItself)
{
    std::vector<pack::Entry> list = {entries[0], entries[1], entries[2]};
    list[1].bytes = std::string(2 * pack::kPageBytes, 'p');
    const std::string directory = write("pages", list, uint64_t(1) << 30);
    pack::Seed seed;
    ASSERT_TRUE(seed.open(directory, 8, identity));
    pack::Bytes found[3], in;
    for (unsigned i = 0; i < 3; ++i)
        ASSERT_TRUE(seed.find(reinterpret_cast<const uint8_t *>(list[i].name.data()), found[i]));
    ASSERT_TRUE(seed.packOf(found[1].data, in));
    EXPECT_EQ((found[1].data - in.data) % pack::kPageBytes, 0);
    for (unsigned i : {0u, 2u}) {
        const bool before = found[i].data + found[i].size <= found[1].data;
        const bool after = found[i].data >= found[1].data + found[1].size;
        EXPECT_TRUE(before || after) << i;
        EXPECT_EQ(std::string(reinterpret_cast<const char *>(found[i].data), found[i].size), list[i].bytes);
    }
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(found[1].data), found[1].size), list[1].bytes);
}

}  // namespace
