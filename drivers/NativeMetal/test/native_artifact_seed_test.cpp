// SPDX-License-Identifier: MIT
// NativeComputeArtifactStorage.h's two tiers: the process's own artifact files, then the seed's packed envelopes.
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "../NativeComputeArtifactStorage.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedPackWriter.h"

namespace {
namespace fs = std::filesystem;
namespace cache = native_artifact_cache;

std::string gImported;
NmbComputeArtifact *const kImported = reinterpret_cast<NmbComputeArtifact *>(uintptr_t(0x10));

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The envelopes cache::store would write, packed under their keys into `directory`
void packEnvelopes(const fs::path &directory, const std::vector<std::pair<cache::Key, std::string>> &payloads)
{
    const fs::path staged = directory.string() + ".staged";
    std::vector<switch_seed_pack::Entry> entries;
    for (const auto &[key, payload] : payloads) {
        ASSERT_TRUE(cache::store(staged.string(), key, payload.data(), payload.size()));
        entries.push_back({std::string(key.begin(), key.end()), 0, (staged / cache::name(key)).string(), ""});
    }
    const uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
    std::string error;
    ASSERT_TRUE(switch_seed_pack::write(directory.string(), 32, identity, entries, error)) << error;
}

class ArtifactSeed : public testing::Test {
protected:
    fs::path root;
    cache::Key key{}, other{};

    void SetUp() override
    {
        std::string pattern = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/artifact-seed-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        for (unsigned i = 0; i < key.size(); ++i) {
            key[i] = uint8_t(i * 7);
            other[i] = uint8_t(255 - i);
        }
        gImported.clear();
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    std::string loaded(const NativeComputeArtifactStorage &storage, const cache::Key &which)
    {
        gImported.clear();
        static const char unread = 0;
        const auto &descriptor = *reinterpret_cast<const NmbComputePipelineDescriptor *>(&unread);
        return storage.load(nullptr, descriptor, false, which) == kImported ? gImported : "<missing>";
    }
};

TEST_F(ArtifactSeed, OwnCacheFirstThenTheSeed)
{
    packEnvelopes(root / "seed", {{key, "seeded analysis"}, {other, "only seeded"}});
    NativeComputeArtifactStorage storage;
    storage.cache = (root / "cache").string();
    storage.seed = (root / "seed").string();
    ASSERT_TRUE(cache::store(storage.cache, key, "own analysis", 12));
    EXPECT_EQ(loaded(storage, key), "own analysis");
    EXPECT_EQ(loaded(storage, other), "only seeded");
    std::string damaged = readFile(storage.cache + "/" + cache::name(key));
    damaged.back() ^= 1;
    std::ofstream(storage.cache + "/" + cache::name(key), std::ios::binary | std::ios::trunc) << damaged;
    EXPECT_EQ(loaded(storage, key), "seeded analysis");
    cache::Key unknown = key;
    unknown[0] ^= 1;
    EXPECT_EQ(loaded(storage, unknown), "<missing>");
}

TEST_F(ArtifactSeed, SeedEnvelopeHeaderAndKeyAreCheckedNotItsPayloadDigest)
{
    packEnvelopes(root / "seed", {{key, "seeded analysis"}});
    const auto pack = switch_seed_pack::packPath((root / "seed").string(), 0);
    const std::string original = readFile(pack);
    cache::Seed seed;
    const void *payload = nullptr;
    size_t bytes = 0;
    for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(16), size_t(24)}) {
        std::string damaged = original;
        damaged[offset] ^= 1;
        std::ofstream(pack, std::ios::binary | std::ios::trunc) << damaged;
        ASSERT_TRUE(seed.open((root / "seed").string()));
        EXPECT_FALSE(seed.find(key, payload, bytes)) << offset;
    }
    std::string payloadDamage = original;
    payloadDamage[cache::kHeaderBytes] ^= 1;
    std::ofstream(pack, std::ios::binary | std::ios::trunc) << payloadDamage;
    ASSERT_TRUE(seed.open((root / "seed").string()));
    ASSERT_TRUE(seed.find(key, payload, bytes));
    EXPECT_EQ(std::string(static_cast<const char *>(payload), bytes), "reeded analysis");
}

TEST_F(ArtifactSeed, NoSeedOrAnotherLayoutIsNoTier)
{
    NativeComputeArtifactStorage none;
    EXPECT_EQ(loaded(none, key), "<missing>");
    NativeComputeArtifactStorage absent;
    absent.seed = (root / "nothing").string();
    EXPECT_EQ(loaded(absent, key), "<missing>");
    fs::create_directories(root / "records");
    std::vector<switch_seed_pack::Entry> records = {{std::string(8, '\1'), 0, "", "a record"}};
    const uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
    std::string error;
    ASSERT_TRUE(switch_seed_pack::write((root / "records").string(), 8, identity, records, error));
    cache::Seed seed;
    EXPECT_FALSE(seed.open((root / "records").string()));
}

// A handful of a built seed's own analyses (SWITCH_SEED_TREE: an image's ngpu/addons with per-file analyses), packed
// and found again with the same payloads
TEST(BuiltArtifactSeed, AnalysesReadBackUnchanged)
{
    const char *tree = getenv("SWITCH_SEED_TREE");
    const fs::path analyses = fs::path(tree ? tree : "") / "TegraGM20BMetal.m2v-seed/shared/native-analysis";
    if (!tree || !fs::is_directory(analyses))
        GTEST_SKIP() << "no SWITCH_SEED_TREE with per-file native analyses";
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(analyses))
        if (entry.path().extension() == ".nca")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    std::vector<switch_seed_pack::Entry> entries;
    std::vector<cache::Key> keys;
    for (size_t i = 0; i < files.size() && entries.size() < 48; i += std::max<size_t>(1, files.size() / 48)) {
        cache::Key k{};
        const std::string stem = files[i].stem().string();
        for (size_t b = 0; b < k.size(); ++b)
            k[b] = uint8_t(std::stoul(stem.substr(2 * b, 2), nullptr, 16));
        keys.push_back(k);
        entries.push_back({std::string(k.begin(), k.end()), 0, files[i].string(), ""});
    }
    const fs::path out = fs::path(getenv("TEST_TMPDIR") ?: "/tmp") / "built-artifact-seed";
    fs::remove_all(out);
    const uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
    std::string error;
    ASSERT_TRUE(switch_seed_pack::write(out.string(), 32, identity, entries, error)) << error;
    cache::Seed seed;
    ASSERT_TRUE(seed.open(out.string()));
    for (size_t i = 0; i < keys.size(); ++i) {
        const void *payload = nullptr;
        size_t bytes = 0;
        ASSERT_TRUE(seed.find(keys[i], payload, bytes)) << entries[i].path;
        EXPECT_EQ(std::string(static_cast<const char *>(payload), bytes),
                  readFile(entries[i].path).substr(cache::kHeaderBytes))
            << entries[i].path;
    }
    fs::remove_all(out);
}

}  // namespace

extern "C" NmbComputeArtifact *nmb_compute_artifact_import(NmbDevice *, const NmbComputePipelineDescriptor *, bool,
                                                           const void *bytes, size_t length)
{
    gImported.assign(static_cast<const char *>(bytes), length);
    return kImported;
}

extern "C" bool nmb_compute_artifact_serialize(const NmbComputeArtifact *, bool (*)(const void *, size_t, void *),
                                               void *)
{
    return false;
}
