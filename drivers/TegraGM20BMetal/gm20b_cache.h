// SPDX-License-Identifier: MIT
// The records the runtime made for states the pack lacks (record.h), kept on disk so each state's probe is paid once per build
// (compiler.h: the in-process table that makes each state at most once).
//
// An entry is a state's record or the reason it has none, under <the process's own shader cache directory>/raw-<stack key>/: the
// directory the plugin chose for m2v's own pipeline binaries, owner-only and kept across reboots. Not a directory other processes
// share: a record is GPU code the render server uploads and runs. Entries are whole files (written apart, synced, renamed in),
// checked against their full key when read; a directory of another build's stack is removed when the cache opens.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <string>
#include <string_view>
#include "compute_record.h"
#include "record.h"

namespace switch_seed_pack {
class Seed;
}

namespace gm20b {

// The installed native records' name of a program slot's entry (RecordCache::findSeedSlot): its bytes' FNV-1a, moved off
// the records' names (a name both took would fail the seed's write); a find compares the bytes
inline uint64_t seedSlotName(const void *slot, size_t size)
{
    constexpr uint64_t kSlotDomain = 0x736c6f7400000000ull;
    return Pack::fnv(slot, size) ^ kSlotDomain;
}

// What a made record depends on: the build (its stack key hashes what records are made and read by: the pack generator, m2v, NVK
// and NAK, the frontend and the runtime's direct compile and codecs, so a new compiler or pack generator is a new stack), the
// functions' AIR (their metallib HASH tags: SHA-256 of each one's bitcode) and the state's key (every descriptor field and function
// constant the probe draws with)
struct CacheKey {
    uint64_t stack = 0;
    uint8_t vertexAir[32] = {}, fragmentAir[32] = {};
    std::string key;

    uint64_t name() const;
};

class RecordCache {
public:
    RecordCache();
    ~RecordCache();
    // false: no entry can be kept (`dir` unusable); loads miss and stores do nothing then. attach() then tidy().
    bool open(const std::string &dir, uint64_t stack, uint64_t capBytes);
    // The directory taken for loads and stores at once (a mkdir); tidy() removes another build's entries and a dead writer's
    // temporaries and counts the bytes kept, which stores need for the cap: it may run later, on another thread
    // `exclusive`: true when only this process writes to `dir`, allowing empty-cache lookup skipping after tidy().
    bool attach(const std::string &dir, uint64_t stack, uint64_t capBytes, bool exclusive = false);
    // The installed native records, <dir>/raw-<stack>/ packed (kits/SwitchSeedPack), are mapped here and read in place
    // before the directory's own entries; attach before any compiler worker starts.
    bool attachSeed(const std::string &dir, uint64_t stack);
    // A program slot the installed native records hold as an entry of its own pages (seedSlotName, program_pages.h
    // programSlotEntryBytes): its bytes in place, with the seed's pack mapping holding them; null: none
    const uint8_t *findSeedSlot(const uint8_t *slot, size_t size, const uint8_t *&mapping, uint64_t &bytes) const;
    void tidy();

    enum class Found : uint8_t {
        None,
        Record,
        Refusal
    };

    // *seeded: whether the entry came from the installed seed rather than this process's own directory
    Found load(const CacheKey &k, RuntimeRecord &record, std::string &refusal, bool *seeded = nullptr);
    // `record` null: the state's refusal (`refusal`)
    bool store(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal);
    // A kernel's record at one compute key (compute_key.h), or its refusal, as a state's
    Found loadKernel(const CacheKey &k, ComputeRecord &record, std::string &refusal);
    bool storeKernel(const CacheKey &k, const ComputeRecord *record, const std::string &refusal);

    uint64_t bytes() const
    {
        std::lock_guard<std::mutex> l(lock_);
        return bytes_;
    }

private:
    std::string dir_;
    std::unique_ptr<switch_seed_pack::Seed> seed_;
    uint64_t seedStack_ = 0, stack_ = 0, cap_ = 0, bytes_ = 0;
    std::atomic<bool> dirReady_{false};
    std::atomic<bool> diskMayHaveEntries_{false};
    bool exclusive_ = false;
    bool anyStored_ = false;
    mutable std::mutex lock_;
    std::string parent_;
    std::string path(const CacheKey &k) const;
    void evict(uint64_t room);
    bool findSeedBytes(const CacheKey &k, std::string_view &bytes) const;
    bool loadDiskBytes(const CacheKey &k, std::string &bytes) const;
    bool storeBytes(const CacheKey &k, const std::string &bytes);
};

// Turns the records' bytes into the entry format and back (gm20b_cache.cpp's own; exposed for the host test)
std::string encodeEntry(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal);
RecordCache::Found decodeEntry(std::string_view bytes, const CacheKey &k, RuntimeRecord &record, std::string &refusal);
std::string encodeKernelEntry(const CacheKey &k, const ComputeRecord *record, const std::string &refusal);
RecordCache::Found decodeKernelEntry(std::string_view bytes, const CacheKey &k, ComputeRecord &record,
                                     std::string &refusal);

}  // namespace gm20b
