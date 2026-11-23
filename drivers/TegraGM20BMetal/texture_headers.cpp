// SPDX-License-Identifier: MIT
// See gpu.h (the texture header pool and its words). The texture header fill follows Mesa NIL's
// nvb097_fill_image_view_desc (pitch, MIT).
#include "lock.h"
#include "fx/fx_image.h"
#include "gpu.h"
#include "multisample.h"

#include <algorithm>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

uint32_t Device::allocTic()
{
    if (!freeTic_.empty()) {
        const uint32_t t = freeTic_.back();
        freeTic_.pop_back();
        return t;
    }
    return nextTic_ < kTicCount ? nextTic_++ : 0;
}

uint32_t Device::headersLeft()
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    return uint32_t(freeTic_.size()) + (kTicCount - nextTic_);
}

// A freed header holds header 0's null descriptor until it is reused: zeroes would read as a buffer texture at VA 0, which a
// draw still in flight, or one binding a stale index, would sample (a fault).
void Device::freeTic(uint32_t tic)
{
    memcpy(tic_.cpu + 32 * tic, tic_.cpu, 32);
    freeTic_.push_back(tic);
    views_.freed(tic);
    const auto freeViews = [&](auto &byHeader) {
        const auto views = byHeader.equal_range(tic);
        for (auto v = views.first; v != views.second; ++v) {
            memcpy(tic_.cpu + 32 * v->second.second, tic_.cpu, 32);
            freeTic_.push_back(v->second.second);
        }
        byHeader.erase(views.first, views.second);
    };
    freeViews(viewTics_);
    freeViews(attachmentViewTics_);
    // a part's header has view headers of its own (passViewTic): freed with it
    std::vector<uint32_t> parts;
    const auto made = partTics_.equal_range(tic);
    for (auto v = made.first; v != made.second; ++v)
        parts.push_back(v->second.second);
    partTics_.erase(made.first, made.second);
    for (const uint32_t part : parts)
        freeTic(part);
}

uint32_t Device::allocTicPair()
{
    if (!freeTicPairs_.empty()) {
        const uint32_t first = freeTicPairs_.back();
        freeTicPairs_.pop_back();
        return first;
    }
    if (nextTic_ + 1 >= kTicCount)
        return 0;
    const uint32_t first = nextTic_;
    nextTic_ += 2;
    return first;
}

// As freeTic leaves a freed header: header 0's null descriptor in both (an over-planes RGB10A8_2P_XR10 texture's pair,
// xr_texture.cpp)
void Device::freeTicPair(uint32_t first)
{
    memcpy(tic_.cpu + 32 * first, tic_.cpu, 32);
    memcpy(tic_.cpu + 32 * (first + 1), tic_.cpu, 32);
    freeTicPairs_.push_back(first);
}

// Texture header of a pitch-linear 2D texture (Mesa NIL nvb097_fill_image_view_desc for a linear image: HEADER_VERSION PITCH,
// TEXTURE_TYPE TWO_D_NO_MIPMAP), word 0 NVK's for the format
void Device::writeTic(const Texture &t)
{
    fx::MipChain one;
    uint32_t w[8] = {};
    const bool linearArray = !t.blockLinear && !t.memoryless && t.layers > 1;
    if (t.samples > 1) {  // NIL's image of the sample grid (createStoredMultisample, createOwnedZeta, their views)
        const uint32_t z = zetaFormatIndex(t.format);
        const FormatInfo *f = z < kZetaFormats ? nullptr : formatInfo(t.format);
        multisampleHeader(t.width, t.height, t.samples, t.bpp, t.va, f ? f->tic0 : kZetaFormatInfo[z].tic0,
                          f && f->srgb, w);
    } else if (t.zeta && t.layers > 1 && t.type != fx::kTexTwoD && zetaArrayHeader(t, t.type, w)) {
        // an owned depth array, cube or cube array (createOwnedZeta) as the type it was made
    } else if (t.zeta) {  // NVK's image as laid out (createZetaOver): one level, its block height
        one.width = t.width;
        one.height = t.height;
        one.levels = 1;
        one.bpp = t.bpp;
        one.yLog2 = t.blockHeight;
        fx::mipChainTic(one, t.va, kZetaFormatInfo[zetaFormatIndex(t.format)].tic0, false, 0, 1, w, fx::kTexTwoD);
    } else if (t.type == fx::kTexOneDBuffer) {
        const FormatInfo *f = formatInfo(twoPlaneXr(t.format) ? kXrTexelBufferWordFormat : t.format);
        fx::bufferTic(t.va, f->tic0, f->srgb, t.width, w);
    } else if (const FormatInfo *f = formatInfo(t.format);
               (t.layers > 1 && !linearArray && fx::arrayLayout(t.width, t.height, t.layers, 1, t.bpp, one)) ||
               (t.type == fx::kTexThreeD &&
                fx::volumeLayout(t.width, t.height, t.depth, 1, t.bpp, !t.blockDepth, one)) ||
               (t.blockLinear && fx::mipChainLayout(t.width, t.height, 1, t.bpp, one))) {  // NIL's header of the image
        if (t.blockLinear || t.blockW > 1 || t.blockH > 1)
            one.yLog2 = t.blockHeight;
        fx::mipChainTic(one, t.va, f->tic0, f->srgb, 0, 1, w, t.type);
    } else {
        w[0] = f->tic0;
        setField(w, 87, 85, 2);                        // HEADER_VERSION PITCH
        setField(w, 63, 37, (t.va >> 5) & 0x7ffffff);  // ADDRESS_BITS31TO5
        setField(w, 79, 64, (t.va >> 32) & 0xffff);
        setField(w, 111, 96, (t.pitch >> 5) & 0xffff);
        // a linear array (createBufferTexture): TWO_D_ARRAY, its layers pitch x height apart, as the pitch header has no
        // array stride of its own (clb097tex.h TEXHEAD_PITCH)
        setField(w, 154, 151, linearArray ? 5 : 7);  // TWO_D_ARRAY, TWO_D_NO_MIPMAP
        if (linearArray)
            setField(w, 189, 176, t.layers - 1);
        setField(w, 112, 112, 1);  // LOD_ANISO_QUALITY2
        setField(w, 113, 113, 1);  // LOD_ANISO_QUALITY HIGH
        setField(w, 114, 114, 1);  // LOD_ISO_QUALITY HIGH
        setField(w, 143, 128, t.width - 1);
        setField(w, 175, 160, t.height - 1);
        setField(w, 150, 150, f->srgb);
        setField(w, 159, 157, 7);  // BORDER_SIZE BORDER_SAMPLER_COLOR
        setField(w, 191, 191, 1);  // NORMALIZED_COORDS
        setField(w, 216, 215, 2);  // ANISO_FINE_SPREAD_FUNC TWO
        setField(w, 218, 217, 1);  // ANISO_COARSE_SPREAD_FUNC ONE
    }
    if (t.blockW > 1 || t.blockH > 1) {
        setField(w, 143, 128, t.texelWidth - 1);  // WIDTH_MINUS_ONE in texels (NIL's extent_px)
        setField(w, 175, 160, t.texelHeight - 1);
    }
    w[0] = swizzledWord0(w[0], t.swizzle);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // write-combined: visible at the next submission's header invalidate
}

void Device::swizzle(Texture &t, const uint8_t channels[4])
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    memcpy(t.swizzle, channels, 4);
    if (!t.tic)
        return;
    uint32_t *w = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic);
    w[0] = swizzledWord0(w[0], t.swizzle);
    // a draw reads its stand-in (bound_texture.h): the swizzle over the stand-in's own (an RG8Unorm_sRGB one's red,
    // green, zero, one)
    if (t.standIn && t.standIn->tic) {
        uint32_t *s = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.standIn->tic);
        s[0] = swizzledWord0(s[0], channels);
    }
}

uint32_t swizzledWord0(uint32_t word0, const uint8_t channels[4])
{
    constexpr uint32_t kZero = 0, kOneInt = 6, kOneFloat = 7, kSint = 3, kUint = 4;
    auto source = [&](uint32_t c) {
        return word0 >> (19 + 3 * c) & 7;
    };
    const uint32_t redType = word0 >> 7 & 7;
    uint32_t one = redType == kSint || redType == kUint ? kOneInt : kOneFloat;
    for (uint32_t c = 0; c < 4; c++)
        if (source(c) == kOneInt || source(c) == kOneFloat)
            one = source(c);
    uint32_t out = word0 & ~(0xfffu << 19);
    for (uint32_t c = 0; c < 4; c++) {
        const uint8_t s = channels[c];  // MTLTextureSwizzle: Zero, One, Red, Green, Blue, Alpha
        const uint32_t from = s == 0 ? kZero : s == 1 ? one : s <= 5 ? source(s - 2u) : source(c);
        out |= from << (19 + 3 * c);
    }
    return out;
}

}  // namespace gm20b
