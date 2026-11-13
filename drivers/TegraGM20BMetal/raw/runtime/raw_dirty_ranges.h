// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raw {

class DirtyRanges {
public:
    struct Range {
        uintptr_t begin = 0, end = 0;
        bool invalidate = false;
    };

    void add(const void *cpu, uint64_t bytes, bool invalidate)
    {
        if (!cpu || !bytes)
            return;
        const uintptr_t address = uintptr_t(cpu);
        const uintptr_t begin = address & ~uintptr_t(63);
        const uintptr_t end = (address + bytes + 63) & ~uintptr_t(63);
        if (!ranges_.empty()) {
            auto &last = ranges_.back();
            if (last.begin <= end && begin <= last.end) {
                last.begin = std::min(last.begin, begin);
                last.end = std::max(last.end, end);
                last.invalidate |= invalidate;
                return;
            }
        }
        ranges_.push_back({begin, end, invalidate});
    }

    const std::vector<Range> &merged()
    {
        std::sort(ranges_.begin(), ranges_.end(), [](const Range &a, const Range &b) {
            return a.begin < b.begin;
        });
        std::size_t count = 0;
        for (const auto range : ranges_) {
            if (count && range.begin <= ranges_[count - 1].end) {
                auto &previous = ranges_[count - 1];
                previous.end = std::max(previous.end, range.end);
                previous.invalidate |= range.invalidate;
            } else {
                ranges_[count++] = range;
            }
        }
        ranges_.resize(count);
        return ranges_;
    }

    void clear()
    {
        ranges_.clear();
    }

private:
    std::vector<Range> ranges_;
};

}  // namespace raw
