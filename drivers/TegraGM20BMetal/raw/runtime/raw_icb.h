// SPDX-License-Identifier: MIT
// An indirect command buffer of draws that inherit their pipeline and buffers: each command is kSlotWords push words in GPU
// memory, written once as the CPU encodes the command, and a range of commands is one run of words the channel fetches by
// reference (RenderEncoder::executeSlots), after the encoder prepared the state they inherit. A command is its topology,
// its index buffer, and the gated draw macro (raw_mme.h) called with its index and Metal's indirect arguments inline: it
// draws as an indirect draw does (root draw parameters, base registers) where its index is in the range the execute put in
// the macros' scratch, and draws nothing elsewhere. A command with nothing encoded is no-ops. The macro call is at the
// same words in every command, so a copy to another index rewrites one word (kIndexWord). A buffer that may hold patch
// commands has kPatchSlotWords a command: a patch command is its topology and the patch macro called with its control-stage
// words, which load them for the state the execute's layout names (raw_mme.h kIcbPatches).
#pragma once
#include <cstdint>
#include "raw_mme.h"
#include "raw_push.h"

namespace raw::icb {

constexpr uint32_t kSlotWords = 18, kPatchSlotWords = 32, kCallWord = 10, kIndexWord = kCallWord + 1;
static_assert(kIndexWord + mme::kIcbPatchesArguments <= kPatchSlotWords);
constexpr uint32_t kNop = Push::hdr(4, SUBC_3D, NVB197_NO_OPERATION, 0);

struct Draw {
    uint32_t primitive;  // MTLPrimitiveType
    uint32_t count, instances, first, baseInstance;
};

struct IndexedDraw {
    uint32_t primitive;
    uint32_t indexSize;  // 2 or 4
    uint64_t indexVa, indexLast;
    uint32_t count, instances, firstIndex;
    int32_t baseVertex;
    uint32_t baseInstance;
};

// A patch command (RenderEncoder::icbPatchDraw makes one): each control-stage buffer's set-0 descriptor (base low, base
// high, range: PackTessBuffer order) and the control stage's push words (raw_pack.h kTessResidualWord, kTessScaleWord...)
struct PatchDraw {
    uint32_t count, instances, first, baseInstance;
    uint32_t buffers[3][3];
    uint32_t control[6];
};

// The raw path's primitive past MTLPrimitiveType's: a tessellation state's patches, each one control point of a patch list
// (m2v link/tessellation), which Metal's tessellator turns into triangles
constexpr uint32_t kPatchPrimitive = 5;

inline uint32_t topology(uint32_t primitive)
{
    // point, line, line strip, triangle, triangle strip, patch
    constexpr uint32_t kTopology[] = {1, 2, 3, 4, 5, 0xe};
    return primitive <= kPatchPrimitive ? kTopology[primitive] : 4;
}

inline bool restarts(uint32_t primitive)
{
    return primitive == 2 || primitive == 4;
}

// Metal biases the depth of triangles alone (RenderEncoder::rasterFor): commands of both kinds need different raster state
inline bool triangles(uint32_t primitive)
{
    return primitive == 3 || primitive == 4 || primitive == kPatchPrimitive;
}

inline void empty(uint32_t *slot, uint32_t words = kSlotWords)
{
    for (uint32_t i = 0; i < words; i++)
        slot[i] = kNop;
}

// The words before the call (`p` at the slot's first) padded to it
inline void padToCall(uint32_t *slot, Push &p)
{
    while (p.at < slot + kCallWord)
        *p.at++ = kNop;
}

inline void draw(uint32_t *slot, uint32_t index, const Draw &d, uint32_t words = kSlotWords)
{
    Push p{slot, slot + words};
    p.m(NVB197_SET_PRIMITIVE_TOPOLOGY, topology(d.primitive));
    p.m(NVB197_SET_DA_PRIMITIVE_RESTART, restarts(d.primitive));
    padToCall(slot, p);
    mme::icbDrawHeader(p, false);
    const uint32_t arguments[] = {index, d.count, d.instances, d.first, d.baseInstance};
    p.raw(arguments, 5);
    while (p.at < p.end)
        *p.at++ = kNop;
}

inline void indexed(uint32_t *slot, uint32_t index, const IndexedDraw &d, uint32_t words = kSlotWords)
{
    Push p{slot, slot + words};
    p.inc(NVB197_SET_INDEX_BUFFER_A,
          {uint32_t(d.indexVa >> 32), uint32_t(d.indexVa), uint32_t(d.indexLast >> 32), uint32_t(d.indexLast)});
    *p.at++ = Push::hdr(1, SUBC_3D, NVB197_SET_DA_PRIMITIVE_RESTART_INDEX, 1);
    *p.at++ = d.indexSize == 2 ? 0xffffu : 0xffffffffu;
    p.m(NVB197_SET_INDEX_BUFFER_E, d.indexSize == 2 ? 1 : 2);
    p.m(NVB197_SET_PRIMITIVE_TOPOLOGY, topology(d.primitive));
    p.m(NVB197_SET_DA_PRIMITIVE_RESTART, restarts(d.primitive));
    padToCall(slot, p);
    mme::icbDrawHeader(p, true);
    const uint32_t arguments[] = {index, d.count, d.instances, d.firstIndex, uint32_t(d.baseVertex), d.baseInstance};
    p.raw(arguments, 6);
    while (p.at < p.end)
        *p.at++ = kNop;
}

inline void patches(uint32_t *slot, uint32_t index, const PatchDraw &d)
{
    Push p{slot, slot + kPatchSlotWords};
    p.m(NVB197_SET_PRIMITIVE_TOPOLOGY, topology(kPatchPrimitive));
    p.m(NVB197_SET_DA_PRIMITIVE_RESTART, 0);
    padToCall(slot, p);
    mme::icbPatchesHeader(p);
    const uint32_t head[] = {index, d.count, d.instances};
    p.raw(head, 3);
    for (const auto &buffer : d.buffers)
        p.raw(buffer, 3);
    p.raw(d.control, 6);
    const uint32_t tail[] = {d.first, d.baseInstance};
    p.raw(tail, 2);
    while (p.at < p.end)
        *p.at++ = kNop;
}

}  // namespace raw::icb
