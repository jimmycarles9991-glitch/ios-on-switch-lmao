// SPDX-License-Identifier: MIT
// The records the runtime made for states the pack lacks (raw_record.h), kept on disk so each state's probe is paid once per build
// (raw_compiler.h: the in-process table that makes each state at most once).
//
// An entry is a state's record or the reason it has none, under <the process's own shader cache directory>/raw-<stack key>/: the
// directory the plugin chose for m2v's own pipeline binaries, owner-only and kept across reboots. Not a directory other processes
// share: a record is GPU code the render server uploads and runs. Entries are whole files (written apart, renamed in), checked
// against their full key and a checksum when read; a directory of another build's stack is removed when the cache opens.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <string>
#include "raw_record.h"

namespace raw {

// What a made record depends on: the build (its stack key hashes the pack, m2v, NVK and NAK, the frontend and the raw path's own
// sources, so a new compiler or pack is a new stack), the functions' AIR (their metallib HASH tags: SHA-256 of each one's bitcode)
// and the state's key (every descriptor field and function constant the probe draws with)
struct CacheKey {
    uint64_t stack = 0;
    uint8_t vertexAir[32] = {}, fragmentAir[32] = {};
    std::string key;

    uint64_t name() const;
};

class RecordCache {
public:
    // false: no entry can be kept (`dir` unusable); loads miss and stores do nothing then. attach() then tidy().
    bool open(const std::string &dir, uint64_t stack, uint64_t capBytes);
    // The directory taken for loads and stores at once (a mkdir); tidy() removes another build's entries and a dead writer's
    // temporaries and counts the bytes kept, which stores need for the cap: it may run later, on another thread
    bool attach(const std::string &dir, uint64_t stack, uint64_t capBytes);
    void tidy();

    enum class Found : uint8_t {
        None,
        Record,
        Refusal
    };

    Found load(const CacheKey &k, RuntimeRecord &record, std::string &refusal);
    // `record` null: the state's refusal (`refusal`)
    bool store(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal);

    uint64_t bytes() const
    {
        return bytes_;
    }

private:
    std::string dir_;
    uint64_t stack_ = 0, cap_ = 0, bytes_ = 0;
    std::mutex lock_;
    std::string parent_;
    std::string path(const CacheKey &k) const;
    void evict(uint64_t room);
};

// Turns the records' bytes into the entry format and back (raw_cache.cpp's own; exposed for the host test)
std::string encodeEntry(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal);
RecordCache::Found decodeEntry(const std::string &bytes, const CacheKey &k, RuntimeRecord &record,
                               std::string &refusal);

}  // namespace raw
