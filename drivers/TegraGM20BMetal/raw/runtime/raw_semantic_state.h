// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "raw_slot_snapshots.h"

namespace raw {

constexpr uint32_t kNoSnapshot = std::numeric_limits<uint32_t>::max();

template <typename T> class StateSnapshots {
    T current_{};
    uint64_t generation_ = 0;
    uint64_t captured_ = std::numeric_limits<uint64_t>::max();
    uint32_t index_ = kNoSnapshot;
    std::vector<T> values_;
    std::vector<uint32_t> changed_;  // each value's slots that may differ from the value before it
    uint32_t pending_ = ~0u;         // the slots edited since the last capture

public:
    const T &current() const
    {
        return current_;
    }

    const T &operator[](uint32_t index) const
    {
        return values_[index];
    }

    uint64_t generation() const
    {
        return generation_;
    }

    size_t size() const
    {
        return values_.size();
    }

    size_t storageBytes() const
    {
        return values_.capacity() * sizeof(T);
    }

    template <typename Edit> bool update(Edit edit)
    {
        if (!edit(current_))
            return false;
        ++generation_;
        pending_ = ~0u;
        return true;
    }

    // An edit of slot `slot` alone
    template <typename Edit> bool update(uint32_t slot, Edit edit)
    {
        if (!edit(current_))
            return false;
        ++generation_;
        pending_ |= slot < 32 ? 1u << slot : ~0u;
        return true;
    }

    uint32_t capture()
    {
        if (captured_ != generation_) {
            index_ = static_cast<uint32_t>(values_.size());
            values_.push_back(current_);
            changed_.push_back(pending_);
            pending_ = 0;
            captured_ = generation_;
        }
        return index_;
    }

    // The slots in which value `index` may differ from value `from`: exactly the edited ones where `index` was captured
    // next after `from`, every slot otherwise
    uint32_t changedFrom(uint32_t from, uint32_t index) const
    {
        return index == from + 1 ? changed_[index] : ~0u;
    }

    void resetCurrent()
    {
        current_ = {};
        ++generation_;
        pending_ = ~0u;
    }

    void clear()
    {
        values_.clear();
        changed_.clear();
        pending_ = ~0u;
        current_ = {};
        generation_ = 0;
        captured_ = std::numeric_limits<uint64_t>::max();
        index_ = kNoSnapshot;
    }
};

template <typename T> bool assignState(T &to, const T &from)
{
    if (to == from)
        return false;
    to = from;
    return true;
}

// A bind's attributeStride where it sets none (MTLAttributeStrideStatic)
constexpr uint32_t kStaticStride = UINT32_MAX;

struct BufferBinding {
    enum class Source : uint8_t {
        Unset,
        Buffer,
        Bytes,
        Offset,
        ClockAnchors  // offset: the kernel's clock anchor table's GPU address (OpKernelClockAnchors)
    };
    void *object = nullptr;
    uint64_t offset = 0;
    uint32_t chunk = 0;
    uint32_t length = 0;
    uint32_t stride =
        kStaticStride;  // the stride forms' attributeStride (setVertexBuffer:offset:attributeStride:atIndex:)
    Source source = Source::Unset;
    uint32_t shadow = 0;  // setBytes: its bytes' cached copy in the recording, + 1 (RMCommandBuffer::_shadow); 0: none

    bool operator==(const BufferBinding &b) const
    {
        return object == b.object && offset == b.offset && chunk == b.chunk && length == b.length &&
               stride == b.stride && source == b.source && shadow == b.shadow;
    }

    bool operator!=(const BufferBinding &b) const
    {
        return !(*this == b);
    }
};

using BufferBindings = std::array<BufferBinding, 31>;
using BufferSnapshots = SlotSnapshots<BufferBinding, 31>;

using TextureSnapshots = ObjectSnapshots<31>;
using SamplerSnapshots = ObjectSnapshots<16>;

struct RasterState {
    enum Field : uint32_t {
        Pipeline = 1u << 0,
        DepthStencil = 1u << 1,
        Scissor = 1u << 2,
        Viewport = 1u << 3,
        Cull = 1u << 4,
        Winding = 1u << 5,
        Fill = 1u << 6,
        Blend = 1u << 7,
        Stencil = 1u << 8,
        DepthBias = 1u << 9,
        Visibility = 1u << 10,
        DepthClip = 1u << 11,
        Amplification = 1u << 12,
    };

    uint32_t set = 0;
    void *pipeline = nullptr;
    void *nativePipeline = nullptr;
    void *nativeVariant = nullptr;
    void *depthStencil = nullptr;
    std::array<uint32_t, 4> scissor{};
    std::array<uint64_t, 6> viewport{};
    // setViewports:count: and setScissorRects:count: past one: their count and where the plan's viewArrays hold them
    // (six words a viewport from viewportsAt, four a rectangle from scissorsAt); index 0 is viewport and scissor
    uint16_t viewports = 1, scissors = 1;
    uint32_t viewportsAt = 0, scissorsAt = 0;
    std::array<uint32_t, 4> blend{};
    std::array<uint32_t, 3> depthBias{};
    std::array<uint32_t, 2> stencil{};
    uint32_t cull = 0, winding = 0, fill = 0, visibility = 0, depthClip = 0;
    uint64_t visibilityOffset = 0;
    // the vertex amplification count, then its view mappings' viewport and render target offsets
    std::array<uint32_t, 17> amplification{1};
};

static_assert(
    [] {
        constexpr uint32_t fields[] = {
            RasterState::Pipeline,      RasterState::DepthStencil, RasterState::Scissor,    RasterState::Viewport,
            RasterState::Cull,          RasterState::Winding,      RasterState::Fill,       RasterState::Blend,
            RasterState::Stencil,       RasterState::DepthBias,    RasterState::Visibility, RasterState::DepthClip,
            RasterState::Amplification,
        };
        uint32_t seen = 0;
        for (uint32_t f : fields) {
            if (f == 0 || (f & (f - 1)) != 0 || (seen & f) != 0)
                return false;
            seen |= f;
        }
        return true;
    }(),
    "each RasterState::Field is its own bit");

struct KernelState {
    void *pipeline = nullptr;
    bool pipelineSet = false;
    std::array<uint32_t, 31> memory{};
    uint32_t memorySet = 0;
};

struct RenderStateRef {
    uint64_t epoch = 0;
    uint64_t resourceRevision = 0;
    uint32_t buffers[2]{};
    uint32_t textures[2]{};
    uint32_t samplers[2]{};
    uint32_t raster = 0;
    uint32_t uses = 0;
    bool usesFinal = false;
};

struct KernelStateRef {
    uint32_t buffers = 0, textures = 0, samplers = 0, state = 0, uses = 0;
    uint64_t epoch = 0;
    uint64_t resourceRevision = 0;
};

struct ResourceUse {
    enum : uint8_t {
        Read = 1,
        Write = 2,
        Sample = 4,
        Explicit = 8
    };

    void *object = nullptr;
    uint8_t access = 0;
};

struct StateCursor {
    uint32_t render = kNoSnapshot;
    uint32_t compute = kNoSnapshot;
    // binding slots compared since the cursor was made, and the slots of the tables compared (a full comparison's)
    uint64_t slotsCompared = 0, slotsInTables = 0;

    void reset()
    {
        render = compute = kNoSnapshot;
    }
};

}  // namespace raw
