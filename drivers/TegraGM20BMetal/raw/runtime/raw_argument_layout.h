// SPDX-License-Identifier: GPL-2.0-or-later OR LGPL-3.0-or-later
// Where MTLArgumentEncoder writes each argument of a layout made from argument descriptors (newArgumentEncoderWithArguments:):
// metal2vulkan's bind/argument_buffer.rs (ArgumentBufferLayout::from_descriptors, place, type_size_align, metal_align),
// whose rules are Metal's answers on an M3 Max and an M4. The descriptors lay out as a C struct in the order given: every
// object is 8 bytes; a constant takes its MSL size and alignment, a vector of 3 the room of 4 and a matrix its columns'
// vectors; a run of constants is a block whose first may raise its alignment, stepped as (o + a - 1) & ~(a - 1) even where
// a is no power of two. The layout's alignment is the largest type alignment, its length the end rounded up to it. An index
// two descriptors claim is the later one's.
// A function's argument buffer (newArgumentEncoderWithBufferIndex:) is m2v's layout of its AIR as flat runs
// (bind/argument_runs.rs, fromRuns): a member covers `count` elements of `idsPerElement` ids, the first `width` of each
// arguments at offset + element * stride + w * elementStride, and a pointer to an argument buffer names that buffer's layout.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace raw {

enum class ArgumentKind : uint8_t {
    Buffer,
    Texture,
    Sampler,
    Constant,
    IndirectCommandBuffer,
    ComputePipeline,
    RenderPipeline,
    VisibleFunctionTable,
    IntersectionFunctionTable,
    AccelerationStructure,
    DepthStencilState,
};

constexpr uint64_t kArgumentHandleBytes = 8;

// One element of an argument's type: a handle, or a constant of `columns` vectors of `rows` scalars of `scalar` bytes
struct ArgumentType {
    ArgumentKind kind = ArgumentKind::Buffer;
    uint8_t scalar = 0, columns = 1, rows = 1;

    uint64_t alignment() const
    {
        return kind == ArgumentKind::Constant ? uint64_t(scalar) * (rows == 3 ? 4 : rows) : kArgumentHandleBytes;
    }

    uint64_t size() const
    {
        return kind == ArgumentKind::Constant ? alignment() * columns : kArgumentHandleBytes;
    }
};

struct ArgumentDescription {
    ArgumentType type;
    uint32_t index = 0, arrayLength = 0, constantBlockAlignment = 0;
};

struct ArgumentMember {
    uint32_t id = 0, count = 1;
    uint64_t offset = 0, size = 0;
    ArgumentKind kind = ArgumentKind::Buffer;
    uint32_t idsPerElement = 1, width = 1;
    uint64_t stride = 0, elementStride = 0;  // stride 0: size
    int32_t nested = -1;                     // a pointer's argument buffer: its layout's index in the encoder's list
};

struct ArgumentPlace {
    ArgumentKind kind = ArgumentKind::Buffer;
    uint64_t offset = 0, size = 0;
    int32_t nested = -1;
};

// One flat run of a function's layout as the bridge gives it (bridge/NativeMetalObjects.h NmbArgumentRun)
struct ArgumentRun {
    uint32_t id, count, idsPerElement, width, kind, nested;
    uint64_t offset, stride, elementStride, size;
};

enum class ArgumentLayoutError : uint8_t {
    None,
    NoArguments,
    ConstantBlockAlignment,
    TooLarge
};

inline bool metalAlign(uint64_t offset, uint64_t a, uint64_t &out)
{
    a = std::max<uint64_t>(a, 1);
    if (offset > UINT64_MAX - (a - 1))
        return false;
    out = (offset + a - 1) & ~(a - 1);
    return true;
}

struct ArgumentLayout {
    uint64_t encodedLength = 0, alignment = 1;
    std::vector<ArgumentMember> members;

    // `position`: the description a refusal names
    static ArgumentLayoutError fromDescriptions(const std::vector<ArgumentDescription> &descriptions,
                                                ArgumentLayout &out, size_t &position)
    {
        out = {};
        if (descriptions.empty())
            return ArgumentLayoutError::NoArguments;
        uint64_t cursor = 0;
        bool inBlock = false;
        for (position = 0; position < descriptions.size(); position++) {
            const ArgumentDescription &d = descriptions[position];
            const uint64_t natural = d.type.alignment(), size = d.type.size();
            uint64_t align = natural;
            if (d.type.kind == ArgumentKind::Constant) {
                if (d.constantBlockAlignment) {
                    if (inBlock)
                        return ArgumentLayoutError::ConstantBlockAlignment;
                    align = std::max<uint64_t>(align, d.constantBlockAlignment);
                }
                inBlock = true;
            } else {
                inBlock = false;
            }
            const uint32_t count = std::max<uint32_t>(d.arrayLength, 1);
            uint64_t offset;
            if (!metalAlign(cursor, align, offset) || size > (UINT64_MAX - offset) / count)
                return ArgumentLayoutError::TooLarge;
            cursor = offset + size * count;
            out.alignment = std::max(out.alignment, natural);
            out.members.push_back({d.index, count, offset, size, d.type.kind});
        }
        return metalAlign(cursor, out.alignment, out.encodedLength) ? ArgumentLayoutError::None
                                                                    : ArgumentLayoutError::TooLarge;
    }

    // A function's layout from its runs; false: a run of no argument kind, or a nested layout past `layouts` of them
    static bool fromRuns(uint64_t encodedLength, uint64_t alignment, const ArgumentRun *runs, uint32_t count,
                         uint32_t layouts, ArgumentLayout &out)
    {
        out = {};
        out.encodedLength = encodedLength;
        out.alignment = std::max<uint64_t>(alignment, 1);
        for (uint32_t i = 0; i < count; i++) {
            const ArgumentRun &r = runs[i];
            if (r.kind > uint32_t(ArgumentKind::DepthStencilState) || r.nested > layouts)
                return false;
            ArgumentMember m;
            m.id = r.id;
            m.count = r.count;
            m.offset = r.offset;
            m.size = r.size;
            m.kind = ArgumentKind(r.kind);
            m.idsPerElement = std::max<uint32_t>(r.idsPerElement, 1);
            m.width = r.width;
            m.stride = r.stride;
            m.elementStride = r.elementStride;
            m.nested = int32_t(r.nested) - 1;
            out.members.push_back(m);
        }
        return true;
    }

    // Where argument `index` lands: in the last member whose indices hold it
    bool place(uint32_t index, ArgumentPlace &out) const
    {
        for (auto m = members.rbegin(); m != members.rend(); ++m) {
            if (index < m->id)
                continue;
            const uint32_t rel = index - m->id, element = rel / m->idsPerElement, within = rel % m->idsPerElement;
            if (element >= m->count || within >= m->width)
                continue;
            const uint64_t stride = m->stride ? m->stride : m->size;
            out = {m->kind, m->offset + stride * element + m->elementStride * within, m->size, m->nested};
            return true;
        }
        return false;
    }
};

}  // namespace raw
