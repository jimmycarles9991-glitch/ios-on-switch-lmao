// SPDX-License-Identifier: MIT
// The blit encoder's copies that make an image a texture is sampled through from its own bytes (blit_encoder.h): an
// RG8Unorm_sRGB stand-in, a planar texture's codes image, a one-plane 4:2:2 texture's luma and chroma
#include "gpu.h"
#include "srgb_pair.h"
#include "xr_parts.h"
#include "blit_remap.h"

#include <vector>

namespace gm20b {

bool BlitEncoder::widenSrgbPair(const Texture &t)
{
    if (!srgbPair(t.format) || !t.standIn)
        return false;
    std::vector<XrPart> parts;
    if (!t.blockLinear) {
        XrPart whole;
        whole.codes = t;
        whole.standIn = *t.standIn;
        parts.push_back(whole);
    } else {
        Texture codes = t;
        codes.standIn = nullptr;
        if (!xrParts(codes, *t.standIn, parts))
            return false;
    }
    cb_->use(t, false);
    cb_->use(*t.standIn, true);
    // each texel's red and green bytes into the stand-in's first two, its blue and alpha NO_WRITE (the swizzle reads
    // zero and one there)
    const uint32_t widen = remapOf(0, 1, kRemapNoWrite, kRemapNoWrite, 1, 2, 4);
    for (const XrPart &part : parts) {
        room();
        copyElements(sideOf(part.codes, 0, 0, 0), sideOf(part.standIn, 0, 0, 0), part.codes.width, part.codes.height,
                     widen, 2);
    }
    return true;
}

// The codes Metal reads of RGB8_444_2P (pack/../m2v ycbcr_convert.rs: "Codes"): red Cr, green Y, blue Cb, each code / 255, alpha
// 1, an RGBA8Unorm texel of the codes, as m2v's image holds them. Remap per destination component: SRC_X..W 0..3, CONST_A 4,
// NO_WRITE 6 (clb0b5 SET_REMAP_COMPONENTS); one byte a component, four destination components.
void BlitEncoder::convertPlanar(const Texture &t)
{
    if (!t.planar || !t.width || !t.height)
        return;
    cb_->useAddress(t.planes.luma, rowStorageBytes(t.planes.lumaPitch, t.width, t.height, 1), false);
    cb_->useAddress(t.planes.chroma, rowStorageBytes(t.planes.chromaPitch, t.width, t.height, 2), false);
    cb_->use(t, true);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.ceOps += 2);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.ceBytes += uint64_t(t.width) * t.height * 4 *
                                                   2);  // (each launch writes the image's rows, through NO_WRITE)
    room();
    constexpr uint32_t A = 4, W = 6, one = 0 << 16, dst4 = 3u << 24;
    p_.m(CE_SET_REMAP_CONST_A, 0xff, SUBC_COPY);
    // the luma plane, one component a texel: green Y, alpha 255
    p_.m(CE_SET_REMAP_COMPONENTS, W | 0 << 4 | W << 8 | A << 12 | one | 0u << 20 | dst4, SUBC_COPY);
    p_.inc(CE_OFFSET_IN_UPPER,
           {uint32_t(t.planes.luma >> 32), uint32_t(t.planes.luma), uint32_t(t.va >> 32), uint32_t(t.va),
            t.planes.lumaPitch, t.pitch, t.width, t.height},
           SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kLaunchPitch | 1u << 10, SUBC_COPY);
    room();
    // the chroma plane, two components a texel (Cb, Cr): red Cr, blue Cb
    p_.m(CE_SET_REMAP_COMPONENTS, 1 | W << 4 | 0 << 8 | W << 12 | one | 1u << 20 | dst4, SUBC_COPY);
    p_.inc(CE_OFFSET_IN_UPPER,
           {uint32_t(t.planes.chroma >> 32), uint32_t(t.planes.chroma), uint32_t(t.va >> 32), uint32_t(t.va),
            t.planes.chromaPitch, t.pitch, t.width, t.height},
           SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kLaunchPitch | 1u << 10, SUBC_COPY);
}

void BlitEncoder::splitPlane(const Texture &t)
{
    if (!t.onePlane || !t.luma || !t.chroma || !t.chroma->width || !t.height)
        return;
    // the luma image's rows: a texture buffer's are its rows of 16384 texels (ycbcr.h kYcbcrBufferRow)
    const uint32_t pairs = t.chroma->width, height = t.luma->height;
    const uint64_t rows = rowStorageBytes(t.planes.lumaPitch, pairs, height, 4);
    cb_->useAddress(t.planes.luma, t.window.bytes ? std::min(rows, t.window.va + t.window.pages - t.planes.luma) : rows,
                    false);
    cb_->use(*t.luma, true);
    cb_->use(*t.chroma, true);
    constexpr uint32_t one = 0 << 16, src4 = 3u << 20, dst2 = 1u << 24;
    for (const Texture *to : {t.luma, t.chroma}) {
        // Y0, Y1 or Cb, Cr of Y0 Cb Y1 Cr (of Cb Y0 Cr Y1: Texture::pairsReversed)
        const bool luma = (to == t.luma) != t.pairsReversed;
        const uint32_t remap = luma ? 0 | 2 << 4 : 1 | 3 << 4;
        copyElements({t.planes.luma, t.planes.lumaPitch}, {to->va, to->pitch}, pairs, height, remap | one | src4 | dst2,
                     2);
    }
}

void BlitEncoder::mergePlane(const Texture &t)
{
    if (!t.onePlane || !t.luma || !t.chroma || !t.chroma->width || !t.height)
        return;
    const uint32_t pairs = t.chroma->width, whole = t.width / 2;
    const uint64_t rows = rowStorageBytes(t.planes.lumaPitch, pairs, t.height, 4);
    cb_->useAddress(t.planes.luma, t.window.bytes ? std::min(rows, t.window.va + t.window.pages - t.planes.luma) : rows,
                    true);
    cb_->use(*t.luma, false);
    cb_->use(*t.chroma, false);
    const auto launch = [&](uint64_t from, uint32_t fromPitch, uint64_t to, uint32_t remap, uint32_t width) {
        copyElements({from, fromPitch}, {to, t.planes.lumaPitch}, width, t.height, remap, 4);
    };
    // Y0 Cb Y1 Cr, or Cb Y0 Cr Y1 (Texture::pairsReversed): luma's bytes 0 and 2 of a pair, chroma's 1 and 3, or the reverse
    constexpr uint32_t W = kRemapNoWrite;
    const uint32_t lumaPair = t.pairsReversed ? remapOf(W, 0, W, 1, 1, 2, 4) : remapOf(0, W, 1, W, 1, 2, 4);
    const uint32_t chromaPair = t.pairsReversed ? remapOf(0, W, 1, W, 1, 2, 4) : remapOf(W, 0, W, 1, 1, 2, 4);
    if (whole)
        launch(t.luma->va, t.luma->pitch, t.planes.luma, lumaPair, whole);
    if (t.width & 1)
        launch(t.luma->va + t.width - 1, t.luma->pitch, t.planes.luma + 4ull * whole,
               t.pairsReversed ? remapOf(W, 0, W, W, 1, 1, 4) : remapOf(0, W, W, W, 1, 1, 4), 1);
    launch(t.chroma->va, t.chroma->pitch, t.planes.luma, chromaPair, pairs);
}

}  // namespace gm20b
