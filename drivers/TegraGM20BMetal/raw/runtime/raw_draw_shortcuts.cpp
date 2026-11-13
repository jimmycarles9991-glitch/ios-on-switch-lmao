// SPDX-License-Identifier: MIT
// A draw's fixed-function shortcuts behind their rules (raw_constant_colour.h, raw_quad_fill.h, raw_blur_twod.h): each
// reads only what the CPU owns (setVertexBytes and setFragmentBytes uploads, indices under the command buffer's lease)
// and takes the draw's own route where anything it needs is missing.
#include <cmath>
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmit2D.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h"
#include "raw_blur_twod.h"
#include "raw_ca_route.h"
#include "raw_constant_colour.h"
#include "raw_draw_hooks.h"
#include "raw_gpu.h"
#include "raw_kit.h"
#include "raw_quad_fill.h"

namespace raw {

namespace {

bool bindsTexture(const PackPipeline &rec)
{
    for (uint32_t k = 0; k < rec.entries && k < kPackEntries; k++)
        if (rec.entry[k].kind == EntryTexture || rec.entry[k].kind == EntryTexture3d)
            return true;
    return false;
}

}  // namespace

const char *Device::packKey(const PackPipeline &rec) const
{
    const PackPipeline *packed = pack_.section<PackPipeline>(PackPipelines);
    const bool inPack = packed && &rec >= packed && &rec < packed + pack_.bytes(PackPipelines) / sizeof(PackPipeline);
    return inPack ? pack_.string(rec.keyString) : nullptr;
}

// The vertex stream from the draw's first vertex where setVertexBytes uploaded it, with the state's position and colour
bool RenderEncoder::cpuStream(const Pipeline &pl, uint32_t first, FillStream &s) const
{
    const PackPipeline &rec = *pl.rec;
    const Bound &stream = bufs_[1][rec.vertexSlot < 31 ? rec.vertexSlot : 0];
    const uint64_t skip = uint64_t(first) * rec.vertexStride;
    if (stream.buffer || !stream.cpu || !rec.vertexStride || skip >= stream.size || !pl.positionComponents)
        return false;
    s.bytes = stream.cpu + skip;
    s.size = stream.size - skip;
    s.stride = rec.vertexStride;
    s.positionOffset = pl.positionOffset;
    s.positionComponents = pl.positionComponents;
    s.colorOffset = fillColorOffset(dev_->packKey(rec));
    return true;
}

// The CPU's copy of the draw's first `count` indices where its lease covers them
bool RenderEncoder::cpuIndices(uint32_t count, uint32_t out[6]) const
{
    if (!drawIndices_ || count > 6 ||
        (!drawIndexCpuSafe_ && (!drawIndexBuffer_ || !cb_->acquireGeometry(*drawIndexBuffer_, drawIndices_,
                                                                           uint64_t(count) * drawIndexSize_))))
        return false;
    for (uint32_t i = 0; i < count; i++)
        out[i] = drawIndexSize_ == 2 ? reinterpret_cast<const uint16_t *>(drawIndices_)[i]
                                     : reinterpret_cast<const uint32_t *>(drawIndices_)[i];
    return true;
}

RenderEncoder::Shortcut RenderEncoder::shortcut(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count,
                                                bool indexed)
{
    Shortcut out;
    if (!(ropConstantColour() || fillViaTriangleBox()) || CaRoute(pl.caRoute) != CaRoute::Fill || !pl.rec)
        return out;
    FillStream s;
    if (!cpuStream(pl, indexed ? 0 : first, s))
        return out;
    uint32_t list[6];
    const bool quadList = primitive == 3 && count == 6, quadStrip = primitive == 4 && count == 4;
    if (indexed && (!quadList || !cpuIndices(6, list)))
        return out;
    if (!indexed && quadList)
        for (uint32_t i = 0; i < 6; i++)
            list[i] = i;
    const Bound &matrix = bufs_[1][2];
    const uint8_t *m = !matrix.buffer && matrix.cpu && matrix.size >= 64 ? matrix.cpu : nullptr;
    FillDraw d;
    d.fillState = true;
    d.samplesTexture = bindsTexture(*pl.rec);
    d.shapesCoverage = pl.dropsPixels;
    d.oneTarget = pass_.count == 1 && pl.writeBits == 1;
    d.oneColour = uniformColour(s, quadList ? list : nullptr, quadList ? 6 : count, out.rgba);
    out.constant = constantColourFill(d);
    uint32_t corners[4] = {0, 1, 2, 3};
    float window[8];
    if (fillViaTriangleBox() && (quadStrip || (quadList && quadOfIndices(list, corners))) &&
        quadWindow(s, corners, m, vp_, window)) {
        const float swapped[8] = {window[0], window[1], window[4], window[5],
                                  window[2], window[3], window[6], window[7]};
        out.bbox = axisAlignedQuad(window) || axisAlignedQuad(swapped);
    }
    return out;
}

void RenderEncoder::shortcutBegin(const Shortcut &s)
{
    if (s.constant)
        kit(p_, switch_gm20b_constant_color, s.rgba);
    if (s.bbox)
        kit(p_, switch_gm20b_fill_via_triangle, uint32_t(NVB197_SET_FILL_VIA_TRIANGLE_MODE_FILL_BBOX));
}

void RenderEncoder::shortcutEnd(const Shortcut &s)
{
    if (s.constant)
        kit(p_, switch_gm20b_constant_color_off);
    if (s.bbox)
        kit(p_, switch_gm20b_fill_via_triangle, uint32_t(NVB197_SET_FILL_VIA_TRIANGLE_MODE_DISABLED));
}

namespace {

switch_gm20b_2d_surface twodSurface(const Texture &t, uint32_t fmt2d)
{
    switch_gm20b_2d_surface s = {};
    s.address = t.va;
    s.format = fmt2d;
    s.pitch = !t.blockLinear;
    s.pitch_bytes = t.pitch;
    s.block_height_log2 = t.blockHeight;
    s.width = t.width;
    s.height = t.height;
    s.compressed = t.comptags;
    return s;
}

}  // namespace

// A blur-chain pass drawn as 2:1 bilinear blits (raw_blur_twod.h): its one target covered by the draw's quad, whose
// texture coordinates (after texmat0, vertex buffer 3: uv * xy + zw) run 0 to 1 across it, one bilinear clamped source
// of the target's format, the fragment uniform's taps a 2^n box and edr_scale (fragment buffer 6) 1. Steps past the first
// halve into pitch scratch from the command buffer's upload ring.
bool RenderEncoder::blurTwod(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count, bool indexed)
{
    const BlurFunction f = BlurFunction(pl.blurFunction);
    const Texture *target = pass_.count == 1 ? pass_.color[0].texture : nullptr;
    const bool strip = primitive == 4 && count == 4, list = primitive == 3 && count == 6;
    if (!blurTwodRoute() || f == BlurFunction::None || !pl.rec || !target || target->samples > 1 || !(strip || list) ||
        memoryless_[0])
        return false;
    uint32_t corners[4] = {0, 1, 2, 3}, indices[6] = {0, 1, 2, 3, 4, 5};
    if (list && ((indexed && !cpuIndices(6, indices)) || !quadOfIndices(indices, corners)))
        return false;
    const PackPipeline &rec = *pl.rec;
    const Texture *source = nullptr;
    const Sampler *sampler = nullptr;
    for (uint32_t k = 0; k < rec.entries && k < kPackEntries; k++)
        if (const PackEntry &e = rec.entry[k]; e.kind == EntryTexture) {
            source = boundTexture(e);
            sampler = e.samplerKind == SamplerSlot ? boundSampler(e) : nullptr;
        }
    const Bound &u = bufs_[2][1], &edr = bufs_[2][6], &texmat = bufs_[1][3];
    if (!source || !sampler || source->samples > 1 || source->format != target->format || u.buffer || !u.cpu ||
        u.size < 80 || edr.buffer || !edr.cpu || edr.size < 2 || texmat.buffer || !texmat.cpu || texmat.size < 16)
        return false;
    const SamplerDesc &sd = sampler->desc;
    _Float16 scale;
    memcpy(&scale, edr.cpu, sizeof scale);
    if (sd.minFilter != 1 || sd.magFilter != 1 || sd.mipFilter || sd.sAddress || sd.tAddress || sd.pixel ||
        scale != _Float16(1))
        return false;
    const uint32_t steps =
        blurTwodSteps(f, blurUniform(u.cpu), source->width, source->height, target->width, target->height);
    const uint32_t fmt2d = fx::twodFormat(target->format);
    if (!steps || !fmt2d || !blurTwodFormat(target->format, steps))
        return false;
    FillStream s;
    const int32_t uvOffset = vertexAttributeOffset(dev_->packKey(rec), 1, 29);  // texcoord0, MTLVertexFormatFloat2
    if (!cpuStream(pl, indexed ? 0 : first, s) || uvOffset < 0)
        return false;
    const Bound &matrix = bufs_[1][2];
    const uint8_t *m = !matrix.buffer && matrix.cpu && matrix.size >= 64 ? matrix.cpu : nullptr;
    float window[8], tm[4];
    memcpy(tm, texmat.cpu, sizeof tm);
    if (vp_[0] != 0 || vp_[1] != 0 || vp_[2] != target->width || vp_[3] != target->height ||
        !quadWindow(s, corners, m, vp_, window))
        return false;
    for (int k = 0; k < 4; k++) {
        float uv[2];
        const uint64_t at = uint64_t(corners[k]) * s.stride + uint64_t(uvOffset);
        if (at + sizeof uv > s.size)
            return false;
        memcpy(uv, s.bytes + at, sizeof uv);
        const float wx = window[2 * k] / float(target->width), wy = window[2 * k + 1] / float(target->height);
        if ((std::fabs(wx) > 1e-4f && std::fabs(wx - 1) > 1e-4f) ||
            (std::fabs(wy) > 1e-4f && std::fabs(wy - 1) > 1e-4f))
            return false;
        if (std::fabs(uv[0] * tm[0] + tm[2] - wx) > 1e-4f || std::fabs(uv[1] * tm[1] + tm[3] - wy) > 1e-4f)
            return false;
    }
    Texture from = *source;
    const uint32_t words = steps * (SWITCH_GM20B_2D_SCALE_WORDS + 16) + 16;
    if (p_.room() < words)
        reserve();
    kit(p_, switch_gm20b_tiled_cache_flush);
    kit(p_, switch_gm20b_wait_for_idle);
    for (uint32_t step = 1; step <= steps; step++) {
        Texture into = *target;
        if (step < steps) {
            into.width = from.width / 2;
            into.height = from.height / 2;
            into.blockLinear = false;
            into.comptags = false;
            into.pitch = (into.width * into.bpp + 255) & ~255u;
            if (!dev_->alloc(dev_->upload_, into.pitch * into.height, 256, into.va))
                return false;
        }
        const switch_gm20b_2d_surface a = twodSurface(from, fmt2d), b = twodSurface(into, fmt2d);
        const switch_gm20b_2d_scale_op op = {0,          0,          into.width, into.height, 2ull << 32, 2ull << 32,
                                             1ull << 32, 1ull << 32, true,       true,        0};
        kit(p_, switch_gm20b_2d_scale, &a, &b, &op);
        if (step < steps)
            kit(p_, switch_gm20b_wait_for_idle);
        from = into;
    }
    kit(p_, switch_gm20b_wait_for_idle);
    kit(p_, switch_gm20b_invalidate_texture_data);
    cb_->use(*source, false);
    cb_->use(*target, true);
    RAW_DIAGNOSTIC(dev_->stats.blurTwodPasses++);
    return true;
}

}  // namespace raw
