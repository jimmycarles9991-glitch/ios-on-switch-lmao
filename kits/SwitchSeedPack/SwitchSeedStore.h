// SPDX-License-Identifier: MIT
// The append-only sibling of SwitchSeedPack.h: one file of fixed size that a single writer adds entries to while readers,
// which mapped it once, read in place. The file is made at its whole size (sparse) and never grows, so no reader remaps,
// and a lookup is a probe of the slot table in memory: no system call.
//
//   header (kStoreHeaderBytes): magic "SWSTORE1" | version u32 | name bytes u32 | slots u32 (a power of two) | 0 u32
//          | data offset u64 | data bytes u64 | identity [32] | 2 x (durable end u64, generation u64)
//          | writer's boot [kStoreBootBytes] (little endian)
//   slots, from kStoreHeaderBytes: per slot offset u64 | length u32 | 0 u32 | name [kStoreNameBytes]
//   data, from data offset: the entries' bytes, each at a kStoreAlignment boundary; an entry whose length is a whole
//          number of kStorePageBytes pages starts on such a page and has its pages to itself (a GPU client imports
//          those pages and runs what they hold in place, so nothing is written into them once they are published)
//
// An entry lands in this order: its bytes, then its slot's length and name, then the slot's offset, stored with release;
// a reader loads the offset with acquire and reads nothing of a slot whose offset is 0. No file is ever cut short while it
// may be mapped: a store made again is made beside the old one and renamed over it. A writer that died midway leaves no
// more than a slot whose offset is 0 (empty to readers, taken again by the next writer) and bytes past the last published
// entry (the next writer starts past the published ones). Within one boot every mapping reads the same page cache, so a
// published entry is whole to every reader. A panic or power cut can leave a slot on the medium without its bytes (pages
// are written back in any order), so persist() syncs every published entry before it moves the durable end past them,
// and a writer opening the store in a later boot retires each entry that ends past that mark. Nothing reads an entry's
// bytes to decide it is whole, so nothing may write a page that holds bytes below the mark once it is set: data is
// only ever appended at or past the mark rounded up to a page, and a persist moves the next append to a page of its
// own. The mark is written into whichever of its two slots holds the older one, so a write torn inside it leaves a mark
// no newer than the last whole one (the newer generation is read, and its end only grows). A slot is never emptied again: a retired one holds kRetired, which a probe steps over.
// Names are hashes: their first bytes pick the slot a probe starts at.
#pragma once
#include "SwitchSeedPack.h"
#include <sys/file.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace switch_seed_pack {

constexpr char kStoreMagic[8] = {'S', 'W', 'S', 'T', 'O', 'R', 'E', '1'};
constexpr uint32_t kStoreVersion = 2, kStoreNameBytes = 32, kStoreAlignment = 8;
constexpr uint64_t kStorePageBytes = 16384;
constexpr size_t kStoreHeaderBytes = 4096, kStoreSlotBytes = 16 + kStoreNameBytes;
constexpr size_t kStoreMarksAt = 72, kStoreMarkBytes = 16, kStoreBootAt = 104, kStoreBootBytes = 48;
constexpr uint64_t kRetired = ~uint64_t(0);

// kern.bootsessionuuid, which every boot draws anew; false where the system gives none
inline bool currentBoot(char boot[kStoreBootBytes])
{
    memset(boot, 0, kStoreBootBytes);
    size_t bytes = kStoreBootBytes - 1;
    return sysctlbyname("kern.bootsessionuuid", boot, &bytes, nullptr, 0) == 0 && boot[0] != 0;
}

// fsync that also orders the medium: writes issued after it land after what it synced (APFS's F_BARRIERFSYNC; the kernel
// promotes it to F_FULLFSYNC where the file system has no barrier)
inline bool syncBarrier(int fd)
{
#ifdef F_BARRIERFSYNC
    if (fcntl(fd, F_BARRIERFSYNC) == 0)
        return true;
#endif
    return fsync(fd) == 0;
}

inline uint64_t loadAcquire(const uint8_t *word)
{
    return __atomic_load_n(reinterpret_cast<const uint64_t *>(word), __ATOMIC_ACQUIRE);
}

// The layout a header names, checked against the mapping it sits in
struct StoreLayout {
    uint32_t slots = 0;
    uint64_t dataOffset = 0, dataBytes = 0;

    static uint64_t dataOffsetFor(uint32_t slots)
    {
        const uint64_t end = kStoreHeaderBytes + uint64_t(slots) * kStoreSlotBytes;
        return (end + 16383) & ~uint64_t(16383);
    }

    bool parse(const uint8_t *header, size_t mapped, const uint8_t *identity)
    {
        if (mapped < kStoreHeaderBytes || memcmp(header, kStoreMagic, sizeof kStoreMagic) ||
            le32(header + 8) != kStoreVersion || le32(header + 12) != kStoreNameBytes ||
            (identity && memcmp(header + 40, identity, kIdentityBytes)))
            return false;
        slots = le32(header + 16);
        dataOffset = le64(header + 24);
        dataBytes = le64(header + 32);
        return slots && !(slots & (slots - 1)) && slots <= (1u << 24) && dataOffset == dataOffsetFor(slots) &&
               dataBytes <= mapped && dataOffset <= mapped - dataBytes;
    }

    uint64_t fileBytes() const
    {
        return dataOffset + dataBytes;
    }
};

inline uint32_t firstSlot(const uint8_t *name, uint32_t slots)
{
    return le32(name) & (slots - 1);
}

// A mapping of the store, read in place; the caller keeps the mapping alive while it reads.
class StoreReader {
public:
    bool attach(const uint8_t *base, size_t mapped, const uint8_t *identity)
    {
        base_ = nullptr;
        if (!base || !layout_.parse(base, mapped, identity))
            return false;
        base_ = base;
        return true;
    }

    bool attached() const
    {
        return base_ != nullptr;
    }

    // The bytes published under `name` (kStoreNameBytes), in place
    bool find(const uint8_t *name, Bytes &out) const
    {
        if (!base_)
            return false;
        for (uint32_t probe = 0, slot = firstSlot(name, layout_.slots); probe < layout_.slots;
             ++probe, slot = (slot + 1) & (layout_.slots - 1)) {
            const uint8_t *at = base_ + kStoreHeaderBytes + uint64_t(slot) * kStoreSlotBytes;
            const uint64_t offset = loadAcquire(at);
            if (!offset)
                return false;
            if (offset == kRetired || memcmp(at + 16, name, kStoreNameBytes))
                continue;
            const uint32_t length = le32(at + 8);
            if (offset < layout_.dataOffset || offset > layout_.fileBytes() || length > layout_.fileBytes() - offset)
                return false;
            out = {base_ + offset, length};
            return true;
        }
        return false;
    }

    // The slot `find` would answer from, for a writer to retire: its index, or -1
    int64_t slotOf(const uint8_t *name) const
    {
        for (uint32_t probe = 0, slot = firstSlot(name, layout_.slots); base_ && probe < layout_.slots;
             ++probe, slot = (slot + 1) & (layout_.slots - 1)) {
            const uint8_t *at = base_ + kStoreHeaderBytes + uint64_t(slot) * kStoreSlotBytes;
            const uint64_t offset = loadAcquire(at);
            if (!offset)
                return -1;
            if (offset != kRetired && !memcmp(at + 16, name, kStoreNameBytes))
                return slot;
        }
        return -1;
    }

    const StoreLayout &layout() const
    {
        return layout_;
    }

private:
    const uint8_t *base_ = nullptr;
    StoreLayout layout_;
};

// The one writer: the file mapped shared and writable, every store through the mapping that readers' mappings share.
class StoreWriter {
public:
    enum class Put : uint8_t {
        Added,
        Present,
        Full,
        Refused
    };

    StoreWriter() = default;
    StoreWriter(const StoreWriter &) = delete;
    StoreWriter &operator=(const StoreWriter &) = delete;

    ~StoreWriter()
    {
        close();
    }

    // The store at `path` taken for writing (made at `slots` and `dataBytes` if it is not there or is not a store of
    // `identity`), the file locked against a second writer. `boot` (a string, null: this boot's) names the boot
    // the writer runs in: a store last written in another boot keeps only the entries below its durable end. False with
    // `error`.
    bool open(const std::string &path, const uint8_t *identity, uint32_t slots, uint64_t dataBytes, std::string &error,
              const char *boot = nullptr)
    {
        close();
        if (!slots || (slots & (slots - 1)) || dataBytes % kStoreAlignment)
            return error = "a slot count that is not a power of two, or unaligned data", false;
        if (!lockFile(path, O_RDWR | O_CREAT, error))
            return false;
        struct stat info = {};
        if (fstat(fd_, &info) || !S_ISREG(info.st_mode))
            return error = path + " is not a file", close(), false;
        char current[kStoreBootBytes] = {};
        bool known = false;
        if (boot) {
            memcpy(current, boot, strnlen(boot, kStoreBootBytes - 1));
            known = current[0] != 0;
        } else {
            known = currentBoot(current);
        }
        const StoreLayout wanted = {slots, StoreLayout::dataOffsetFor(slots), dataBytes};
        if (!mapExisting(uint64_t(info.st_size), identity) && !remake(path, wanted, identity, error))
            return close(), false;
        recover(known && memcmp(base_ + kStoreBootAt, current, kStoreBootBytes) == 0);
        memcpy(base_ + kStoreBootAt, current, kStoreBootBytes);
        return true;
    }

    // What a persist syncs: the data written since the durable end, and the slots, as they stand when it is taken
    struct Pending {
        uint64_t from = 0, end = 0, changes = 0;
    };

    // False: nothing was put or retired since the last persist. Every byte written since the mark lies at or past the
    // mark's page end; the next put starts on a page of its own, so no later sync rewrites one this persist trusts.
    bool pending(Pending &out)
    {
        if (!base_ || changes_ == persisted_)
            return false;
        out = {pageEnd(durable()), end_, changes_};
        if (end_ > out.from)
            end_ = pageEnd(end_);
        return true;
    }

    // `p` synced to the medium. It reads only the mapping and the file, which open and close alone change, so a caller
    // may run it beside put and retire, never beside those two
    bool sync(const Pending &p) const
    {
        return (p.end <= p.from || msync(base_ + p.from, size_t(p.end - p.from), MS_SYNC) == 0) &&
               msync(base_, size_t(layout_.dataOffset), MS_SYNC) == 0 && syncBarrier(fd_);
    }

    // After sync(p): the durable end moved past `p`, for syncMark to sync, in the slot of the older mark
    void markDurable(const Pending &p)
    {
        if (p.end > durable()) {
            const uint64_t generation = markGeneration() + 1;
            uint8_t *mark = base_ + kStoreMarksAt + (generation & 1) * kStoreMarkBytes;
            putLe64(mark, p.end);
            putLe64(mark + 8, generation);
        }
        persisted_ = std::max(persisted_, p.changes);
    }

    // The header, with its durable end, synced; beside put and retire as sync() is
    bool syncMark() const
    {
        return msync(base_, kStoreHeaderBytes, MS_SYNC) == 0 && syncBarrier(fd_);
    }

    // Every entry published and every slot retired so far synced to the medium, then the durable end moved past them and
    // synced too. Readers in this boot never wait for it. False: a sync failed, and the mark stays below what it did not
    // reach.
    bool persist()
    {
        Pending p;
        if (!pending(p))
            return base_ != nullptr;
        if (!sync(p))
            return false;
        markDurable(p);
        return syncMark();
    }

    void close()
    {
        if (base_)
            munmap(base_, layout_.fileBytes());
        base_ = nullptr;
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
        used_ = retired_ = 0;
        end_ = 0;
        changes_ = persisted_ = 0;
    }

    // `replace`: an entry under `name` is retired once this one is in (a reader finds one or the other, never neither)
    Put put(const uint8_t *name, const uint8_t *bytes, uint32_t length, bool replace = false)
    {
        if (!base_)
            return Put::Refused;
        const int64_t old = reader_.slotOf(name);
        if (old >= 0 && !replace)
            return Put::Present;
        const uint64_t align = length && !(length % kStorePageBytes) ? kStorePageBytes : kStoreAlignment;
        const uint64_t at = (end_ + align - 1) & ~(align - 1);
        if ((used_ + retired_ + 1) * 4 > uint64_t(layout_.slots) * 3 || at + length > layout_.fileBytes())
            return Put::Full;
        uint32_t slot = firstSlot(name, layout_.slots);
        while (loadAcquire(slotAt(slot)))
            slot = (slot + 1) & (layout_.slots - 1);
        memcpy(base_ + at, bytes, length);
        uint8_t *entry = slotAt(slot);
        putLe32(entry + 8, length);
        memcpy(entry + 16, name, kStoreNameBytes);
        __atomic_store_n(reinterpret_cast<uint64_t *>(entry), at, __ATOMIC_RELEASE);
        end_ = at + length;
        ++changes_;
        ++used_;
        if (old >= 0)
            retireSlot(uint32_t(old));
        return Put::Added;
    }

    // The entry under `name` stepped over from now on, so a later put may publish it again
    bool retire(const uint8_t *name)
    {
        const int64_t slot = reader_.slotOf(name);
        if (slot < 0)
            return false;
        retireSlot(uint32_t(slot));
        return true;
    }

    const StoreReader &reader() const
    {
        return reader_;
    }

    uint8_t *base() const
    {
        return base_;
    }

    uint64_t mappedBytes() const
    {
        return base_ ? layout_.fileBytes() : 0;
    }

    uint32_t used() const
    {
        return used_;
    }

    uint32_t retired() const
    {
        return retired_;
    }

    uint64_t dataUsed() const
    {
        return end_ > layout_.dataOffset ? end_ - layout_.dataOffset : 0;
    }

    uint64_t durableEnd() const
    {
        return base_ ? durable() : 0;
    }

    const StoreLayout &layout() const
    {
        return layout_;
    }

private:
    int fd_ = -1;
    uint8_t *base_ = nullptr;
    StoreLayout layout_;
    StoreReader reader_;
    uint32_t used_ = 0, retired_ = 0;
    uint64_t end_ = 0;
    uint64_t changes_ = 0, persisted_ = 0;  // puts and retires, and how many of them the durable end covers

    static void putLe32(uint8_t *p, uint32_t value)
    {
        for (unsigned i = 0; i < 4; ++i)
            p[i] = uint8_t(value >> (8 * i));
    }

    static void putLe64(uint8_t *p, uint64_t value)
    {
        for (unsigned i = 0; i < 8; ++i)
            p[i] = uint8_t(value >> (8 * i));
    }

    void retireSlot(uint32_t slot)
    {
        __atomic_store_n(reinterpret_cast<uint64_t *>(slotAt(slot)), kRetired, __ATOMIC_RELEASE);
        ++changes_;
        --used_;
        ++retired_;
    }

    uint8_t *slotAt(uint32_t slot) const
    {
        return base_ + kStoreHeaderBytes + uint64_t(slot) * kStoreSlotBytes;
    }

    uint64_t pageEnd(uint64_t offset) const
    {
        return std::min((offset + kStorePageBytes - 1) & ~(kStorePageBytes - 1), layout_.fileBytes());
    }

    // The two marks' newer generation, 0 for a store never persisted
    uint64_t markGeneration() const
    {
        return std::max(le64(base_ + kStoreMarksAt + 8), le64(base_ + kStoreMarksAt + kStoreMarkBytes + 8));
    }

    // The newer mark's end: every published entry below it is on the medium (the data offset: none is)
    uint64_t durable() const
    {
        const uint8_t *first = base_ + kStoreMarksAt, *second = first + kStoreMarkBytes;
        const uint8_t *newer = le64(second + 8) > le64(first + 8) ? second : first;
        return std::clamp(le64(newer), layout_.dataOffset, layout_.fileBytes());
    }

    bool mapRange(uint64_t bytes)
    {
        void *mapped = mmap(nullptr, size_t(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapped == MAP_FAILED)
            return false;
        base_ = static_cast<uint8_t *>(mapped);
        return true;
    }

    bool mapExisting(uint64_t size, const uint8_t *identity)
    {
        uint8_t header[kStoreHeaderBytes];
        if (size < kStoreHeaderBytes || pread(fd_, header, sizeof header, 0) != ssize_t(sizeof header) ||
            !layout_.parse(header, size_t(size), identity) || layout_.fileBytes() != size || !mapRange(size))
            return false;
        return reader_.attach(base_, size_t(size), identity);
    }

    bool lockFile(const std::string &path, int flags, std::string &error)
    {
        fd_ = ::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd_ < 0)
            return error = "cannot open " + path + ": " + strerror(errno), false;
        if (flock(fd_, LOCK_EX | LOCK_NB))
            return error = "another writer holds " + path, close(), false;
        return true;
    }

    // A file that is not a store of `identity` is never cut in place: a reader may still map it (a compiler's earlier store
    // the kernel still shares), and would fault past its new end. The new store is made beside it and renamed over it.
    bool remake(const std::string &path, const StoreLayout &wanted, const uint8_t *identity, std::string &error)
    {
        close();
        const std::string fresh = path + ".new";
        unlink(fresh.c_str());
        if (!lockFile(fresh, O_RDWR | O_CREAT | O_EXCL, error) || !make(wanted, identity, error))
            return false;
        if (rename(fresh.c_str(), path.c_str()))
            return error = "cannot replace " + path + ": " + strerror(errno), unlink(fresh.c_str()), false;
        return true;
    }

    bool make(const StoreLayout &wanted, const uint8_t *identity, std::string &error)
    {
        if (ftruncate(fd_, off_t(wanted.fileBytes())))
            return error = std::string("cannot size the store: ") + strerror(errno), false;
        layout_ = wanted;
        if (!mapRange(wanted.fileBytes()))
            return error = std::string("cannot map the store: ") + strerror(errno), false;
        memcpy(base_, kStoreMagic, sizeof kStoreMagic);
        putLe32(base_ + 8, kStoreVersion);
        putLe32(base_ + 12, kStoreNameBytes);
        putLe32(base_ + 16, wanted.slots);
        putLe64(base_ + 24, wanted.dataOffset);
        putLe64(base_ + 32, wanted.dataBytes);
        memcpy(base_ + 40, identity, kIdentityBytes);
        return reader_.attach(base_, size_t(wanted.fileBytes()), identity) ||
               (error = "the store made does not parse", false);
    }

    // `sameBoot`: the last writer ran in this boot, so every published entry is whole in the page cache; otherwise only
    // those below the durable end are, and the rest are retired before any reader maps the store
    void recover(bool sameBoot)
    {
        const uint64_t durable = this->durable();
        end_ = layout_.dataOffset;
        for (uint32_t slot = 0; slot < layout_.slots; ++slot) {
            uint8_t *entry = slotAt(slot);
            const uint64_t offset = loadAcquire(entry);
            if (!offset)
                continue;
            if (offset == kRetired) {
                ++retired_;
                continue;
            }
            const uint64_t entryEnd = offset + le32(entry + 8);
            if (!sameBoot && entryEnd > durable) {
                __atomic_store_n(reinterpret_cast<uint64_t *>(entry), kRetired, __ATOMIC_RELEASE);
                ++changes_;
                ++retired_;
                continue;
            }
            ++used_;
            end_ = entryEnd > end_ && entryEnd <= layout_.fileBytes() ? entryEnd : end_;
        }
        // nothing new lands in a page that holds bytes below the mark, in either case: the same boot's newest entries
        // may have been retired below it
        end_ = std::max(end_, pageEnd(durable));
    }
};

// The store at `path` written again with only its live entries (made fresh at `slots` and `dataBytes`), renamed over
// it: a reader still mapping the old file keeps reading it whole. With `keepBytes` and `keepEntries`, only the newest
// entries within both are kept (the oldest dropped first: entries are appended in the order they were made), so a full
// store has room again. No writer may hold `path`. `boot`: as StoreWriter::open takes it.
inline bool compactStore(const std::string &path, const uint8_t *identity, uint32_t slots, uint64_t dataBytes,
                         std::string &error, uint64_t keepBytes = UINT64_MAX, uint32_t keepEntries = UINT32_MAX,
                         const char *boot = nullptr)
{
    const std::string fresh = path + ".new";
    unlink(fresh.c_str());
    StoreWriter old, made;
    if (!old.open(path, identity, slots, dataBytes, error, boot) ||
        !made.open(fresh, identity, slots, dataBytes, error, boot))
        return false;
    const StoreLayout &layout = old.layout();
    std::vector<std::pair<uint64_t, const uint8_t *>> live;  // offset, slot
    for (uint32_t slot = 0; slot < layout.slots; ++slot) {
        const uint8_t *entry = old.base() + kStoreHeaderBytes + uint64_t(slot) * kStoreSlotBytes;
        const uint64_t offset = loadAcquire(entry);
        if (offset && offset != kRetired)
            live.emplace_back(offset, entry);
    }
    std::sort(live.begin(), live.end());
    size_t first = live.size();
    uint64_t kept = 0;
    while (first > 0 && live.size() - first < keepEntries) {
        const uint64_t bytes = le32(live[first - 1].second + 8) + kStoreAlignment;
        if (kept + bytes > keepBytes)
            break;
        kept += bytes;
        --first;
    }
    for (size_t i = first; i < live.size(); ++i) {
        Bytes bytes;
        const uint8_t *name = live[i].second + 16;
        if (old.reader().find(name, bytes) &&
            made.put(name, bytes.data, uint32_t(bytes.size)) == StoreWriter::Put::Full)
            break;
    }
    if (!made.persist())
        return error = "cannot sync " + fresh + ": " + strerror(errno), made.close(), unlink(fresh.c_str()), false;
    made.close();
    old.close();
    if (rename(fresh.c_str(), path.c_str()))
        return error = "cannot replace " + path + ": " + strerror(errno), unlink(fresh.c_str()), false;
    return true;
}

}  // namespace switch_seed_pack
