// SPDX-License-Identifier: MIT
// raw/runtime/raw_cache.h and raw_metallib.h on made-up records and a made-up universal metallib (raw/test/cachecheck runs the
// same cache checks on a bucket's real records, and the AIR hashes on QuartzCore's own library).
#include "../raw/runtime/raw_cache.h"
#include "../raw/runtime/raw_metallib.h"
#include "../raw/test/cache_checks.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

raw::RuntimeRecord madeUp(uint32_t n)
{
    raw::RuntimeRecord r;
    r.pipeline.keyHash = 0x1000 + n;
    r.pipeline.vs = 0;
    r.pipeline.fs = 1;
    r.pipeline.entries = 2;
    r.pipeline.entry[0] = {0, raw::EntryUbo, 1, uint8_t(n), 0, 16, 0};
    r.pipeline.entry[1] = {16, raw::EntryTexture, 2, 3, raw::SamplerSlot, 3, 0};
    r.pipeline.pushMask = 1u << (n % 32);
    r.pipeline.pushWords[n % 32] = n;
    r.keyString = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 " + std::to_string(n);
    for (int k = 0; k < 2; k++) {
        r.programs[k].id = std::to_string(n * 2 + k);
        r.programs[k].image.assign(80 + 64 * (n + 1), uint8_t(n + k));
        r.programs[k].cbufTypes = {1, 3};
    }
    r.programs[1].data.assign(256, 0x5A);
    r.programs[1].cbufTypes = {1, 2, 3};
    r.blob = {0x1A00, n, 0x1E08, n + 1};
    if (n % 3 == 0) {
        r.extras = {{0x1340, n}};
        r.restore = {{0x1340, 0}};
        r.samplers.push_back({n, 1, 2, 3, 4, 5, 6, 7});
    }
    return r;
}

void tag(std::vector<uint8_t> &out, const char *fourcc, const void *data, uint16_t length)
{
    out.insert(out.end(), fourcc, fourcc + 4);
    out.push_back(uint8_t(length));
    out.push_back(uint8_t(length >> 8));
    const auto *b = static_cast<const uint8_t *>(data);
    out.insert(out.end(), b, b + length);
}

template <class T> void put(std::vector<uint8_t> &out, size_t at, T v)
{
    memcpy(&out[at], &v, sizeof v);
}

void bigEndian(std::vector<uint8_t> &out, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = uint8_t(v >> (24 - 8 * i));
}

// A universal file of an Apple GPU slice and an AIR64 slice whose function list names `functions`, each HASH a pattern of its
// index
std::vector<uint8_t> universalMetallib(const std::vector<std::string> &functions)
{
    std::vector<uint8_t> slice(0x58);
    memcpy(slice.data(), "MTLB", 4);
    std::vector<uint8_t> list(4);
    put(list, 0, uint32_t(functions.size()));
    for (size_t i = 0; i < functions.size(); i++) {
        std::vector<uint8_t> e(4);
        tag(e, "NAME", functions[i].c_str(), uint16_t(functions[i].size() + 1));
        const uint8_t type = 1;
        tag(e, "TYPE", &type, 1);
        raw::AirHash hash;
        hash.fill(uint8_t(0xA0 + i));
        tag(e, "HASH", hash.data(), uint16_t(hash.size()));
        e.insert(e.end(), {'E', 'N', 'D', 'T'});
        put(e, 0, uint32_t(e.size()));
        list.insert(list.end(), e.begin(), e.end());
    }
    put(slice, 24, uint64_t(slice.size()));
    put(slice, 32, uint64_t(list.size() - 4));
    slice.insert(slice.end(), list.begin(), list.end());
    std::vector<uint8_t> file(0x100, 0);
    bigEndian(file, 0, 0xCAFEBABE);
    bigEndian(file, 4, 2);
    bigEndian(file, 8, 0x01000013);  // an Apple GPU binary first, as the frameworks ship them
    bigEndian(file, 16, 0x100);
    bigEndian(file, 20, 16);
    bigEndian(file, 28, 0x01000017);
    bigEndian(file, 36, 0x110);
    bigEndian(file, 40, uint32_t(slice.size()));
    file.resize(0x110, 0xEE);
    file.insert(file.end(), slice.begin(), slice.end());
    return file;
}

TEST(RawMetallib, ReadsTheAirSlicesFunctionHashes)
{
    const std::vector<std::string> names = {"fixed_vert_lph_spc", "fixed_frag_lph_cph", "downsample_4_frag_lph"};
    const std::vector<uint8_t> file = universalMetallib(names);
    std::map<std::string, raw::AirHash> hashes;
    ASSERT_TRUE(raw::metallibAirHashes(file.data(), file.size(), hashes));
    ASSERT_EQ(hashes.size(), names.size());
    for (size_t i = 0; i < names.size(); i++) {
        raw::AirHash want;
        want.fill(uint8_t(0xA0 + i));
        EXPECT_EQ(hashes[names[i]], want) << names[i];
    }
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/universal.metallib";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    std::map<std::string, raw::AirHash> fromFile;
    ASSERT_TRUE(raw::metallibAirHashes(path, fromFile));
    EXPECT_EQ(fromFile, hashes);
    std::vector<uint8_t> cut(file.begin(), file.end() - 10);
    EXPECT_FALSE(raw::metallibAirHashes(cut.data(), cut.size(), hashes));
    EXPECT_TRUE(hashes.empty());
}

TEST(RawCache, KeepsRecordsAcrossProcessesAndMakesEachStateOnce)
{
    std::vector<std::pair<std::string, raw::RuntimeRecord>> records;
    for (uint32_t n = 0; n < 12; n++)
        records.push_back(
            {"samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v" + std::to_string(n) + " | f f", madeUp(n)});
    cachechecks::run(records, "samples=1 | v two_draws | f f", "2 draws", getenv("TEST_TMPDIR") ?: "/tmp");
    EXPECT_EQ(cachechecks::failures, 0);
}

}  // namespace
