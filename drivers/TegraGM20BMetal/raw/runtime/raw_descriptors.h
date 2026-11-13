// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include "raw_spill_table.h"

namespace raw {

// A render state's set 0: its NVK layout's size (PackPipeline::setBytes) in 256-byte blocks, at most two (its record's
// kPackEntries entries, 16 bytes each at most). The words past a state's own size stay zero, so two builds of a set
// compare whole.
using DescriptorWords = std::array<uint32_t, 128>;
constexpr uint32_t kRenderSetBytes = sizeof(DescriptorWords);

constexpr uint32_t renderSetBytes(uint32_t layoutBytes)
{
    return layoutBytes <= 256 ? 256 : (layoutBytes + 255) & ~255u;
}

// The upload allocations must remain immutable and live until every draw using this encoder has retired.
class DescriptorSnapshot {
public:
    // `bytes`: the upload's size; a set of other bytes is uploaded again whatever its words
    template <typename Upload> uint64_t resolve(const DescriptorWords &words, Upload upload, uint32_t bytes = 256)
    {
        if (!address_ || words_ != words || bytes_ != bytes) {
            const uint64_t address = upload(words);
            if (!address)
                return 0;
            words_ = words;
            bytes_ = bytes;
            address_ = address;
        }
        return address_;
    }

    void forget()
    {
        address_ = 0;
    }

private:
    DescriptorWords words_ = {};
    uint32_t bytes_ = 0;
    uint64_t address_ = 0;
};

struct PreparedDescriptorWords {
    DescriptorWords words{};
    std::array<uint32_t, 32> residuals{};
    uint32_t residualMask = 0;
    SpillResiduals spilled[2];  // the vertex and the fragment argument tables' (raw_spill_table.h)
    uint64_t address = 0;
};

// A pipeline's set 0, built again where its bindings moved. `moved`: the entries whose bindings moved since the last
// resolve (bit k: entry k); the set of another pipeline than the last one's is built whole. `memory`: the entries the GPU
// reads from the set's upload (PackPipeline::memoryEntries): the set is uploaded again only where one of them moved, its
// other entries the CPU's alone (the uniform buffers' constant buffer binds read their words here).
class PreparedDescriptors {
public:
    template <typename Build, typename Upload>
    const PreparedDescriptorWords *resolve(const void *pipeline, uint32_t moved, uint32_t memory, Build build,
                                           Upload upload, uint32_t bytes = 256)
    {
        const bool same = value_.address && pipeline_ == pipeline;
        if (same && !moved)
            return &value_;
        if (!same)
            value_ = {};
        pipeline_ = nullptr;  // until this build succeeds
        if (!build(value_, same ? moved : ~0u))
            return nullptr;
        if (!same || (moved & memory)) {
            const uint64_t address = snapshot_.resolve(value_.words, upload, bytes);
            if (!address)
                return nullptr;
            value_.address = address;
        }
        pipeline_ = pipeline;
        return &value_;
    }

    const PreparedDescriptorWords &last() const
    {
        return value_;
    }

    // The GPU loaded words into the set it uploaded last: the next resolve builds and uploads one of its own
    void forget()
    {
        value_ = {};
        pipeline_ = nullptr;
        snapshot_.forget();
    }

private:
    DescriptorSnapshot snapshot_;
    PreparedDescriptorWords value_;
    const void *pipeline_ = nullptr;
};

// What the last draw bound of set 0: its address, and each uniform buffer's constant buffer (the descriptor's base and
// range). A draw of another pipeline binds all of them again; one of the same pipeline only those that moved.
class DescriptorBinding {
public:
    static constexpr uint32_t kUbos = 32;

    bool changed(const void *pipeline, uint64_t address)
    {
        if (pipeline_ != pipeline)
            ubosBound_ = 0;
        if (pipeline_ == pipeline && address_ == address)
            return false;
        pipeline_ = pipeline;
        address_ = address;
        return true;
    }

    bool uboMoved(uint32_t ubo, const uint32_t *descriptor)
    {
        const std::array<uint32_t, 3> bound = {descriptor[0], descriptor[1], descriptor[2]};
        if (ubo >= kUbos)
            return true;
        if ((ubosBound_ >> ubo & 1) && ubos_[ubo] == bound)
            return false;
        ubos_[ubo] = bound;
        ubosBound_ |= 1u << ubo;
        return true;
    }

private:
    const void *pipeline_ = nullptr;
    uint64_t address_ = 0;
    uint32_t ubosBound_ = 0;
    std::array<std::array<uint32_t, 3>, kUbos> ubos_{};
};

}  // namespace raw
