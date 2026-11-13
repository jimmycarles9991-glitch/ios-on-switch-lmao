// SPDX-License-Identifier: MIT
// Writes the seed SwitchSeedPack.h reads. The same entries always make the same bytes. Each tier (0: observed,
// 1: derived) gets packs of its own, about kBucketBytes each, and an entry's pack within its tier is picked by its
// name's leading bits: adding or changing one entry rewrites one pack and the index, not every pack after it. An entry
// of a whole number of kPageBytes pages starts on such a page and has its pages to itself, as SwitchSeedStore.h places
// one: a GPU client imports those pages and runs the program slot they hold in place.
#pragma once
#include "SwitchSeedPack.h"
#include <sys/stat.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace switch_seed_pack {

constexpr uint64_t kBucketBytes = uint64_t(12) << 20;
constexpr uint32_t kTiers = 2, kAlignment = 8, kEntriesPerRun = 4;

struct Entry {
    std::string name;
    uint32_t tier = 0;
    std::string path;  // the file holding its bytes; `bytes` when empty
    std::string bytes;
};

inline bool readWhole(const std::string &path, std::string &out)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat info = {};
    bool ok = !fstat(fd, &info) && S_ISREG(info.st_mode);
    out.resize(ok ? size_t(info.st_size) : 0);
    for (size_t at = 0; ok && at < out.size();) {
        const ssize_t got = read(fd, &out[at], out.size() - at);
        ok = got > 0;
        at += ok ? size_t(got) : 0;
    }
    ::close(fd);
    return ok;
}

// Written beside, then renamed over: a reader still mapping the old file keeps it whole.
inline bool replaceFile(const std::string &path, const std::string &bytes)
{
    const std::string temporary = path + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    bool ok = !fchmod(fd, 0644);
    for (size_t at = 0; ok && at < bytes.size();) {
        const ssize_t put = ::write(fd, bytes.data() + at, bytes.size() - at);
        ok = put > 0;
        at += ok ? size_t(put) : 0;
    }
    ok = !::close(fd) && ok && !rename(temporary.c_str(), path.c_str());
    if (!ok)
        unlink(temporary.c_str());
    return ok;
}

inline void putLe(std::string &out, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        out += char(value >> (8 * i));
}

inline uint64_t alignmentOf(uint64_t length)
{
    return length && !(length % kPageBytes) ? kPageBytes : kAlignment;
}

inline uint32_t prefixBits(size_t entries)
{
    uint32_t bits = 0;
    while (bits < kMaxPrefixBits && (size_t(1) << bits) <= entries / kEntriesPerRun)
        ++bits;
    return bits;
}

// The seed of `entries` in `directory` (made if missing): false, with `error`, when an entry cannot be read, its name is
// not `nameBytes` long, two share a name, its tier is unknown, or a pack would pass 4 GiB
inline bool write(const std::string &directory, uint32_t nameBytes, const uint8_t *identity, std::vector<Entry> entries,
                  std::string &error, uint64_t bucketBytes = kBucketBytes)
{
    if (!bucketBytes)
        return error = "no bucket size", false;
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.name < b.name;
    });

    struct Placed {
        uint64_t length = 0;
        uint32_t pack = 0, offset = 0;
    };

    std::vector<Placed> placed(entries.size());
    uint64_t tierBytes[kTiers] = {};
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry &entry = entries[i];
        struct stat info = {};
        if (entry.name.size() != nameBytes || nameBytes < kMinNameBytes || nameBytes > kMaxNameBytes ||
            entry.tier >= kTiers || (i && entry.name == entries[i - 1].name))
            return error = "a misnamed, duplicate or untiered entry", false;
        if (!entry.path.empty() && (stat(entry.path.c_str(), &info) || !S_ISREG(info.st_mode)))
            return error = "unreadable: " + entry.path, false;
        placed[i].length = entry.path.empty() ? entry.bytes.size() : uint64_t(info.st_size);
        if (placed[i].length > UINT32_MAX)
            return error = "an entry past 4 GiB: " + entry.path, false;
        tierBytes[entry.tier] += (placed[i].length + alignmentOf(placed[i].length) - 1) / kAlignment * kAlignment;
    }
    uint32_t tierPacks[kTiers] = {}, tierFirst[kTiers] = {}, packs = 0;
    for (uint32_t tier = 0; tier < kTiers; ++tier) {
        tierPacks[tier] = uint32_t((tierBytes[tier] + bucketBytes - 1) / bucketBytes);
        tierFirst[tier] = packs;
        packs += tierPacks[tier];
    }
    std::vector<std::vector<size_t>> members(packs);
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint32_t tier = entries[i].tier;
        const auto *name = reinterpret_cast<const uint8_t *>(entries[i].name.data());
        placed[i].pack = tierFirst[tier] + uint32_t(uint64_t(be32(name)) * tierPacks[tier] >> 32);
        members[placed[i].pack].push_back(i);
    }
    if ((mkdir(directory.c_str(), 0755) && errno != EEXIST) ||
        (mkdir((directory + "/packs").c_str(), 0755) && errno != EEXIST))
        return error = "cannot make " + directory, false;
    std::string index(kMagic, sizeof kMagic), pack, bytes;
    putLe(index, kVersion, 4);
    putLe(index, nameBytes, 4);
    putLe(index, entries.size(), 4);
    putLe(index, packs, 4);
    putLe(index, tierPacks[0], 4);
    const uint32_t bits = prefixBits(entries.size());
    putLe(index, bits, 4);
    index.append(reinterpret_cast<const char *>(identity), kIdentityBytes);
    for (uint32_t number = 0; number < packs; ++number) {
        pack.clear();
        for (size_t i : members[number]) {
            if (!(entries[i].path.empty() ? (bytes = entries[i].bytes, true) : readWhole(entries[i].path, bytes)) ||
                bytes.size() != placed[i].length)
                return error = "unreadable: " + entries[i].path, false;
            const uint64_t align = alignmentOf(bytes.size());
            pack.resize((pack.size() + align - 1) / align * align);
            if (pack.size() + bytes.size() > UINT32_MAX)
                return error = "a pack past 4 GiB", false;
            placed[i].offset = uint32_t(pack.size());
            pack += bytes;
            pack.resize((pack.size() + kAlignment - 1) / kAlignment * kAlignment);
        }
        if (!replaceFile(packPath(directory, number), pack))
            return error = "cannot write " + packPath(directory, number), false;
        putLe(index, pack.size(), 8);
    }
    for (uint32_t number = packs; !unlink(packPath(directory, number).c_str()); ++number) {
    }
    size_t at = 0;
    for (uint64_t run = 0; run <= (uint64_t(1) << bits); ++run) {
        while (bits && at < entries.size() &&
               be32(reinterpret_cast<const uint8_t *>(entries[at].name.data())) >> (32 - bits) < run)
            ++at;
        putLe(index, run == (uint64_t(1) << bits) ? entries.size() : bits ? at : 0, 4);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        index += entries[i].name;
        putLe(index, placed[i].pack, 4);
        putLe(index, placed[i].offset, 4);
        putLe(index, placed[i].length, 4);
    }
    if (!replaceFile(directory + "/index", index))
        return error = "cannot write " + directory + "/index", false;
    return true;
}

}  // namespace switch_seed_pack
