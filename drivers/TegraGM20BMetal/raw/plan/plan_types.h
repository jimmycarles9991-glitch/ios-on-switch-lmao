// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace raw::plan {

constexpr uint32_t kNone = UINT32_MAX;

// An L3 resource: its slot in the resource table and the generation that slot had when recorded
struct Handle {
    uint32_t index = kNone, generation = 0;

    bool valid() const
    {
        return index != kNone;
    }

    bool operator==(const Handle &o) const
    {
        return index == o.index && generation == o.generation;
    }

    bool operator!=(const Handle &o) const
    {
        return !(*this == o);
    }
};

// The engine a node runs on. Every class but VIC and the display is a subchannel of the one GPU channel (gpu.c binds
// all five per channel), so the engine picks the barrier vocabulary, not a queue.
enum class Engine : uint8_t {
    None,
    ThreeD,
    TwoD,
    Copy,
    Compute,
    Inline,
    Vic,
    Display
};

// The way an access reaches memory, which decides the flush its write needs and the cache its read must drop
enum class Path : uint8_t {
    Rop,       // colour target loads, blends, partial masks and writes, clears
    Zrop,      // depth/stencil target accesses
    Texture,   // texture-unit reads: sampling, a fetch through the barrier form, the 2D engine's source
    Storage,   // shader loads and stores (the interlock's storage form, storage buffers and images)
    Constant,  // constant-buffer reads
    Vertex,    // vertex and index fetch
    Indirect,  // arguments read by the macro unit and the push fetcher (indirect draws)
    Copy,      // the copy engine
    TwoD,      // the 2D engine's destination writes
    Inline,    // inline-to-memory writes
    Display,   // the display controller after the job (present)
    Count
};

constexpr uint32_t kPathCount = uint32_t(Path::Count);

enum class Stage : uint8_t {
    Vertex,
    Fragment,
    Compute,
    Transfer
};

// Levels [level, level + levels) and layers [layer, layer + layers); the defaults name the whole resource
struct SubRange {
    uint16_t level = 0, levels = UINT16_MAX;
    uint16_t layer = 0, layers = UINT16_MAX;

    bool overlaps(const SubRange &o) const
    {
        const auto within = [](uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
            return a < b + m && b < a + n;
        };
        return within(level, levels, o.level, o.levels) && within(layer, layers, o.layer, o.layers);
    }
};

// A resource use by its recording slot (Recording::resources): the planner never holds an object pointer
struct Use {
    uint32_t slot = kNone;
    SubRange range;
    Path path = Path::Texture;
    Stage stage = Stage::Fragment;
    bool write = false;
};

}  // namespace raw::plan
