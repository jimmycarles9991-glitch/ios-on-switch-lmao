// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include "../NativeMetalArtifactCache.h"

namespace {
namespace cache = native_artifact_cache;

class ArtifactCache : public testing::Test {
protected:
    std::string directory;
    cache::Key key{};
    std::string payload = std::string("native analysis") + '\0' + "bytes";

    void SetUp() override
    {
        char path[] = "/tmp/switch-native-artifact-XXXXXX";
        ASSERT_NE(mkdtemp(path), nullptr);
        directory = path;
        for (unsigned i = 0; i < key.size(); ++i)
            key[i] = uint8_t(i * 7);
    }

    void TearDown() override
    {
        std::filesystem::remove_all(directory);
    }

    bool save()
    {
        return cache::store(directory, key, payload.data(), payload.size());
    }

    std::string path() const
    {
        return directory + "/" + cache::name(key);
    }

    std::string contents()
    {
        std::ifstream input(path(), std::ios::binary);
        return {std::istreambuf_iterator<char>(input), {}};
    }

    void replace(const std::string &bytes)
    {
        std::ofstream output(path(), std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), std::streamsize(bytes.size()));
    }
};

TEST_F(ArtifactCache, ExactIdentityRoundTripsWithMappingLifetime)
{
    ASSERT_TRUE(save());
    cache::Mapping mapped;
    ASSERT_TRUE(mapped.load(directory, key));
    EXPECT_EQ(std::string(static_cast<const char *>(mapped.data()), mapped.size()), payload);
    cache::Mapping moved(std::move(mapped));
    EXPECT_EQ(mapped.size(), 0u);
    EXPECT_EQ(mapped.data(), nullptr);
    EXPECT_EQ(std::string(static_cast<const char *>(moved.data()), moved.size()), payload);
    EXPECT_FALSE(moved.load(directory, cache::Key{}));
    EXPECT_EQ(moved.size(), 0u);
}

TEST_F(ArtifactCache, CorruptHeaderAndPayloadNeverBecomeHits)
{
    ASSERT_TRUE(save());
    const auto original = contents();
    for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(16), size_t(24), size_t(56), cache::kHeaderBytes}) {
        auto corrupt = original;
        corrupt[offset] ^= 1;
        replace(corrupt);
        cache::Mapping mapped;
        EXPECT_FALSE(mapped.load(directory, key)) << offset;
    }
    replace(original + 'x');
    cache::Mapping mapped;
    EXPECT_FALSE(mapped.load(directory, key));
    for (size_t size : {size_t(0), cache::kHeaderBytes, original.size() - 1}) {
        replace(original.substr(0, size));
        EXPECT_FALSE(mapped.load(directory, key)) << size;
    }
}

TEST_F(ArtifactCache, RenamedForeignKeyAndSymlinkAreRefused)
{
    ASSERT_TRUE(save());
    cache::Key foreign = key;
    foreign[31] ^= 1;
    const auto renamed = directory + "/" + cache::name(foreign);
    ASSERT_EQ(rename(path().c_str(), renamed.c_str()), 0);
    cache::Mapping mapped;
    EXPECT_FALSE(mapped.load(directory, foreign));
    ASSERT_EQ(symlink(renamed.c_str(), path().c_str()), 0);
    EXPECT_FALSE(mapped.load(directory, key));
    EXPECT_FALSE(cache::store(directory, key, nullptr, 1));
    EXPECT_FALSE(cache::store(directory, key, payload.data(), 0));
    EXPECT_FALSE(cache::store(directory, key, payload.data(), cache::kMaxPayloadBytes + 1));
}

TEST_F(ArtifactCache, AtomicReplacementKeepsExistingMappingImmutable)
{
    ASSERT_TRUE(save());
    cache::Mapping old;
    ASSERT_TRUE(old.load(directory, key));
    const std::string replacement(4096, 'r');
    ASSERT_TRUE(cache::store(directory, key, replacement.data(), replacement.size()));
    cache::Mapping current;
    ASSERT_TRUE(current.load(directory, key));
    EXPECT_EQ(std::string(static_cast<const char *>(old.data()), old.size()), payload);
    EXPECT_EQ(std::string(static_cast<const char *>(current.data()), current.size()), replacement);
}

TEST_F(ArtifactCache, ConcurrentWritersPublishWholeRecords)
{
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i)
        threads.emplace_back([&, i] {
            const std::string body(2048 + i, char('a' + i));
            for (unsigned j = 0; j < 64; ++j) {
                if (!cache::store(directory, key, body.data(), body.size())) {
                    ++failures;
                    continue;
                }
                cache::Mapping mapped;
                if (!mapped.load(directory, key)) {
                    ++failures;
                    continue;
                }
                const auto *bytes = static_cast<const char *>(mapped.data());
                if (mapped.size() < 2048 || mapped.size() > 2055 || bytes[0] < 'a' || bytes[0] > 'h' ||
                    mapped.size() != size_t(2048 + bytes[0] - 'a') ||
                    !std::all_of(bytes, bytes + mapped.size(), [&](char c) {
                        return c == bytes[0];
                    }))
                    ++failures;
            }
        });
    for (auto &thread : threads)
        thread.join();
    EXPECT_EQ(failures, 0u);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}
}  // namespace
