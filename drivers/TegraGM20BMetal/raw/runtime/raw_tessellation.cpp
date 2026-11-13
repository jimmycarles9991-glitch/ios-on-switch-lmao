// SPDX-License-Identifier: MIT
// A tessellation state's draws (raw_gpu.h RenderEncoder): each patch one control point of a PATCH_LIST of one (m2v
// link/tessellation), drawn by the hardware tessellator through the state's control and evaluation programs. The control
// stage reads the encoder's factor buffer and the draw's patch and control-point index buffers through set 0
// (EntryTessBuffer), and its push words (PackPipeline::controlWord) as m2v's binder writes them (bind/graphics.rs
// patch_draw). An indirect draw's MTLDrawPatchIndirectArguments are a non-indexed draw's: the channel's MME reads them.
// An indirect command buffer's patch commands carry the same words for their own buffers (icbPatchDraw); the execute
// names where its state reads them (icbPatchLayout), and the GPU loads each command's before it draws.
#include <algorithm>
#include <cstring>
#include "raw_gpu.h"
#include "raw_icb.h"
#include "raw_residual.h"

namespace raw {
namespace {

constexpr uint16_t kHalfInfinity = 0x7c00;
// m2v INFINITE_SCALE: takes every positive half factor past the largest level, finitely
constexpr float kInfiniteScale = 4294967296.0f;

}  // namespace

bool tessellationScaleBits(float scale, uint32_t &bits)
{
    const _Float16 half = static_cast<_Float16>(scale);
    uint16_t halfBits;
    memcpy(&halfBits, &half, sizeof halfBits);
    if (halfBits < 1 || halfBits > kHalfInfinity)
        return false;
    const float pushed = halfBits == kHalfInfinity ? kInfiniteScale : static_cast<float>(half);
    memcpy(&bits, &pushed, sizeof bits);
    return true;
}

RenderEncoder::Bound RenderEncoder::boundFrom(const Buffer *buffer, uint64_t offset)
{
    if (!buffer || !buffer->va || offset >= buffer->length)
        return {};
    return {buffer->va + offset, uint32_t(std::min<uint64_t>(buffer->length - offset, 0xffffffffu)), buffer, offset,
            buffer->cpu ? buffer->cpu + offset : nullptr};
}

void RenderEncoder::setTessellationFactors(const Buffer *buffer, uint64_t offset, uint64_t instanceStride)
{
    const Bound next = boundFrom(buffer, offset & ~uint64_t(1));
    const uint32_t stride = next.va ? uint32_t(instanceStride & ~uint64_t(3)) : 0;
    Bound &factors = tess_.buffers[TessFactors];
    if (factors.va == next.va && factors.size == next.size && factors.buffer == next.buffer &&
        tess_.instanceStride == stride)
        return;
    factors = next;
    tess_.instanceStride = stride;
    tessWordsDirty_ = true;
    invalidateDescriptors();
}

void RenderEncoder::setTessellationFactorScale(float scale)
{
    uint32_t bits = tess_.scaleBits;
    const bool valid = tessellationScaleBits(scale, bits);
    if (valid == tess_.scaleValid && bits == tess_.scaleBits)
        return;
    tess_.scaleValid = valid;
    tess_.scaleBits = bits;
    tessWordsDirty_ = true;
}

// The draw's patch and control-point index buffers into set 0, and whether it binds a patch index buffer into the control
// stage's words; false: a draw the state does not take (`why`), as m2v's binder refuses it. As Metal draws them (m2v
// render.rs draw_patches, measured): with the state's control points whatever the draw names, the patch indices from
// their offset rounded down to 4, every control point's index 0 where the state has an index type and the draw binds no
// control-point index buffer (the zero page), and one the draw binds unread where the state has none
bool RenderEncoder::bindPatches(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                                const Buffer *pointIndices, uint64_t pointIndexOffset, const char **why)
{
    (void)controlPoints;
    const Pipeline *pl = formFor(pipeline_);
    if (!pl || !tessellates(*pl->rec)) {
        *why = "patches drawn with a state of no post-tessellation vertex function";
        return false;
    }
    const PackPipeline &r = *pl->rec;
    if (!r.controlPointIndexBytes)
        pointIndices = nullptr;
    if (pointIndices && pointIndexOffset % r.controlPointIndexBytes) {
        *why = "control-point indices at an offset that is not a multiple of their size";
        return false;
    }
    patchIndexOffset &= ~uint64_t(3);
    if (!tess_.scaleValid) {
        *why = "a tessellation factor scale that is not a positive half";
        return false;
    }
    const Bound patches = boundFrom(patchIndices, patchIndexOffset), points = boundFrom(pointIndices, pointIndexOffset);
    const uint32_t indexed = patches.va ? 1 : 0;
    Bound &p = tess_.buffers[TessPatchIndices], &c = tess_.buffers[TessControlPointIndices];
    if (p.va != patches.va || p.size != patches.size || c.va != points.va || c.size != points.size) {
        p = patches;
        c = points;
        invalidateDescriptors();
    }
    if (indexed != tess_.indexed) {
        tess_.indexed = indexed;
        tessWordsDirty_ = true;
    }
    return true;
}

bool RenderEncoder::drawPatches(uint32_t controlPoints, uint32_t start, uint32_t count, uint32_t instances,
                                uint32_t baseInstance, const Buffer *patchIndices, uint64_t patchIndexOffset,
                                const Buffer *pointIndices, uint64_t pointIndexOffset, const char **why)
{
    if (!pipeline_)
        return true;
    if (!bindPatches(controlPoints, patchIndices, patchIndexOffset, pointIndices, pointIndexOffset, why))
        return false;
    return draw(icb::kPatchPrimitive, start, count, instances, baseInstance, why);
}

bool RenderEncoder::drawPatchesIndirect(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                                        const Buffer *pointIndices, uint64_t pointIndexOffset, const Buffer *arguments,
                                        uint64_t offset, const char **why)
{
    if (!pipeline_)
        return true;
    if (!bindPatches(controlPoints, patchIndices, patchIndexOffset, pointIndices, pointIndexOffset, why))
        return false;
    return indirect(icb::kPatchPrimitive, false, arguments, offset, why);
}

// The set-0 descriptor source of tessellation buffer `which`: the zero page where none is bound, whose factors cull every
// patch and whose indices read 0, as m2v binds its zero buffer there
RenderEncoder::Bound RenderEncoder::tessBuffer(uint8_t which) const
{
    if (which < TessBufferCount && tess_.buffers[which].va)
        return tess_.buffers[which];
    Bound zero;
    zero.va = dev_->zero_.va;
    zero.size = uint32_t(dev_->zero_.bytes);
    return zero;
}

// The control stage's words m2v's binder pushes besides the buffers' residuals
void RenderEncoder::controlWords(const PackPipeline &r, uint32_t (&want)[32]) const
{
    const uint32_t first = r.controlWord - 1u;
    want[first + kTessInstanceStrideWord] = tess_.instanceStride;
    want[first + kTessScaleWord] = tess_.scaleBits;
    want[first + kTessIndexedWord] = tess_.indexed;
}

// As setTessellationFactors: and bindPatches bind an encoder's draw: the factors from their offset's low 32 bits rounded
// down to 2 (m2v keeps a command's offsets so) and the stride down to 4, the patch indices from theirs rounded down to 4;
// a buffer the command names none of, or one past its bytes, is the zero page
bool RenderEncoder::icbPatchDraw(const Device &dev, const IcbPatches &in, icb::PatchDraw &out, const char **why)
{
    uint32_t scale = 0;
    if (!tessellationScaleBits(in.scale, scale)) {
        *why = "a tessellation factor scale that is not a positive half";
        return false;
    }
    const Bound bound[TessBufferCount] = {boundFrom(in.factors, uint32_t(in.factorOffset) & ~1u),
                                          boundFrom(in.patchIndices, uint32_t(in.patchIndexOffset) & ~3u),
                                          boundFrom(in.pointIndices, uint32_t(in.pointIndexOffset))};
    out = {in.count, in.instances, in.first, in.baseInstance, {}, {}};
    for (uint8_t k = 0; k < TessBufferCount; k++) {
        const bool zero = !bound[k].va;
        const uint64_t va = zero ? dev.zero_.va : bound[k].va;
        const uint32_t size = zero ? uint32_t(dev.zero_.bytes) : bound[k].size;
        BufferDescriptor d;
        bufferDescriptor(true, 0, 0, va, size, d);
        out.buffers[k][0] = uint32_t(d.base);
        out.buffers[k][1] = uint32_t(d.base >> 32);
        out.buffers[k][2] = d.range;
        out.control[kTessResidualWord[k]] = d.residual;
    }
    out.control[kTessInstanceStrideWord] = bound[TessFactors].va ? uint32_t(in.factorStride & ~uint64_t(3)) : 0;
    out.control[kTessScaleWord] = scale;
    out.control[kTessIndexedWord] = bound[TessPatchIndices].va ? 1 : 0;
    return true;
}

// The control stage reads set 0 through the constant buffer it binds it at (Pipeline::setSlot[2]): a constant buffer load
// is ordered with the draws around it there, not for a program that reads the set through its address in the root table
bool RenderEncoder::icbPatchLayout(const char **why)
{
    const Pipeline &pl = *prepared_;
    const PackPipeline &r = *pl.rec;
    if (!pl.setSlot[2]) {
        *why =
            "patch commands of a state whose control stage reads set 0 by its address, which no constant buffer load "
            "orders";
        cb_->refusal = {};
        return false;
    }
    if (renderSetBytes(r.setBytes) > mme::kIcbSetBytes) {
        *why = "patch commands of a state whose set 0 is larger than the patch macro loads it at";
        cb_->refusal = {};
        return false;
    }
    uint32_t entries[TessBufferCount] = {mme::kNoEntry, mme::kNoEntry, mme::kNoEntry};
    for (uint32_t i = 0; i < r.entries; i++)
        if (r.entry[i].kind == EntryTessBuffer && r.entry[i].slot < TessBufferCount)
            entries[r.entry[i].slot] = r.entry[i].offset;
    cb_->useAddress(dev_->zero_.va, uint32_t(dev_->zero_.bytes), false);
    if (p_.room() < 16)
        reserve();
    mme::icbPatchLayout(p_, preparedSet_, entries, 0x700 + 4 * (r.controlWord - 1u));
    return true;
}

void RenderEncoder::icbPatchesDone()
{
    const uint32_t first = prepared_->rec->controlWord - 1u;
    constexpr uint32_t kWords = sizeof(icb::PatchDraw::control) / sizeof(uint32_t);
    if (p_.room() < 16)
        reserve();
    p_.m(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, 0x700 + 4 * first);
    p_.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), cb_->rootPush_ + first, kWords);
    descriptors_.forget();
    invalidateDescriptors();
}

}  // namespace raw
