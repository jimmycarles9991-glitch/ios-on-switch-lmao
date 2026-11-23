// SPDX-License-Identifier: MIT
// gm20b_cache.h's installed native records: packed as the seed build packs them (kits/SwitchSeedPack), read in
// place before the process's own directory, refused whole when another build's or damaged.
#include "../gm20b_cache.h"
#include "../cache_io.h"
#include "../program_pages.h"
#include "../harness/cache_checks.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedPackWriter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

namespace fs = std::filesystem;

std::string hex16(uint64_t value)
{
    char text[17];
    snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::string bigEndian(uint64_t value)
{
    std::string bytes;
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes += char(value >> shift);
    return bytes;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The records under <from>/raw-<stack> packed into <to>/raw-<stack> for `identity`, as the seed build does
void packRecords(const fs::path &from, const fs::path &to, uint64_t stack, uint64_t identity,
                 std::vector<fs::path> files = {})
{
    if (files.empty())
        for (const auto &file : fs::directory_iterator(from / ("rec-" + hex16(stack))))
            if (file.path().extension() == ".rec")
                files.push_back(file.path());
    std::vector<switch_seed_pack::Entry> entries;
    for (const auto &file : files)
        entries.push_back({bigEndian(std::stoull(file.stem().string(), nullptr, 16)), 0, file.string(), ""});
    uint8_t named[switch_seed_pack::kIdentityBytes] = {};
    memcpy(named, &identity, sizeof identity);
    std::string error;
    fs::create_directories(to);
    ASSERT_TRUE(switch_seed_pack::write((to / ("rec-" + hex16(stack))).string(), 8, named, entries, error)) << error;
}

gm20b::RuntimeRecord record(uint32_t n)
{
    gm20b::RuntimeRecord r;
    r.keyString = "native state " + std::to_string(n);
    r.programs[0].id = std::to_string(n);
    r.programs[0].image.assign(96 + 16 * n, uint8_t(n));
    r.programs[1].data.assign(64, 0x5a);
    r.blob = {0x1a00, n};
    return r;
}

class NativeSeed : public testing::Test {
protected:
    fs::path root, staged, seed, local;
    gm20b::CacheKey key = cachechecks::keyOf(0x123, "native state", 7);
    gm20b::RuntimeRecord installed = record(4);

    void SetUp() override
    {
        std::string pattern = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/native-seed-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        staged = root / "staged";
        seed = root / "seed";
        local = root / "private";
        fs::create_directories(staged);
        fs::create_directories(local);
        gm20b::RecordCache builder;
        ASSERT_TRUE(builder.open(staged.string(), key.stack, 0));
        ASSERT_TRUE(builder.store(key, &installed, ""));
        for (uint32_t n = 10; n < 200; ++n)
            ASSERT_TRUE(builder.store(cachechecks::keyOf(key.stack, "other " + std::to_string(n), 9), &installed, ""));
        packRecords(staged, seed, key.stack, key.stack);
    }

    void TearDown() override
    {
        if (!root.empty())
            fs::remove_all(root);
    }

    fs::path pack(uint32_t number) const
    {
        return switch_seed_pack::packPath((seed / ("rec-" + hex16(key.stack))).string(), number);
    }
};

TEST_F(NativeSeed, ReadOnlyConsumerLoadsWithoutPrivateCacheAndCannotStore)
{
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.attachSeed(seed.string(), key.stack));
    gm20b::RuntimeRecord loaded;
    std::string why;
    EXPECT_EQ(cache.load(key, loaded, why), gm20b::RecordCache::Found::Record);
    EXPECT_TRUE(cachechecks::same(loaded, installed));
    EXPECT_FALSE(cache.store(key, &installed, ""));
    cache.tidy();
    EXPECT_EQ(cache.bytes(), 0u);
    EXPECT_EQ(cache.load(key, loaded, why), gm20b::RecordCache::Found::Record);
    EXPECT_EQ(cache.load(cachechecks::keyOf(key.stack, "unseeded", 7), loaded, why), gm20b::RecordCache::Found::None);
}

TEST_F(NativeSeed, SeedWinsOverPrivateRecordAndDamagedSeedRecordFallsBack)
{
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.open(local.string(), key.stack, 0));
    const auto privateRecord = record(1);
    ASSERT_TRUE(cache.store(key, &privateRecord, ""));
    ASSERT_TRUE(cache.attachSeed(seed.string(), key.stack));
    gm20b::RuntimeRecord loaded;
    std::string why;
    EXPECT_EQ(cache.load(key, loaded, why), gm20b::RecordCache::Found::Record);
    EXPECT_TRUE(cachechecks::same(loaded, installed));
    const std::string entry = gm20b::encodeEntry(key, &installed, "");
    for (uint32_t number = 0; number < 64 && fs::exists(pack(number)); ++number) {
        // Damaged in place at the same length: a mapped pack cut short would fault its reader
        std::string bytes = readFile(pack(number));
        for (size_t at = bytes.find(entry); at != std::string::npos; at = bytes.find(entry, at + 1))
            bytes[at + entry.size() / 2] ^= 1;
        std::fstream(pack(number), std::ios::binary | std::ios::in | std::ios::out).write(bytes.data(), bytes.size());
    }
    EXPECT_EQ(cache.load(key, loaded, why), gm20b::RecordCache::Found::Record);
    EXPECT_TRUE(cachechecks::same(loaded, privateRecord));
}

TEST_F(NativeSeed, AnotherBuildsOrCutShortSeedIsRefusedWhole)
{
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.open(local.string(), key.stack, 0));
    const auto privateRecord = record(2);
    ASSERT_TRUE(cache.store(key, &privateRecord, ""));
    gm20b::RuntimeRecord loaded;
    std::string why;
    fs::remove_all(seed);
    packRecords(staged, seed, key.stack, key.stack + 1);
    EXPECT_FALSE(cache.attachSeed(seed.string(), key.stack));
    fs::remove_all(seed);
    packRecords(staged, seed, key.stack, key.stack);
    fs::resize_file(pack(0), fs::file_size(pack(0)) - 1);
    EXPECT_FALSE(cache.attachSeed(seed.string(), key.stack));
    const auto index = seed / ("rec-" + hex16(key.stack)) / "index";
    for (const auto &bytes : {std::string("SWSEEDPK"), readFile(index).substr(0, 100)}) {
        std::ofstream(index, std::ios::binary | std::ios::trunc) << bytes;
        EXPECT_FALSE(cache.attachSeed(seed.string(), key.stack));
    }
    EXPECT_FALSE(cache.attachSeed((root / "nothing").string(), key.stack));
    EXPECT_EQ(cache.load(key, loaded, why), gm20b::RecordCache::Found::Record);
    EXPECT_TRUE(cachechecks::same(loaded, privateRecord));
}

TEST_F(NativeSeed, ExactStackAirAndKeyAreRequired)
{
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.attachSeed(seed.string(), key.stack));
    gm20b::RuntimeRecord loaded;
    std::string why;
    for (auto other : {cachechecks::keyOf(key.stack + 1, key.key, 7), cachechecks::keyOf(key.stack, key.key, 8),
                       cachechecks::keyOf(key.stack, key.key + "x", 7)})
        EXPECT_EQ(cache.load(other, loaded, why), gm20b::RecordCache::Found::None);
    ASSERT_TRUE(cache.open(local.string(), key.stack + 1, 0));
    EXPECT_FALSE(cache.attachSeed(seed.string(), key.stack));
    EXPECT_EQ(cache.load(key, loaded, why), gm20b::RecordCache::Found::None);
}

TEST_F(NativeSeed, ComputeRecordLoadsFromTheSameImmutableLayer)
{
    key.key = "kernel | explicit dispatch shape";
    gm20b::ComputeRecord original;
    original.keyString = key.key;
    original.code.assign(128, 0x42);
    original.cbufs = 1;
    gm20b::RecordCache builder;
    ASSERT_TRUE(builder.open(staged.string(), key.stack, 0));
    ASSERT_TRUE(builder.storeKernel(key, &original, ""));
    packRecords(staged, seed, key.stack, key.stack);
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.attachSeed(seed.string(), key.stack));
    gm20b::ComputeRecord loaded;
    std::string why;
    EXPECT_EQ(cache.loadKernel(key, loaded, why), gm20b::RecordCache::Found::Record);
    EXPECT_EQ(loaded.code, original.code);
    EXPECT_EQ(loaded.keyString, original.keyString);
}

// A program slot the seed holds as an entry of its own (native_seed_manifest.h writeSlots) is found by its bytes on a page
// boundary of its pack's mapping, the prefetch's zeros after it, for the device to import and run in place
TEST_F(NativeSeed, AProgramSlotIsFoundInPlaceOnPagesOfItsOwn)
{
    std::vector<uint8_t> slot(20000);
    for (size_t i = 0; i < slot.size(); ++i)
        slot[i] = uint8_t(i * 7 + 1);
    std::vector<uint8_t> entry = slot;
    entry.resize(gm20b::programSlotEntryBytes(slot.size()));
    const fs::path records = staged / ("rec-" + hex16(key.stack));
    const fs::path file = records / (hex16(gm20b::seedSlotName(slot.data(), slot.size())) + ".slot");
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char *>(entry.data()), std::streamsize(entry.size()));
    std::vector<fs::path> files = {file};
    for (const auto &f : fs::directory_iterator(records))
        if (f.path().extension() == ".rec")
            files.push_back(f.path());
    packRecords(staged, seed, key.stack, key.stack, files);
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.attachSeed(seed.string(), key.stack));
    const uint8_t *mapping = nullptr;
    uint64_t bytes = 0;
    const uint8_t *found = cache.findSeedSlot(slot.data(), slot.size(), mapping, bytes);
    ASSERT_NE(found, nullptr);
    ASSERT_TRUE(found >= mapping && found + entry.size() <= mapping + bytes);
    EXPECT_EQ(uint64_t(found - mapping) % gm20b::kProgramSlotPage, 0u);
    EXPECT_EQ(memcmp(found, slot.data(), slot.size()), 0);
    EXPECT_TRUE(std::all_of(found + slot.size(), found + entry.size(), [](uint8_t b) {
        return !b;
    }));
    gm20b::RuntimeRecord loaded;
    std::string why;
    EXPECT_EQ(cache.load(key, loaded, why), gm20b::RecordCache::Found::Record);
    slot[100] ^= 1;
    EXPECT_EQ(cache.findSeedSlot(slot.data(), slot.size(), mapping, bytes), nullptr);
}

// A handful of a built seed's own records (SWITCH_SEED_TREE: an image's ngpu/addons, per-file native records), packed
// and read back through the cache to the same bytes
TEST(BuiltNativeSeed, RecordsReadBackUnchanged)
{
    const char *tree = getenv("SWITCH_SEED_TREE");
    const fs::path native = fs::path(tree ? tree : "") / "TegraGM20BMetal.pack" / "native";
    if (!tree || !fs::is_directory(native))
        GTEST_SKIP() << "no SWITCH_SEED_TREE with per-file native records";
    fs::path records;
    for (const auto &entry : fs::directory_iterator(native))
        if (entry.is_directory() && entry.path().filename().string().rfind("rec-", 0) == 0)
            records = entry.path();
    ASSERT_FALSE(records.empty());
    const uint64_t stack = std::stoull(records.filename().string().substr(4), nullptr, 16);
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(records))
        if (entry.path().extension() == ".rec")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::vector<fs::path> sample;
    for (size_t i = 0; i < files.size() && sample.size() < 48; i += std::max<size_t>(1, files.size() / 48))
        sample.push_back(files[i]);
    const fs::path out = fs::path(getenv("TEST_TMPDIR") ?: "/tmp") / "built-native-seed";
    fs::remove_all(out);
    packRecords(native, out, stack, stack, sample);
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.attachSeed(out.string(), stack));
    for (const auto &file : sample) {
        const std::string bytes = readFile(file);
        gm20b::Reader reader(bytes);
        char magic[sizeof gm20b::kEntryMagic];
        reader.bytes(magic, sizeof magic);
        reader.value<uint32_t>();
        const uint32_t kind = reader.value<uint32_t>();
        gm20b::CacheKey k;
        k.stack = reader.value<uint64_t>();
        reader.bytes(k.vertexAir, sizeof k.vertexAir);
        reader.bytes(k.fragmentAir, sizeof k.fragmentAir);
        k.key = reader.text();
        ASSERT_TRUE(reader.ok()) << file;
        std::string why;
        if (kind == gm20b::kKindKernel) {
            gm20b::ComputeRecord loaded;
            ASSERT_EQ(cache.loadKernel(k, loaded, why), gm20b::RecordCache::Found::Record) << file;
            EXPECT_EQ(gm20b::encodeKernelEntry(k, &loaded, ""), bytes) << file;
        } else {
            gm20b::RuntimeRecord loaded;
            ASSERT_EQ(cache.load(k, loaded, why), gm20b::RecordCache::Found::Record) << file;
            EXPECT_EQ(gm20b::encodeEntry(k, &loaded, ""), bytes) << file;
        }
    }
    fs::remove_all(out);
}

}  // namespace
