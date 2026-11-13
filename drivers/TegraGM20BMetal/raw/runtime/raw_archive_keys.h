// SPDX-License-Identifier: MIT
// A binary archive of this device is a list of pipeline keys: the fnv64 of each state's key (RawPipelineKey.h for a render
// state, raw_compute_key.h computeArchiveKey for a compute state, archiveTileKey for a tile state), added to it or read from
// its file. What GM20B code a state has is the pack's and the record cache's, which every creation reads whatever archive it
// lists. An archive opened from an Apple file holds the states the pack declares for that file (declaredBy).
//
// serializeToURL: writes "SWBARCH1", u32 key count, u32 0, then the keys ascending, u64 each, little endian.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "raw_archive.h"
#include "raw_pack.h"

namespace raw {

constexpr char kArchiveMagic[8] = {'S', 'W', 'B', 'A', 'R', 'C', 'H', '1'};
constexpr size_t kArchiveHeaderBytes = 16;

class ArchiveKeys {
public:
    void add(uint64_t key)
    {
        keys_.insert(key);
    }

    bool holds(uint64_t key) const
    {
        return keys_.count(key) != 0;
    }

    size_t size() const
    {
        return keys_.size();
    }

    std::vector<uint8_t> serialized() const
    {
        std::vector<uint8_t> bytes(kArchiveHeaderBytes + 8 * keys_.size());
        memcpy(bytes.data(), kArchiveMagic, sizeof kArchiveMagic);
        const uint32_t count = uint32_t(keys_.size());
        memcpy(bytes.data() + 8, &count, sizeof count);
        uint8_t *at = bytes.data() + kArchiveHeaderBytes;
        for (uint64_t key : keys_) {
            memcpy(at, &key, sizeof key);
            at += sizeof key;
        }
        return bytes;
    }

    // false: no file of this format (another magic, or fewer keys than its count says)
    static bool parse(const uint8_t *bytes, size_t length, ArchiveKeys &out)
    {
        out = {};
        if (length < kArchiveHeaderBytes || memcmp(bytes, kArchiveMagic, sizeof kArchiveMagic))
            return false;
        uint32_t count;
        memcpy(&count, bytes + 8, sizeof count);
        if ((length - kArchiveHeaderBytes) / 8 < count)
            return false;
        for (uint32_t i = 0; i < count; i++) {
            uint64_t key;
            memcpy(&key, bytes + kArchiveHeaderBytes + 8 * i, sizeof key);
            out.keys_.insert(key);
        }
        return true;
    }

private:
    std::set<uint64_t> keys_;
};

enum class ArchiveFile : uint8_t {
    Keys,  // this device's (kArchiveMagic)
    Apple,  // a metallib, thin ("MTLB") or universal of AIR64 metallibs and Apple GPU binaries, or a thin Apple GPU binary
    M2v,   // m2v's ("M2VBARCH"): its binaries are m2v's, nothing of this device's
    None,  // no archive: Metal's MTLBinaryArchiveErrorInvalidFile
};

// What metal2vulkan-runtime's binary_archive/container.rs read as an archive, and this device's own format
inline ArchiveFile archiveFile(const uint8_t *bytes, size_t length)
{
    constexpr uint32_t kAir64 = 0x01000017, kUniversal = 0xcafebabe;
    constexpr size_t kSliceBytes = 20, kSlicesMax = 64;
    const auto big = [&](size_t at) {
        return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
    };
    if (length < 8)
        return ArchiveFile::None;
    if (!memcmp(bytes, kArchiveMagic, 8))
        return ArchiveFile::Keys;
    if (m2vArchive(bytes, length))
        return ArchiveFile::M2v;
    if (!memcmp(bytes, "MTLB", 4) || appleGpuBinary(bytes, length))
        return ArchiveFile::Apple;
    const size_t slices = big(4);
    if (big(0) != kUniversal || !slices || slices > kSlicesMax || length - 8 < slices * kSliceBytes)
        return ArchiveFile::None;
    for (size_t i = 0; i < slices; i++) {
        const size_t at = 8 + i * kSliceBytes, offset = big(at + 8), size = big(at + 12);
        if (offset > length || size > length - offset)
            return ArchiveFile::None;
        const uint32_t cpu = big(at);
        if (cpu != kAppleGpuCpuType && !(cpu == kAir64 && size >= 4 && !memcmp(bytes + offset, "MTLB", 4)))
            return ArchiveFile::None;
    }
    return ArchiveFile::Apple;
}

enum class ArchiveAnswer : uint8_t {
    NotAsked,  // no FailOnBinaryArchiveMiss, or no archive listed: the state is made
    Declared,  // a listed archive holds it: made
    Missed,    // nil and Metal's error, never compiled
};

// `holds(i)`: whether listed archive i holds the state
template <typename Holds> ArchiveAnswer archiveAnswer(bool failOnMiss, size_t archives, Holds holds)
{
    if (!failOnMiss || !archives)
        return ArchiveAnswer::NotAsked;
    for (size_t i = 0; i < archives; i++)
        if (holds(i))
            return ArchiveAnswer::Declared;
    return ArchiveAnswer::Missed;
}

// The pack's list of the states the Apple archive file at `path` declares for this GPU (pack/archives.py keys it by the
// file's inventory path); nullptr where the pack lists no such archive: it declares nothing
inline const PackArchive *declaredBy(const Pack &pack, const std::string &path)
{
    const std::string listed = inventoryPath(path);
    return pack.archive(Pack::fnv(listed.data(), listed.size()));
}

// A tile state's: its function, constants, colour formats and sample count, as its descriptor names them
inline std::string archiveTileKey(const std::string &function, const std::string &constants,
                                  const std::vector<uint32_t> &formats, uint32_t samples, bool matchesTile)
{
    std::string key = "t " + function + (constants.empty() ? "" : " " + constants) + " |";
    for (uint32_t f : formats)
        key += " " + std::to_string(f);
    return key + " s=" + std::to_string(samples) + " m=" + (matchesTile ? "1" : "0");
}

}  // namespace raw
