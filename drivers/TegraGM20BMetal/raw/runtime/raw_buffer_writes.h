// SPDX-License-Identifier: MIT
// The buffer memory a command buffer's GPU work wrote so far, in encode order. A split prologue (raw_split.cpp) runs ahead
// of all of the command buffer's 3D work, so a draw takes its split twin only where nothing before it wrote the uniform
// memory the prologue reads. One encoder owns a command buffer: no lock.
#pragma once
#include <cstdint>

namespace raw {

class BufferWrites {
public:
    static constexpr uint32_t kRanges = 32;

    void clear()
    {
        count_ = 0;
        unknown_ = false;
    }

    // [va, va + bytes) written; a write of no known extent, or past the list, is a write of anything
    void note(uint64_t va, uint64_t bytes)
    {
        if (!bytes)
            return;
        if (count_ && va_[count_ - 1] == va && bytes_[count_ - 1] == bytes)
            return;
        if (!va || count_ == kRanges) {
            unknown_ = true;
            return;
        }
        va_[count_] = va;
        bytes_[count_++] = bytes;
    }

    // a write the encode does not bound (a dispatch, an indirect command buffer's)
    void anything()
    {
        unknown_ = true;
    }

    bool meets(uint64_t va, uint64_t bytes) const
    {
        if (unknown_)
            return true;
        for (uint32_t i = 0; i < count_; i++)
            if (va < va_[i] + bytes_[i] && va_[i] < va + bytes)
                return true;
        return false;
    }

private:
    uint64_t va_[kRanges] = {}, bytes_[kRanges] = {};
    uint32_t count_ = 0;
    bool unknown_ = false;
};

// Why a draw of a state with a split did not take its twin (RenderEncoder::splitTwin), debug builds' counts
enum class SplitWhole : uint8_t {
    Forced,   // a probe forced whole programs
    Box,      // its pixel limit is empty, or the pass's plan did not admit it at that size
    Unbound,  // a uniform buffer the prologue reads is not bound
    Written,  // work before it in the command buffer wrote that buffer's memory
    Unmade,   // its twin could not be made
    Count
};

}  // namespace raw
