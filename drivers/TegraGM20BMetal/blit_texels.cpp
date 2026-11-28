// SPDX-License-Identifier: MIT
// The blit encoder's remapped texel copies (blit_encoder.h): between textures, a stencil aspect through its temp, and
// between a texture and a buffer's rows
#include "gpu.h"
#include "blit_remap.h"

#include <algorithm>
#include <cstdio>

namespace gm20b {

namespace {
// `side` from element `x` of its line `y` on (`element`: the bytes of an element of the copy on this side)
CopySide shifted(const CopySide &side, uint32_t x, uint32_t y, uint32_t element)
{
    CopySide s = side;
    if (s.blockLinear) {
        s.x += x;
        s.y += y;
    } else {
        s.va += uint64_t(y) * s.pitch + uint64_t(x) * element;
    }
    return s;
}

// The elements of a line a row of `side` holds; every one on a block-linear side
uint32_t rowElements(const CopySide &side, uint32_t element)
{
    return side.blockLinear ? UINT32_MAX : side.pitch / element;
}
}  // namespace

void BlitEncoder::copyElements(const CopySide &s, const CopySide &d, uint32_t w, uint32_t h, uint32_t remap,
                               uint32_t bytes, bool ordered)
{
    constexpr uint32_t kLaunchWords = SWITCH_GM20B_COPY_RECT_WORDS(1);
    const uint32_t unit = (remap >> 16 & 3) + 1, sElement = unit * ((remap >> 20 & 3) + 1),
                   dElement = unit * ((remap >> 24 & 3) + 1);
    const uint32_t fits = std::min(rowElements(s, sElement), rowElements(d, dElement));
    if (h < 2 || fits >= w) {
        room(kLaunchWords);
        copyRemapped(p_, s, d, w, h, remap, bytes, dev_->stats, ordered);
        return;
    }
    const bool overlaid = rowElements(d, dElement) < w;
    const uint32_t columns = fits ? (w + fits - 1) / fits : UINT32_MAX;
    if (h <= columns) {
        for (uint32_t y = 0; y < h; y++) {
            room(kLaunchWords);
            copyRemapped(p_, shifted(s, 0, y, sElement), shifted(d, 0, y, dElement), w, 1, remap, bytes, dev_->stats,
                         y ? overlaid : ordered);
        }
        return;
    }
    // a column's rows overlap in no side; where the destination's do, a later row's bytes lie in an earlier column
    for (uint32_t k = 0; k < columns; k++) {
        const uint32_t x = (overlaid ? columns - 1 - k : k) * fits;
        room(kLaunchWords);
        copyRemapped(p_, shifted(s, x, 0, sElement), shifted(d, x, 0, dElement), std::min(fits, w - x), h, remap, bytes,
                     dev_->stats, k ? overlaid : ordered);
    }
}

bool BlitEncoder::copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                             uint32_t dy, uint32_t dz, uint32_t w, uint32_t h, uint8_t aspect)
{
    if (!w || !h)
        return true;
    const CopySide s = sideOf(src, sx, sy, sz), d = sideOf(dst, dx, dy, dz);
    // an eight-byte texel's first or second word alone: Depth32Float_Stencil8's depth or stencil word, RGB10A8_2P_XR10's
    // colour word or alpha word
    const uint32_t remap = aspect == kAspectStencil ? remapOf(kRemapNoWrite, 1, kRemapNoWrite, kRemapNoWrite, 4, 2, 2)
                           : aspect == kAspectDepth ? remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 2, 2)
                                                    : texelRemap(src.bpp);
    const uint32_t unit = (remap >> 16 & 3) + 1, sElement = unit * ((remap >> 20 & 3) + 1),
                   dElement = unit * ((remap >> 24 & 3) + 1);
    const CopyOverlap ov =
        copySideOverlap(s, d, w, h, sElement, dElement, true, src.storageRange().arena, dst.storageRange().arena);
    if (ov == CopyOverlap::Identity)
        return true;
    cb_->use(src, false);
    cb_->use(dst, true);
    if (ov == CopyOverlap::Overlap) {
        uint32_t stagePitch = 0;
        uint64_t planeBytes = 0, totalBytes = 0;
        if (!overlapStageLayout(w, h, sElement, 1, stagePitch, planeBytes, totalBytes))
            return false;
        Texture *scratch = cb_->copyScratch(totalBytes);
        if (!scratch)
            return false;
        const CopySide stage{scratch->va, stagePitch};
        // Both launches run NON_PIPELINED and the trailing barrier drains stage -> d without resetting access() sets.
        copyElements(s, stage, w, h, texelRemap(sElement), sElement, true);
        copyElements(stage, d, w, h, remap, aspect == kAspectWhole ? src.bpp : 4, true);
        barrier();
    } else {
        room(32);
        copyElements(s, d, w, h, remap, aspect == kAspectWhole ? src.bpp : 4);
    }
    return true;
}

void BlitEncoder::copyStencilTexels(const Texture &zeta, uint32_t x, uint32_t y, uint32_t z, const Texture &bytes,
                                    uint32_t bx, uint32_t by, uint32_t bz, uint32_t w, uint32_t h, bool intoZeta)
{
    if (!zeta.stencilTemp || !w || !h)
        return;
    const Texture &s = *zeta.stencilTemp;
    cb_->use(zeta, intoZeta);
    cb_->use(bytes, !intoZeta);
    cb_->use(s, true);
    // the temp is the texture's level 0 alone: a larger region (a multisampled one's sample grid) goes through it in parts.
    // Every launch waits for the copies before it: the temp is shared by every stencil copy of the texture, which the
    // encoder's access() orders by the textures' names alone
    const CopySide temp{s.va, s.pitch};
    const uint32_t tw = std::max(1u, std::min(w, s.width)), th = std::max(1u, std::min(h, s.height));
    for (uint32_t ty = 0; ty < h; ty += th)
        for (uint32_t tx = 0; tx < w; tx += tw) {
            const uint32_t cw = std::min(tw, w - tx), ch = std::min(th, h - ty);
            const CopySide texels = sideOf(zeta, x + tx, y + ty, z), plain = sideOf(bytes, bx + tx, by + ty, bz);
            room(64);
            if (intoZeta) {
                copyElements(plain, temp, cw, ch, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 1, 2), 1,
                             true);
                copyElements(temp, texels, cw, ch, remapOf(kRemapNoWrite, kRemapNoWrite, 0, kRemapNoWrite, 2, 1, 4), 2,
                             true);
            } else {
                copyElements(texels, temp, cw, ch, remapOf(2, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 2, 4, 1), 2,
                             true);
                copyElements(temp, plain, cw, ch, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 2, 1), 1,
                             true);
            }
        }
}

void BlitEncoder::copyFirstByte(const Texture &from, const Texture &to)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    copyElements(sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), std::min(from.width, to.width),
                 std::min(from.height, to.height),
                 remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, from.bpp, to.bpp), 1);
}

void BlitEncoder::copyFirstTwoBytes(const Texture &from, const Texture &to)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    copyElements(sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), std::min(from.width, to.width),
                 std::min(from.height, to.height), remapOf(0, 1, kRemapNoWrite, kRemapNoWrite, 1, from.bpp, to.bpp), 2);
}

void BlitEncoder::copyArgb8(const Texture &from, const Texture &to, bool toStandIn)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    // ARGB8Unorm's bytes are alpha, red, green, blue; the stand-in's red, green, blue, alpha
    const uint32_t remap = toStandIn ? remapOf(1, 2, 3, 0, 1, 4, 4) : remapOf(3, 0, 1, 2, 1, 4, 4);
    copyElements(sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), std::min(from.width, to.width),
                 std::min(from.height, to.height), remap, 4);
}

void BlitEncoder::copyRedBlueSwapped(const Texture &from, const Texture &to, uint32_t w, uint32_t h)
{
    cb_->use(from, false);
    cb_->use(to, true);
    room(32);
    copyElements(sideOf(from, 0, 0, 0), sideOf(to, 0, 0, 0), w, h, remapOf(2, 1, 0, 3, 1, 4, 4), 4);
}

void BlitEncoder::copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, uint32_t h, bool toTexture, uint8_t aspect)
{
    if (t.xrPlanes && t.luma)  // plane 0's words alone (xr.h bufferTexelBytes): the alpha plane stays as it is
        return copyBufferTexels(va, pitch, *t.luma, x, y, z, w, h, toTexture, aspect);
    if (t.samples > 1 && !t.zeta)
        return copyBufferSamples(va, pitch, t, x, y, w, h, toTexture);
    if (!toTexture)
        cb_->bufferWrites_.note(va, uint64_t(pitch) * h + uint64_t(w) * 16);
    cb_->use(t, toTexture);
    room(48);
    const CopySide rows{va, pitch}, texels = sideOf(t, x, y, z);
    const CopySide &from = toTexture ? rows : texels, &to = toTexture ? texels : rows;
    if (t.zeta && t.format == kZetaFormat && aspect == kAspectDepth) {
        // the texel's first word: depth
        copyElements(from, to, w, h,
                     toTexture ? remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 1, 2)
                               : remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 2, 1),
                     4);
        return;
    }
    if (aspect == kAspectStencil && t.stencilTemp && ((t.zeta && t.format == kZetaFormat) || twoPlaneXr(t.format))) {
        // its fifth byte, stencil (an RGB10A8_2P_XR10 texel's alpha, whose sixth byte is 0): as the third of its two-byte
        // components to or from the temp's two bytes a texel, the buffer's byte and a 0, and those two bytes' first to or
        // from the buffer's byte; each launch after the copies before it, which may share the temp
        const Texture &s = *t.stencilTemp;
        cb_->use(s, true);
        const CopySide temp{s.va + uint64_t(y) * s.pitch + 2ull * x, s.pitch};
        if (toTexture) {
            const bool xr = twoPlaneXr(t.format);
            if (xr) {
                room(64);
                p_.m(CE_SET_REMAP_CONST_A, 0, SUBC_COPY);
            }
            copyElements(rows, temp, w, h,
                         remapOf(0, xr ? kRemapConstA : kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 1, 2),
                         xr ? 2 : 1, true);
            copyElements(temp, texels, w, h, remapOf(kRemapNoWrite, kRemapNoWrite, 0, kRemapNoWrite, 2, 1, 4), 2, true);
        } else {
            copyElements(texels, temp, w, h, remapOf(2, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 2, 4, 1), 2, true);
            copyElements(temp, rows, w, h, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 2, 1), 1, true);
        }
        return;
    }
    if (twoPlaneXr(t.format)) {
        // four-byte components: the texel's color word to the buffer's word, or into the texel with its alpha word NO_WRITE (6)
        constexpr uint32_t kOut = 3u << 16 | 1u << 20, kIn = 6u << 4 | 3u << 16 | 1u << 24;
        copyElements(from, to, w, h, toTexture ? kIn : kOut, bufferTexelBytes(t.format, t.bpp));
        return;
    }
    copyElements(from, to, w, h, texelRemap(t.bpp), t.bpp);
}

void BlitEncoder::copyBufferSamples(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t w,
                                    uint32_t h, bool toTexture)
{
    uint32_t gx = 0, gy = 0;
    sampleGrid(t.samples, gx, gy);
    Texture grid = t;
    grid.width <<= gx;
    grid.height <<= gy;
    grid.samples = 1;
    const uint32_t bpp = t.bpp, pair = 2 * bpp;
    if (!toTexture)
        cb_->bufferWrites_.note(va, uint64_t(pitch) * (h - 1) + uint64_t(w) * (bpp << (gx + gy)));
    cb_->use(t, toTexture);
    room(48);
    const CopySide rows{va, pitch}, texels = sideOf(grid, x << gx, y << gy, 0);
    if (!gy) {
        copyElements(toTexture ? rows : texels, toTexture ? texels : rows, w << gx, h, texelRemap(bpp), bpp);
        return;
    }
    const Texture &s = *t.sampleRows;
    cb_->use(s, true);
    // every launch after the copies before it: the temp is every buffer copy's of the texture, which access() orders by
    // the textures' names alone
    const CopySide temp{s.va, s.pitch};
    if (!toTexture)
        copyElements(texels, temp, w << gx, h << gy, texelRemap(bpp), bpp, true);
    for (uint32_t k = 0; k < 2; k++) {
        const CopySide half{s.va + uint64_t(k) * s.pitch, 2 * s.pitch};
        if (bpp <= 4) {
            const uint32_t into = k ? remapOf(kRemapNoWrite, kRemapNoWrite, 0, 1, bpp, 2, 4)
                                    : remapOf(0, 1, kRemapNoWrite, kRemapNoWrite, bpp, 2, 4);
            const uint32_t out = k ? remapOf(2, 3, kRemapNoWrite, kRemapNoWrite, bpp, 4, 2)
                                   : remapOf(0, 1, kRemapNoWrite, kRemapNoWrite, bpp, 4, 2);
            room(32);
            if (toTexture)
                copyElements(rows, half, w, h, out, pair, true);
            else
                copyElements(half, rows, w, h, into, pair, true);
            continue;
        }
        // a pair of eight- or sixteen-byte samples: no remap holds two, so each launch moves one pair a line, a column of
        // them down the rows or a row of them across, whichever takes fewer launches
        const bool columns = w <= h;
        for (uint32_t c = 0; c < (columns ? w : h); c++) {
            const CopySide line = columns ? CopySide{half.va + uint64_t(c) * pair, half.pitch}
                                          : CopySide{half.va + uint64_t(c) * half.pitch, pair},
                           slots = columns ? CopySide{va + uint64_t(c) * 2 * pair + uint64_t(k) * pair, pitch}
                                           : CopySide{va + uint64_t(c) * pitch + uint64_t(k) * pair, 2 * pair};
            room(32);
            copyElements(toTexture ? slots : line, toTexture ? line : slots, pair / 16, columns ? h : w, texelRemap(16),
                         pair, true);
        }
    }
    if (toTexture) {
        room(32);
        copyElements(temp, texels, w << gx, h << gy, texelRemap(bpp), bpp, true);
    }
}

}  // namespace gm20b
