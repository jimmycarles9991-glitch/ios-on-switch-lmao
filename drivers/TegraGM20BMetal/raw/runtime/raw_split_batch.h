// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace raw {

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

    auto identity() const
    {
        return std::tie(program, data, dataBytes, threads);
    }

    bool operator<(const SplitBatchKey &other) const
    {
        return identity() < other.identity();
    }

    bool operator==(const SplitBatchKey &other) const
    {
        return identity() == other.identity();
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
        latest_.clear();
        last_ = kEnd;
    }

    void append(const SplitBatchKey &key, uint32_t record)
    {
        uint32_t run = last_;
        if (run == kEnd || !(runs_[run].key == key)) {
            const auto found = latest_.find(key);
            run = found == latest_.end() ? kEnd : found->second;
        }
        if (run == kEnd || runs_[run].count == kMaxRecords) {
            run = uint32_t(runs_.size());
            runs_.push_back({key, record, record, 1});
            latest_[key] = run;
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
    std::vector<Run> runs_;
    std::vector<uint32_t> next_;
    std::map<SplitBatchKey, uint32_t> latest_;
    uint32_t last_ = kEnd;
};

}  // namespace raw
