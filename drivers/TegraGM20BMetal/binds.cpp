// SPDX-License-Identifier: MIT
// See render_encoder.h (RenderEncoder's buffer, viewport, scissor and state setters).
#include "lock.h"
#include "gpu.h"

#include <algorithm>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

void RenderEncoder::setBytes(int stage, uint32_t slot, const void *bytes, uint32_t length)
{
    SWITCH_METAL_ENCODE_SPAN(Uniforms);
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    uint64_t va;
    const uint32_t chunk = (length + kUboChunk - 1) & ~(kUboChunk - 1);
    uint8_t *m = dev_->alloc(dev_->upload_, chunk ? chunk : kUboChunk, kUboChunk, va);
    memcpy(m, bytes, length);
    memset(m + length, 0, (chunk ? chunk : kUboChunk) - length);
    bufs_[stage][slot] = {va, length, nullptr, 0,
                          m};  // m2v binds the Metal length (its upload chunks are 256-byte aligned)
    bufs_[stage][slot].resourceVa = dev_->upload_.bo.va;
    bufs_[stage][slot].named = true;
    bufferMoved(stage, slot);
    SWITCH_METAL_DIAGNOSTIC(cb_->counts_.uploadBytes += chunk);
}

void RenderEncoder::setBytesAt(int stage, uint32_t slot, uint64_t va, uint32_t length, const uint8_t *cpu,
                               const Buffer *allocation)
{
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    const Bound &old = bufs_[stage][slot];
    const uint64_t resourceVa = allocation ? allocation->va : va;
    if (old.va == va && old.size == length && !old.buffer && old.cpu == cpu && old.resourceVa == resourceVa)
        return;
    bufs_[stage][slot] = {va, length, nullptr, 0, cpu, resourceVa, allocation != nullptr};
    if (allocation) {
        cb_->use(*allocation, false);
        if (allocation->hostVisible && !allocation->wc)
            cb_->clean(cpu, length);
    }
    bufferMoved(stage, slot);
}

void RenderEncoder::setBuffer(int stage, uint32_t slot, const Buffer *b, uint64_t offset)
{
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    const Bound next =
        b ? Bound{b->va + offset, uint32_t(std::min<uint64_t>(b->length - std::min(offset, b->length), 0xffffffffu)), b,
                  offset, b->cpu && offset < b->length ? b->cpu + offset : nullptr}
          : Bound{};
    const Bound &old = bufs_[stage][slot];
    if (old.va == next.va && old.size == next.size && old.buffer == next.buffer && old.offset == next.offset &&
        old.cpu == next.cpu)
        return;
    bufs_[stage][slot] = next;
    bufferMoved(stage, slot);
}

void RenderEncoder::setBufferOffset(int stage, uint32_t slot, uint64_t offset)
{
    if (slot >= 31 || stage < 1 || stage > 2 || !bufs_[stage][slot].buffer)
        return;
    setBuffer(stage, slot, bufs_[stage][slot].buffer, offset);
}

void RenderEncoder::setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (reachStep_) {
        reachScissor_[0] = x, reachScissor_[1] = y, reachScissor_[2] = w, reachScissor_[3] = h;
        const double *v = reachViewport_;
        if (scissorOutsideViewport(x, y, w, h, v[0], v[1], v[2], v[3]))
            w = h = 0;
        const Rect r = reachScissor(x, y, w, h, reachW_, reachH_, reachStep_);
        x = r.x0, y = r.y0, w = r.x1 - r.x0, h = r.y1 - r.y0;
    }
    // a rectangle past the render area is clipped to it, as plain Metal clips it (m2v render_state.rs `set_scissors`)
    const uint32_t x0 = std::min(x, areaW_), y0 = std::min(y, areaH_);
    w = uint32_t(std::min<uint64_t>(uint64_t(x) + w, areaW_)) - x0;
    h = uint32_t(std::min<uint64_t>(uint64_t(y) + h, areaH_)) - y0;
    x = x0;
    y = y0;
    sc_[0] = x;
    sc_[1] = y;
    sc_[2] = w;
    sc_[3] = h;
    scissor_[0] = (x + w) << 16 | x;
    scissor_[1] = (y + h) << 16 | y;
    scissorDirty_ = true;
    views_.scissors = 1;
}

// Metal's viewport as m2v gives it to Vulkan (y flipped: y + h, height -h) and NVK writes it (nvk_emit_viewport)
void RenderEncoder::setViewport(double x, double y, double w, double h, double znear, double zfar)
{
    const Rect reachClip = reachStep_ ? reachViewportClip(x, y, w, h, reachStep_) : Rect{};
    if (reachStep_) {
        reachViewport_[0] = x, reachViewport_[1] = y, reachViewport_[2] = w, reachViewport_[3] = h;
        const double s = 1.0 / reachStep_;
        x *= s, y *= s, w *= s, h *= s;
    }
    depthRange_[0] = float(znear);
    depthRange_[1] = float(zfar);
    spillDirty_ = true;
    vp_[0] = x;
    vp_[1] = y;
    vp_[2] = w;
    vp_[3] = h;
    const float vy = float(y + h), vh = float(-h);
    viewport_[0] = fbits(0.5f * float(w));
    viewport_[1] = fbits(0.5f * vh);
    viewport_[2] = fbits(float(zfar - znear));
    viewport_[3] = fbits(float(x) + 0.5f * float(w));
    viewport_[4] = fbits(vy + 0.5f * vh);
    viewport_[5] = fbits(float(znear));
    const float xmin = std::clamp(float(x), 0.0f, 65535.0f), xmax = std::clamp(float(x + w), 0.0f, 65535.0f);
    const float ymin = std::clamp(std::min(vy, vy + vh), 0.0f, 65535.0f),
                ymax = std::clamp(std::max(vy, vy + vh), 0.0f, 65535.0f);
    viewport_[6] = uint32_t(xmax - xmin) << 16 | uint32_t(xmin);
    viewport_[7] = uint32_t(ymax - ymin) << 16 | uint32_t(ymin);
    if (reachStep_) {
        viewport_[6] = (reachClip.x1 - reachClip.x0) << 16 | reachClip.x0;
        viewport_[7] = (reachClip.y1 - reachClip.y0) << 16 | reachClip.y0;
        const uint32_t *c = reachScissor_;
        setScissor(c[0], c[1], c[2], c[3]);
    }
    viewportDirty_ = true;
    views_.viewports = 1;
}

void RenderEncoder::setViewports(const double (*viewports)[6], uint32_t count)
{
    count = std::clamp(count, 1u, kMaxViewports);
    setViewport(viewports[0][0], viewports[0][1], viewports[0][2], viewports[0][3], viewports[0][4], viewports[0][5]);
    for (uint32_t j = 1; j < count; j++) {
        viewportWords(viewports[j], views_.viewport[j]);
        std::copy_n(viewports[j], 4, views_.vp[j]);
    }
    views_.dirtyViewports |= ((1u << count) - 1) & ~1u;
    views_.viewports = count;
}

void RenderEncoder::setScissors(const uint32_t (*rects)[4], uint32_t count)
{
    count = std::clamp(count, 1u, kMaxViewports);
    setScissor(rects[0][0], rects[0][1], rects[0][2], rects[0][3]);
    for (uint32_t j = 1; j < count; j++)
        views_.scissor[j] = scissorBox(rects[j], areaW_, areaH_);
    views_.dirtyScissors |= ((1u << count) - 1) & ~1u;
    views_.scissors = count;
}

void RenderEncoder::setBlendColor(const float rgba[4])
{
    uint32_t v[4];
    memcpy(v, rgba, sizeof v);
    if (memcmp(v, blendColor_, sizeof v)) {
        memcpy(blendColor_, v, sizeof v);
        blendColorDirty_ = true;
        spillDirty_ = true;
    }
}

void RenderEncoder::setDepthStencil(const DepthStencilState *s)
{
    if (reachStep_)
        return;  // a reach target has no depth or stencil
    dsState_ = s ? s : &DepthStencilState::metalDefault();
    ds_ = dsState_->desc();
    compatible_ = nullptr;
    dsDirty_ = true;
    spillDirty_ = true;
}

void RenderEncoder::setStencilReference(uint32_t front, uint32_t back)
{
    stencilRef_[0] = front;
    stencilRef_[1] = back;
    dsDirty_ = true;
    spillDirty_ = true;
}

void RenderEncoder::setDepthBias(float constant, float slope, float clamp)
{
    depthBias_ = depthBiasFactors(constant, slope, clamp);
}

}  // namespace gm20b
