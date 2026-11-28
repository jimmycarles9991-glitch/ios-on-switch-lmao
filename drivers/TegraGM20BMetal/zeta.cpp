// SPDX-License-Identifier: MIT
// See zeta.h. Method numbers: NVIDIA's class headers (kits/SwitchGM20B: Mesa's clb197.h); the target words follow
// Mesa NVK nvk_CmdBeginRendering, the state's emission nvk_flush_ds_state, the clears nvk_CmdClearAttachments (all MIT).
#include "zeta.h"

#include <algorithm>
#include <cstring>
#include "../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../kits/SwitchGM20BEmit/SwitchGM20BEmitDecompress.h"
#include "fx/fx_image.h"
#include "gpu.h"
#include "kit.h"
#include "rotated_pass.h"
#include "samples.h"
#include "shader_test_push.h"
#include "shader_tests.h"
#include "tiled_cache.h"

#include "gm20b_gpu_internal.h"

namespace gm20b {
namespace {
enum : uint32_t {
    Z_CLEAR_VALUE = NVB197_SET_Z_CLEAR_VALUE,
    STENCIL_CLEAR_VALUE = NVB197_SET_STENCIL_CLEAR_VALUE,
    ZT_A = NVB197_SET_ZT_A,
    STENCIL_COMPRESSION = NVB197_SET_STENCIL_COMPRESSION,
    ZT_SPARSE = NVB197_SET_ZT_SPARSE,
    ZT_SIZE_A = NVB197_SET_ZT_SIZE_A,
    ZT_LAYER = NVB197_SET_ZT_LAYER,
    Z_COMPRESSION = NVB197_SET_Z_COMPRESSION,
};

uint32_t floatBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
}  // namespace

bool zetaShape(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t, uint64_t &bytes,
               uint64_t &align)
{
    const uint32_t i = zetaFormatIndex(format);
    if (i == kZetaFormats)
        return false;
    const uint32_t bpp = kZetaFormatInfo[i].bpp;
    fx::MipChain c;
    if (samples > 1 ? !fx::msLayout(width, height, samples, bpp, c) : !fx::mipChainLayout(width, height, 1, bpp, c))
        return false;
    const fx::MipLevel &l = c.level[0];
    t = Texture();
    t.width = width;
    t.height = height;
    t.format = format;
    t.samples = uint8_t(samples);
    t.bpp = bpp;
    t.pitch = l.rowBytes;
    t.blockLinear = true;
    t.blockHeight = l.yLog2;
    t.layerBytes = uint64_t(l.rowBytes) * l.rows;  // NIL's array stride: one level, whole tiles already
    t.zeta = true;
    bytes = c.bytes;
    align = c.align;
    return true;
}

void zetaTargetWords(Push &p, const Texture *z, uint32_t layers)
{
    if (!z) {
        p.m(ZT_SELECT, 0);
        return;
    }
    const ZetaTargetValues v = zetaTargetValues(*z, sampleRows(*z));
    p.incArray(ZT_A, v.a, 5);
    p.m(ZT_SELECT, 1);
    // SIZE_C the pass's layers from ZT_A's (THIRD_DIMENSION_DEFINES_ARRAY_SIZE)
    const uint32_t size[3] = {v.size[0], v.size[1], std::max(layers, 1u)};
    p.incArray(ZT_SIZE_A, size, 3);
    p.m(ZT_LAYER, 0);
    p.m(Z_COMPRESSION, z->comptags);
    p.m(STENCIL_COMPRESSION, z->comptags);
    p.m(ZT_SPARSE, 0);
}

bool DepthStencilDesc::active() const
{
    return depthCompare != 7 || depthWrite || !stencilNoop(front) || !stencilNoop(back);
}

bool DepthStencilDesc::writes(bool depth, bool stencil) const
{
    const auto faceWrites = [](const StencilFace &s) {
        return (s.writeMask & 0xff) && (s.fail || s.depthFail || s.pass);
    };
    return (depth && depthWrite) || (stencil && (faceWrites(front) || faceWrites(back)));
}

bool DepthStencilRegs::operator==(const DepthStencilRegs &o) const
{
    return !memcmp(v, o.v, sizeof v);
}

Texture RenderEncoder::hiddenZetaShape(const PassDesc &pass) const
{
    Texture shape;
    shape.width = pass.width;
    shape.height = pass.height;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (const Texture *t = pass_.color[j].texture) {
            shape.width = shape.width ? std::min(shape.width, t->width) : t->width;
            shape.height = shape.height ? std::min(shape.height, t->height) : t->height;
        }
    shape = planeShaped(shape, rot_);
    shape.format = kZetaFormat;
    shape.samples = samples_;
    shape.layers = std::max(pass.layers, 1u);
    shape.zeta = shape.memoryless = true;
    return shape;
}

namespace {
// shader_tests.h's copies of a separate pair's aspects, with an aspect of a Depth32Float_Stencil8 texture moved straight
// between its 8-byte texels and the backing's: its depth word (two 4-byte components a side), or its stencil byte with
// the padding byte beside it (four 2-byte components a side, the third written), the rest of each texel kept
ShaderTestTransfers separateTransfers(uint8_t aspects, bool packed, bool toAttachment, bool depthPacked,
                                      bool stencilPacked)
{
    const auto remap = [](uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t source,
                          uint32_t destination) {
        return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (source - 1) << 20 | (destination - 1) << 24;
    };
    constexpr uint32_t keep = 6;
    ShaderTestTransfers plan =
        shaderTestTransfers(uint8_t(aspects & ~(stencilPacked ? kShaderTestStencil : 0)), packed, toAttachment);
    if (depthPacked && (aspects & kShaderTestDepth))
        plan.transfers[0].remap =
            remap(0, keep, keep, keep, 4, toAttachment || packed ? 2 : 1, !toAttachment || packed ? 2 : 1);
    if (stencilPacked && (aspects & kShaderTestStencil))
        plan.transfers[plan.count++] = {toAttachment ? ShaderTestImage::stencil : ShaderTestImage::attachment,
                                        toAttachment ? ShaderTestImage::attachment : ShaderTestImage::stencil,
                                        remap(keep, keep, 2, keep, 2, 4, 4), 2, false};
    return plan;
}
}  // namespace

// The separate depth and stencil textures' aspects into the pass's Depth32Float_Stencil8 backing, or out of it, by
// shader_tests.h's copies (as a shader test's stand-ins are filled and drained), over the render area: after the work
// before is done with them, and done before the work after reads them. A layered pass's every layer each texture has, each
// through the temporary's one
void RenderEncoder::copySeparateZeta(Push &p, uint8_t aspects, bool toAttachment)
{
    const Texture &zeta = *pass_.depthStencil.texture;
    ShaderTestShape shape;
    if (!aspects || !shaderTestShape(zeta.format, samples_, planeArea().x1, planeArea().y1, aspects, shape))
        return;
    uint32_t layer = 0;
    auto side = [&](const Texture &texture) {
        auto result = sideOf(texture, 0, 0, &texture == separateTemp_ ? 0 : layer);
        result.height = sampleRows(texture);
        return result;
    };
    const auto image = [&](ShaderTestImage which) {
        switch (which) {
        case ShaderTestImage::depth:
            return side(*separate_.depth);
        case ShaderTestImage::stencil:
            return side(*separate_.stencil);
        case ShaderTestImage::temporary:
            return side(*separateTemp_);
        case ShaderTestImage::attachment:
            break;
        }
        return side(zeta);
    };
    const auto has = [&](ShaderTestImage which) {
        const Texture *t = which == ShaderTestImage::depth     ? separate_.depth
                           : which == ShaderTestImage::stencil ? separate_.stencil
                                                               : nullptr;
        return !t || layer < std::max(t->layers, 1u);
    };
    if (aspects & kShaderTestDepth)
        cb_->use(*separate_.depth, !toAttachment);
    if (aspects & kShaderTestStencil)
        cb_->use(*separate_.stencil, !toAttachment);
    cb_->use(zeta, toAttachment);
    if (cb_->tiledCacheDirty_) {
        tiledCacheDrain(p, dev_->k_.info());
        cb_->tiledCacheDirty_ = false;
    }
    p.m(WAIT_FOR_IDLE, 0);
    if (toAttachment)
        p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
    const auto copies = separateTransfers(aspects, shape.packedStencil, toAttachment,
                                          separate_.depth && separate_.depth->format == kZetaFormat,
                                          separate_.stencil->format == kZetaFormat);
    for (layer = 0; layer < std::max(pass_.layers, 1u); layer++)
        for (uint8_t at = 0; at < copies.count; ++at) {
            const auto &copy = copies.transfers[at];
            if (has(copy.source) && has(copy.destination))
                copyRemapped(p, image(copy.source), image(copy.destination), shape.width, shape.height, copy.remap,
                             copy.bytes, dev_->stats, copy.ordered || (layer && !at));
        }
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.compZetaCopies += zeta.comptags);
    p.inc(CE_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    p.m(CE_LAUNCH_DMA, (kLaunchPitch & ~3u) | 2u | (1u << 10), SUBC_COPY);
    p.m(INVALIDATE_SHADER_CACHES, kInvalidateData);
    p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
}

// The layers zetaShadowOf_ has between it and the pass's backing (CommandBuffer::render), over the render area: in ahead of the
// pass's clears unless they clear every aspect, out after its draws. A compressed zeta is decompressed in place by the zeta
// ROP ahead of both, as the copy engine is proven over no compressed zeta's texels
void RenderEncoder::copyZetaShadow(Push &p, bool toAttachment)
{
    const Texture &own = *zetaShadowOf_, &backing = *pass_.depthStencil.texture;
    const DepthStencilAttachment &zs = pass_.depthStencil;
    cb_->use(own, !toAttachment);
    cb_->use(backing, toAttachment);
    bool decompress = toAttachment && own.zeta && own.comptags;
#if SWITCH_METAL_DIAGNOSTICS
    decompress &= !zetaShadowCompressed_;
#endif
    if (decompress) {
        zetaTargetWords(p, &own);
        kit(p, switch_gm20b_decompress_zeta, zetaHasDepth(own.format), zetaHasStencil(own.format));
        tiledCacheDrain(p, dev_->k_.info());
        p.m(WAIT_FOR_IDLE, 0);
    }
    if (toAttachment && (zs.clearDepth || !zetaHasDepth(backing.format)) &&
        (zs.clearStencil || !zetaHasStencil(backing.format)))
        return;
    const Rect a = planeArea();
    for (uint32_t l = 0; l < std::min(std::max(own.layers, 1u), std::max(pass_.layers, 1u)); l++) {
        const CopySide from = sideOf(own, a.x0, a.y0, l), into = sideOf(backing, a.x0, a.y0, l);
        copySides(p, toAttachment ? from : into, toAttachment ? into : from, a.x1 - a.x0, a.y1 - a.y0, own.bpp,
                  dev_->stats);
    }
}

void zetaClearValues(Push &p, bool depth, float clearDepth, bool stencil, uint32_t clearStencil)
{
    if (depth)
        p.m(Z_CLEAR_VALUE, floatBits(clearDepth));
    if (stencil)
        p.m(STENCIL_CLEAR_VALUE, clearStencil & 0xff);
}

}  // namespace gm20b
