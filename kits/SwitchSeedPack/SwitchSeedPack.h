// SPDX-License-Identifier: MIT
// A read-only seed of many small files kept as a few large ones, in one directory: `index` and `packs/<NNN>`, each
// mapped once when the seed opens (the descriptors closed again). A lookup is then a run table and a binary search in
// memory, and a hit is a pointer into a pack: no system call either way. SwitchSeedPackWriter.h writes them.
//
//   index: magic "SWSEEDPK" | version u32 | name bytes u32 | entries u32 | packs u32 | observed packs u32
//          | prefix bits k u32 | identity [32]                                                          (64 bytes)
//          | per pack: its length u64
//          | 2^k + 1 run starts u32: the entries whose names begin with the k bits r are [start r, start r+1)
//          | per entry, sorted by name: name [name bytes] | pack u32 | offset u32 | length u32       (little endian)
//
// Names are hashes, so their leading bits spread the entries evenly over the runs. The identity is what the entries were
// made for (the caller's: a stack key, a build); a seed of another identity does not open. Packs [0, observed packs)
// hold only entries a device was seen to use. A pack that is not there leaves its entries missing; one there whose
// length is not the index's refuses the whole seed. A stage stages the packs from observed packs on all or none, so the
// first of them not there ends the opening: a seed staged with only its observed packs costs one failed open, not one
// per pack.
#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace switch_seed_pack {

constexpr char kMagic[8] = {'S', 'W', 'S', 'E', 'E', 'D', 'P', 'K'};
constexpr uint32_t kVersion = 1, kMaxPrefixBits = 24, kMinNameBytes = 4, kMaxNameBytes = 64;
constexpr size_t kHeaderBytes = 64, kIdentityBytes = 32, kEntryTailBytes = 12;
// The page an entry of whole pages starts on (SwitchSeedPackWriter.h), SwitchSeedStore.h's kStorePageBytes
constexpr uint64_t kPageBytes = 16384;

inline uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

inline uint64_t le64(const uint8_t *p)
{
    return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32;
}

inline uint32_t be32(const uint8_t *p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

inline std::string packPath(const std::string &directory, uint32_t pack)
{
    char name[24];
    snprintf(name, sizeof name, "/packs/%03u", pack);
    return directory + name;
}

struct Bytes {
    const uint8_t *data = nullptr;
    size_t size = 0;
};

// The whole file at `path` mapped read only and shared; an empty file maps to nothing. `absent`: there is no such file.
inline bool mapFile(const std::string &path, Bytes &out, bool &absent)
{
    out = {};
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    absent = fd < 0 && errno == ENOENT;
    if (fd < 0)
        return false;
    struct stat info = {};
    bool ok = !fstat(fd, &info) && S_ISREG(info.st_mode);
    if (ok && info.st_size > 0) {
        void *mapped = mmap(nullptr, size_t(info.st_size), PROT_READ, MAP_SHARED, fd, 0);
        ok = mapped != MAP_FAILED;
        if (ok)
            out = {static_cast<const uint8_t *>(mapped), size_t(info.st_size)};
    }
    ::close(fd);
    return ok;
}

inline void unmap(Bytes &bytes)
{
    if (bytes.data)
        munmap(const_cast<uint8_t *>(bytes.data), bytes.size);
    bytes = {};
}

// Opened before any reader starts and closed after the last: find() reads only what open() mapped.
class Seed {
public:
    Seed() = default;
    Seed(const Seed &) = delete;
    Seed &operator=(const Seed &) = delete;

    ~Seed()
    {
        close();
    }

    // `identity`: kIdentityBytes the index must name, or null for any
    bool open(const std::string &directory, uint32_t nameBytes, const uint8_t *identity)
    {
        close();
        bool absent = false;
        if (!mapFile(directory + "/index", index_, absent) || !parse(nameBytes, identity)) {
            close();
            return false;
        }
        packs_.resize(le32(index_.data + 20));
        const uint32_t observed = observedPacks();
        for (uint32_t pack = 0; pack < packs_.size(); ++pack) {
            const bool mapped = mapFile(packPath(directory, pack), packs_[pack], absent);
            if ((!mapped && !absent) || (mapped && packs_[pack].size != le64(index_.data + kHeaderBytes + 8 * pack))) {
                close();
                return false;
            }
            if (absent && pack >= observed)
                break;
        }
        return true;
    }

    void close()
    {
        for (Bytes &pack : packs_)
            unmap(pack);
        packs_.clear();
        unmap(index_);
        entries_ = 0;
    }

    bool opened() const
    {
        return index_.data != nullptr;
    }

    uint32_t packs() const
    {
        return uint32_t(packs_.size());
    }

    uint32_t observedPacks() const
    {
        return opened() ? le32(index_.data + 24) : 0;
    }

    // The bytes kept under `name` (the index's name bytes long), in place in their pack
    bool find(const uint8_t *name, Bytes &out) const
    {
        if (!opened())
            return false;
        const uint32_t run = bits_ ? be32(name) >> (32 - bits_) : 0;
        uint32_t low = std::min(le32(runs_ + 4 * run), entries_),
                 high = std::min(le32(runs_ + 4 * (run + 1)), entries_);
        const size_t stride = nameBytes_ + kEntryTailBytes;
        while (low < high) {
            const uint32_t middle = low + (high - low) / 2;
            const uint8_t *entry = table_ + stride * middle;
            const int order = memcmp(entry, name, nameBytes_);
            if (order < 0) {
                low = middle + 1;
            } else if (order > 0) {
                high = middle;
            } else {
                const uint32_t pack = le32(entry + nameBytes_), offset = le32(entry + nameBytes_ + 4),
                               length = le32(entry + nameBytes_ + 8);
                if (pack >= packs_.size() || !packs_[pack].data || offset > packs_[pack].size ||
                    length > packs_[pack].size - offset)
                    return false;
                out = {packs_[pack].data + offset, length};
                return true;
            }
        }
        return false;
    }

    // The pack mapping `at` lies in (a find's bytes): its first byte and size; false: none of this seed's
    bool packOf(const uint8_t *at, Bytes &pack) const
    {
        for (const Bytes &p : packs_)
            if (p.data && at >= p.data && at < p.data + p.size) {
                pack = p;
                return true;
            }
        return false;
    }

private:
    Bytes index_;
    std::vector<Bytes> packs_;
    const uint8_t *runs_ = nullptr, *table_ = nullptr;
    uint32_t nameBytes_ = 0, entries_ = 0, bits_ = 0;

    bool parse(uint32_t nameBytes, const uint8_t *identity)
    {
        const uint8_t *header = index_.data;
        if (index_.size < kHeaderBytes || memcmp(header, kMagic, sizeof kMagic) || le32(header + 8) != kVersion ||
            le32(header + 12) != nameBytes || nameBytes < kMinNameBytes || nameBytes > kMaxNameBytes)
            return false;
        const uint64_t entries = le32(header + 16), packs = le32(header + 20), bits = le32(header + 28);
        if (bits > kMaxPrefixBits || le32(header + 24) > packs ||
            (identity && memcmp(header + 32, identity, kIdentityBytes)))
            return false;
        const uint64_t runs = (uint64_t(1) << bits) + 1;
        const uint64_t table = kHeaderBytes + 8 * packs + 4 * runs;
        if (table + entries * (nameBytes + kEntryTailBytes) != index_.size)
            return false;
        runs_ = header + kHeaderBytes + 8 * packs;
        table_ = header + table;
        nameBytes_ = nameBytes;
        entries_ = uint32_t(entries);
        bits_ = uint32_t(bits);
        return le32(runs_ + 4 * (runs - 1)) == entries_;
    }
};

}  // namespace switch_seed_pack
