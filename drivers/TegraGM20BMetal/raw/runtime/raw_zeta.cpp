// SPDX-License-Identifier: MIT
// See raw_zeta.h. Method numbers: NVIDIA's class headers as Mesa ships them (cl9097.h, clb197.h, clc597.h); the target words follow
// Mesa NVK nvk_CmdBeginRendering, the state nvk_flush_ds_state and its vk_to_nv9097_* encodings, the clears nvk_CmdClearAttachments
// (all MIT).
#include "raw_zeta.h"

#include <algorithm>
#include <cstring>
#include "../fx/fx_image.h"
#include "raw_gpu.h"
#include "raw_samples.h"

namespace raw {
namespace {
enum : uint32_t {
    Z_CLEAR_VALUE = 0x0d90,
    STENCIL_CLEAR_VALUE = 0x0da0,
    ZT_A = 0x0fe0,
    STENCIL_COMPRESSION = 0x111c,
    ZT_SPARSE = 0x1208,
    ZT_SIZE_A = 0x1228,
    ZT_SELECT = 0x1538,
    ZT_LAYER = 0x179c,
    Z_COMPRESSION = 0x19cc,
};

// kDepthStencilRegs methods: SET_DEPTH_TEST, _WRITE, _FUNC, SET_STENCIL_TEST, SET_STENCIL_OP_FAIL/ZFAIL/ZPASS, SET_STENCIL_FUNC,
// _REF, _FUNC_MASK, SET_STENCIL_MASK, then the back face's ops, func, ref, mask, func mask
constexpr uint32_t kDepthStencilMethods[kDepthStencilRegs] = {0x12cc, 0x12e8, 0x130c, 0x1380, 0x1384, 0x1388,
                                                              0x138c, 0x1390, 0x1394, 0x1398, 0x139c, 0x1598,
                                                              0x159c, 0x15a0, 0x15a4, 0x0f54, 0x0f58, 0x0f5c};

// MTLCompareFunction and VkCompareOp share their order; NV's OGL value is 0x200 | it
uint32_t compareOp(uint8_t f)
{
    return 0x200u | (f & 7u);
}

// MTLStencilOperation and VkStencilOp share their order; NV's D3D value is it + 1
uint32_t stencilOp(uint8_t op)
{
    return (op & 7u) + 1;
}

bool noop(const StencilFace &s)
{
    return s.compare == 7 && s.depthFail == 0 && s.pass == 0;
}

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

void zetaTargetWords(Push &p, const Texture *z)
{
    if (!z) {
        p.m(ZT_SELECT, 0);
        return;
    }
    p.inc(ZT_A, {uint32_t(z->va >> 32), uint32_t(z->va), kZetaFormatInfo[zetaFormatIndex(z->format)].zt,
                 uint32_t(z->blockHeight & 0xf) << 4,  // WIDTH ONE_GOB, HEIGHT, DEPTH ONE_GOB
                 uint32_t(z->layerBytes >> 2)});
    p.m(ZT_SELECT, 1);
    // SIZE_A the row stride in texels (NVK: the Z/S hardware has no tile width), C one layer, THIRD_DIMENSION_DEFINES_ARRAY_SIZE
    p.inc(ZT_SIZE_A, {z->pitch / z->bpp, sampleRows(*z), 1});
    p.m(ZT_LAYER, 0);
    p.m(Z_COMPRESSION, z->comptags);
    p.m(STENCIL_COMPRESSION, z->comptags);
    p.m(ZT_SPARSE, 0);
}

bool DepthStencilDesc::active() const
{
    return depthCompare != 7 || depthWrite || !noop(front) || !noop(back);
}

bool DepthStencilRegs::operator==(const DepthStencilRegs &o) const
{
    return !memcmp(v, o.v, sizeof v);
}

DepthStencilRegs depthStencilRegs(const DepthStencilDesc &d, uint32_t refFront, uint32_t refBack, bool depth,
                                  bool stencil)
{
    DepthStencilRegs r;
    const bool depthTest = d.depthCompare != 7 || d.depthWrite;
    const bool stencilTest = !(noop(d.front) && noop(d.back));
    r.v[0] = depthTest && depth;
    r.v[1] = d.depthWrite && depth;
    r.v[2] = compareOp(d.depthCompare);
    r.v[3] = stencilTest && stencil;
    const StencilFace *faces[2] = {&d.front, &d.back};
    const uint32_t refs[2] = {refFront, refBack};
    for (uint32_t i = 0; i < 2; i++) {
        const StencilFace &s = *faces[i];
        uint32_t *ops = i ? &r.v[11] : &r.v[4];
        ops[0] = stencilOp(s.fail);
        ops[1] = stencilOp(s.depthFail);
        ops[2] = stencilOp(s.pass);
        ops[3] = compareOp(s.compare);
        uint32_t *rest = i ? &r.v[15] : &r.v[8];  // front: ref, func mask, mask; back: ref, mask, func mask
        rest[0] = std::min(refs[i], 0xffu);
        rest[1] = (i ? s.writeMask : s.readMask) & 0xff;
        rest[2] = (i ? s.readMask : s.writeMask) & 0xff;
    }
    return r;
}

void emitDepthStencil(Push &p, const DepthStencilRegs &want, DepthStencilRegs &held)
{
    for (uint32_t i = 0; i < kDepthStencilRegs; i++)
        if (want.v[i] != held.v[i])
            p.m(kDepthStencilMethods[i], want.v[i]);
    held = want;
}

void zetaClearValues(Push &p, bool depth, float clearDepth, bool stencil, uint32_t clearStencil)
{
    if (depth)
        p.m(Z_CLEAR_VALUE, floatBits(clearDepth));
    if (stencil)
        p.m(STENCIL_CLEAR_VALUE, clearStencil & 0xff);
}

}  // namespace raw
