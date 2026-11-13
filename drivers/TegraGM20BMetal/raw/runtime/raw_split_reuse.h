// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace raw {

// DEVELOPMENT: what a draw the split plan takes draws with (HW Test's bench, pack forms split, whole_generic and
// split_whole): its twin, as shipped; the generic whole program with no variant and no prologue; or the prologue launched
// and its frontier written but the generic whole program drawn
enum class SplitProbe : uint8_t {
    Twin,
    Generic,
    LaunchOnly,
};

struct SplitInputs {
    const void *program = nullptr;
    std::array<uint32_t, 32> push{};
    std::array<uint64_t, 8> buffers{};
    std::array<uint32_t, 8> bytes{};

    bool operator==(const SplitInputs &other) const
    {
        return program == other.program && push == other.push && buffers == other.buffers && bytes == other.bytes;
    }

    uint64_t hash() const
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        auto add = [&](uint64_t word) {
            h ^= word;
            h *= 0x100000001b3ULL;
            h ^= h >> 32;
        };
        add(reinterpret_cast<uintptr_t>(program));
        for (auto word : push)
            add(word);
        for (auto word : buffers)
            add(word);
        for (auto word : bytes)
            add(word);
        return h;
    }
};

// Entries live for one command buffer. Storage stays warm across resets; no map nodes are allocated per draw.
class SplitReuse {
public:
    void clear()
    {
        size_ = 0;
        last_ = SIZE_MAX;
        if (!++generation_) {
            for (auto &entry : entries_)
                entry.generation = 0;
            generation_ = 1;
        }
    }

    uint64_t find(const SplitInputs &inputs)
    {
        if (last_ != SIZE_MAX && entries_[last_].inputs == inputs)
            return entries_[last_].frontier;
        if (entries_.empty())
            return 0;
        const auto at = slot(inputs, inputs.hash());
        if (entries_[at].generation != generation_)
            return 0;
        last_ = at;
        return entries_[at].frontier;
    }

    void remember(const SplitInputs &inputs, uint64_t frontier)
    {
        if (!frontier || size_ == kLimit)
            return;
        if (entries_.empty() || (size_ + 1) * 2 > entries_.size())
            grow();
        insert(inputs, inputs.hash(), frontier);
    }

    size_t size() const
    {
        return size_;
    }

    size_t capacity() const
    {
        return entries_.size();
    }

private:
    struct Entry {
        SplitInputs inputs;
        uint64_t hash = 0, frontier = 0, generation = 0;
    };

    static constexpr size_t kLimit = 4096;
    std::vector<Entry> entries_;
    uint64_t generation_ = 1;
    size_t size_ = 0, last_ = SIZE_MAX;

    size_t slot(const SplitInputs &inputs, uint64_t hash) const
    {
        size_t at = hash & (entries_.size() - 1);
        while (entries_[at].generation == generation_ &&
               (entries_[at].hash != hash || !(entries_[at].inputs == inputs)))
            at = (at + 1) & (entries_.size() - 1);
        return at;
    }

    void insert(const SplitInputs &inputs, uint64_t hash, uint64_t frontier)
    {
        const auto at = slot(inputs, hash);
        size_ += entries_[at].generation != generation_;
        entries_[at] = {inputs, hash, frontier, generation_};
        last_ = at;
    }

    void grow()
    {
        auto old = std::move(entries_);
        entries_.resize(old.empty() ? 16 : old.size() * 2);
        size_ = 0;
        last_ = SIZE_MAX;
        for (const auto &entry : old)
            if (entry.generation == generation_)
                insert(entry.inputs, entry.hash, entry.frontier);
    }
};

}  // namespace raw
