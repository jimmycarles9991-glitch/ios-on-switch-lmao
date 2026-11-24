// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gm20b {

struct alignas(16) SplitBatchRecord {
    uint32_t push[32] = {};
    uint64_t frontier = 0, reserved = 0;

    struct Buffer {
        uint64_t va = 0;
        uint32_t bytes = 0, reserved = 0;
    } buffers[8];
};

struct SplitBatchHeader {
    uint64_t records = 0;
    uint32_t count = 0, stride = sizeof(SplitBatchRecord);
};

static_assert(sizeof(SplitBatchRecord) == 272);
static_assert(offsetof(SplitBatchRecord, frontier) == 128);
static_assert(offsetof(SplitBatchRecord, buffers) == 144);
static_assert(sizeof(SplitBatchHeader) == 16);

struct SplitBatchKey {
    uint32_t program = 0;
    uint64_t data = 0;
    uint32_t dataBytes = 0, threads = 0;

    bool operator==(const SplitBatchKey &other) const
    {
        return program == other.program && data == other.data && dataBytes == other.dataBytes &&
               threads == other.threads;
    }
};

class SplitBatchPlanner {
public:
    static constexpr uint32_t kEnd = ~0u, kMaxRecords = 1024;

    struct Run {
        SplitBatchKey key;
        uint32_t first = kEnd, last = kEnd, count = 0;
    };

    void clear(size_t count)
    {
        runs_.clear();
        next_.assign(count, kEnd);
        used_ = 0;
        last_ = kEnd;
        if (++epoch_ == 0) {
            for (Slot &slot : table_)
                slot.epoch = 0;
            epoch_ = 1;
        }
    }

    void append(const SplitBatchKey &key, uint32_t record)
    {
        uint32_t run = last_;
        if (run == kEnd || !(runs_[run].key == key))
            run = lookup(key);
        if (run == kEnd || runs_[run].count == kMaxRecords) {
            run = uint32_t(runs_.size());
            runs_.push_back({key, record, record, 1});
            store(key, run);
        } else {
            next_[runs_[run].last] = record;
            runs_[run].last = record;
            ++runs_[run].count;
        }
        last_ = run;
    }

    const std::vector<Run> &runs() const
    {
        return runs_;
    }

    uint32_t next(uint32_t record) const
    {
        return next_[record];
    }

private:
    struct Slot {
        SplitBatchKey key;
        uint32_t run = kEnd;
        uint32_t epoch = 0;
    };

    static size_t hashKey(const SplitBatchKey &key)
    {
        uint64_t h = key.data ^ (uint64_t(key.program) << 32) ^ (uint64_t(key.dataBytes) << 16) ^ key.threads;
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebull;
        return size_t(h ^ (h >> 31));
    }

    uint32_t lookup(const SplitBatchKey &key) const
    {
        if (table_.empty())
            return kEnd;
        const size_t mask = table_.size() - 1;
        for (size_t i = hashKey(key) & mask;; i = (i + 1) & mask) {
            const Slot &slot = table_[i];
            if (slot.epoch != epoch_)
                return kEnd;
            if (slot.key == key)
                return slot.run;
        }
    }

    void grow()
    {
        std::vector<Slot> old = std::move(table_);
        const size_t nextSize = old.empty() ? 16 : old.size() * 2;
        table_.assign(nextSize, {});
        const size_t mask = nextSize - 1;
        for (const Slot &slot : old) {
            if (slot.epoch != epoch_)
                continue;
            for (size_t i = hashKey(slot.key) & mask;; i = (i + 1) & mask) {
                if (table_[i].epoch != epoch_) {
                    table_[i] = slot;
                    break;
                }
            }
        }
    }

    void store(const SplitBatchKey &key, uint32_t run)
    {
        if (table_.empty() || (used_ + 1) * 2 > table_.size())
            grow();
        const size_t mask = table_.size() - 1;
        for (size_t i = hashKey(key) & mask;; i = (i + 1) & mask) {
            Slot &slot = table_[i];
            if (slot.epoch != epoch_) {
                slot = {key, run, epoch_};
                ++used_;
                return;
            }
            if (slot.key == key) {
                slot.run = run;
                return;
            }
        }
    }

    std::vector<Run> runs_;
    std::vector<uint32_t> next_;
    std::vector<Slot> table_;
    uint32_t epoch_ = 1;
    uint32_t used_ = 0;
    uint32_t last_ = kEnd;
};

}  // namespace gm20b
