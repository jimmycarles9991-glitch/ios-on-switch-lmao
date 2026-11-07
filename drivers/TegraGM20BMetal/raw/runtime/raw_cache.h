// SPDX-License-Identifier: MIT
// The records the runtime made for states the pack lacks (raw_record.h), kept on disk so each state's probe is paid once per build,
// and the in-process table that makes each state at most once.
//
// An entry is a state's record or the reason it has none, under <the process's own shader cache directory>/raw-<stack key>/: the
// directory the plugin chose for m2v's own pipeline binaries, owner-only and kept across reboots. Not a directory other processes
// share: a record is GPU code the render server uploads and runs. Entries are whole files (written apart, renamed in), checked
// against their full key and a checksum when read; a directory of another build's stack is removed when the cache opens.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
    // false: no entry can be kept (`dir` unusable); loads miss and stores do nothing then
    bool open(const std::string &dir, uint64_t stack, uint64_t capBytes);

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
    std::string path(const CacheKey &k) const;
    void evict(uint64_t room);
};

// Turns the records' bytes into the entry format and back (raw_cache.cpp's own; exposed for the host test)
std::string encodeEntry(const CacheKey &k, const RuntimeRecord *record, const std::string &refusal);
RecordCache::Found decodeEntry(const std::string &bytes, const CacheKey &k, RuntimeRecord &record,
                               std::string &refusal);

// The states this process completes: each asked state made once, by the disk cache or `make` (the probe and record), one at a
// time on a background-QoS serial queue, never on the asker's thread. A later ask of a state answers its result or that it is
// still being made.
class Compiler {
public:
    enum class Outcome : uint8_t {
        Made,
        Refused,  // the state's own reason (kept on disk: the same probe would refuse it again)
        Failed    // the probe could not run (not kept: a later process may)
    };

    using Make = std::function<Outcome(const CacheKey &k, RuntimeRecord &record, std::string &why)>;
    // Called on the queue once the state is made (`record` null: refused, `why`)
    using Done = std::function<void(const RuntimeRecord *record, const std::string &why)>;

    Compiler(RecordCache *cache, Make make);
    ~Compiler();

    enum class State : uint8_t {
        Pending,
        Ready,
        Refused
    };

    // The state's result so far; the first ask of it queues its making and calls `done` once made
    State ask(const CacheKey &k, Done done, const RuntimeRecord **record = nullptr);
    void drain();  // host tests: waits for everything asked so far

    struct Stats {
        uint64_t hits = 0, misses = 0, refusals = 0, failures = 0, makeUs = 0;
    };

    Stats stats() const;

private:
    struct Entry {
        State state = State::Pending;
        std::unique_ptr<RuntimeRecord> record;
        std::string why;
    };

    struct Job;

    RecordCache *cache_;
    Make make_;
    void *queue_;
    mutable std::mutex lock_;
    std::condition_variable idle_;
    uint64_t queued_ = 0;
    std::map<uint64_t, Entry> entries_;
    Stats stats_;
    static void run(void *job);
};

}  // namespace raw
