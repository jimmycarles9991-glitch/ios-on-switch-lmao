// SPDX-License-Identifier: MIT
// DEVELOPMENT: how often a command buffer's structure repeats one encoded just before it, which decides whether replaying
// a pushbuffer instead of encoding it again (FRAMECPU C4) can pay. A shape hashes the encoders in order, each pass's
// attachment formats, load and store actions and size class, and each draw's pipeline and depth/stencil state objects,
// primitive, vertex count class, instancing, indexing and binding shape (which slots hold bytes, a buffer or an offset,
// and which texture and sampler slots are set); never addresses or values that change per frame.
#pragma once
#include <atomic>
#include <cstdint>

namespace raw {

class ReplayShape {
public:
    void mix(uint64_t v)
    {
        hash_ = (hash_ ^ v) * 0x100000001b3ull;
    }

    uint64_t value() const
    {
        return hash_;
    }

    // log2 buckets: a vertex count or a size changes the shape only across a power of two
    static uint64_t sizeClass(uint64_t v)
    {
        return v ? 64 - uint64_t(__builtin_clzll(v)) : 0;
    }

private:
    uint64_t hash_ = 0xcbf29ce484222325ull;
};

// Command buffers whose shape is the last one's (same), one of the three before it (recent), or none of them (fresh).
// note() is the submit worker's alone; the counts are read by anyone.
class ReplayTally {
public:
    std::atomic<uint64_t> same{0}, recent{0}, fresh{0};

    void note(uint64_t shape)
    {
        if (seen_ && last_[0] == shape) {
            same.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        bool found = false;
        for (uint32_t i = 1; i < kDepth && i < seen_; ++i)
            found |= last_[i] == shape;
        (found ? recent : fresh).fetch_add(1, std::memory_order_relaxed);
        for (uint32_t i = kDepth - 1; i > 0; --i)
            last_[i] = last_[i - 1];
        last_[0] = shape;
        seen_ += seen_ < kDepth;
    }

private:
    static constexpr uint32_t kDepth = 4;
    uint64_t last_[kDepth] = {};
    uint32_t seen_ = 0;
};

}  // namespace raw
