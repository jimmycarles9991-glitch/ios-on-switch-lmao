// SPDX-License-Identifier: MIT
// fillTexture:level:slice:region:color:[pixelFormat:] (Apple SPI) and generateMipmapsForTexture: as recorded. A fill is
// placed, cut and packed here as plain Metal does it (m2v blit_fill.rs, fill_value.rs: EVs FRT-fil1 to fil4 and
// XRR2-fill1 to fill4 on an Apple M4 under macOS 27.0.1), and written by the copy engine or the 2D engine
// (TegraGM20BMetalEncodeBlit.mm).
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalBlitImages.h"
#import "TegraGM20BMetalBlitRules.h"
#include "fill_record.h"
#include "fill_texel.h"
#include "mip.h"
#include "fx/fx_mip_probe.h"

#include <algorithm>

namespace {

// The units `from` .. `from + count` of an axis of `units`, cut there
void cut(NSUInteger from, NSUInteger count, uint32_t units, uint32_t &first, uint32_t &end)
{
    first = uint32_t(std::min<NSUInteger>(from, units));
    end = uint32_t(std::min<NSUInteger>(count > NSUIntegerMax - from ? NSUIntegerMax : from + count, units));
}

// The layer a fill of `slice` writes: a 1D, 2D or 3D texture ignores the slice, a cube takes it modulo 6, and an array (a
// multisampled texture and a texture buffer as one of one slice) writes nothing past its slices
bool fillLayer(SwitchMetalTexture *t, NSUInteger slice, uint32_t &layer)
{
    switch (t->_descriptor.textureType) {
    case MTLTextureType1D:
    case MTLTextureType2D:
    case MTLTextureType3D:
        layer = 0;
        return true;
    case MTLTextureTypeCube:
        layer = uint32_t(slice % 6);
        return true;
    default:
        layer = uint32_t(std::min<NSUInteger>(slice, UINT32_MAX));
        return slice < sliceCount((id<MTLTexture>)t);
    }
}

// One unit of `t` as the fill leaves it, from the colour packed in `size` bytes (repeated from their start and cut at
// the unit's bytes; none: zeros), and the words of it the fill writes: Depth32Float_Stencil8's depth word, its stencil
// view's stencil byte (the word after the depth, whose other bytes are padding), a texel of RGB10A8_2P_XR10's colour
// word (its alpha kept); every other format its texel or block
void fillUnit(SwitchMetalTexture *t, const uint8_t packed[16], uint32_t size, gm20b::FillRecord &f)
{
    const MTLPixelFormat format = t->_descriptor.pixelFormat;
    uint32_t bytes = t->_raw.bpp, from = 0, unit = t->_raw.bpp;
    if (format == MTLPixelFormatX32_Stencil8) {
        from = 4, unit = 1, f.words = 2;
    } else if (format == MTLPixelFormatDepth32Float_Stencil8) {
        unit = 4, f.words = 1;
    } else if (uint32_t(format) == 550 || uint32_t(format) == 551) {  // RGB10A8_2P_XR10 and its sRGB twin
        unit = 4, f.words = 1;
    }
    f.bytes = uint8_t(std::min<uint32_t>(bytes, sizeof f.texel));
    for (uint32_t k = 0; k < unit && from + k < sizeof f.texel; k++)
        f.texel[from + k] = size ? packed[k % size] : 0;
}

// Whether encodeMipmaps has an engine for every step of `t`'s chain: the 2D engine's surface format, or a pack state the
// device's pack holds
bool mipRouted(SwitchMetalTexture *t)
{
    gm20b::Device &d = *gRM->raw;
    SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    const uint32_t format = t->_raw.format;
    if (t->_raw.type == gm20b::fx::kTexThreeD)
        return d.mip3dPipeline(format) || d.ownPipeline(gm20b::wordMip3dState(format));
    const uint32_t fmt2d = format == owner->_raw.format ? owner->_raw.chain->fmt2d : gm20b::fx::twodChainFormat(format);
    return fmt2d || d.mipPipeline(format) || gm20bFormatMips(format) || d.ownPipeline(gm20b::wordMipState(format));
}

void recordFill(__unsafe_unretained SwitchMetalCommandBuffer *cb, id<MTLTexture> t, NSUInteger level, NSUInteger slice,
                MTLRegion region, MTLClearColor color, MTLPixelFormat packedIn)
{
    constexpr const char *call = "fillTexture:level:slice:region:color:";
    if (!t)
        return gm20bCallInvalid(cb, PredTexForeign, 0, 0, "a fill of a nil texture");
    gm20b::FillRecord f;
    if (SwitchMetalTexture *x = gm20bTexture(t)) {
        if (x->_descriptor.storageMode == MTLStorageModeMemoryless)
            return gm20bCallInvalid(cb, PredCallBlit, level, slice, "a fill of a memoryless texture");
        if (level >= x->_descriptor.mipmapLevelCount)
            return skipInvalidBlit(call, "of a level past the texture's");
        if (!fillLayer(x, slice, f.layer))
            return skipInvalidBlit(call, "of a slice past the array's");
        if (x->_raw.converted() || x->_raw.xrPlanes)
            return skipUndefinedFill(call);
        gm20b::Texture image;
        if (!blitImage(x, uint32_t(level), image))
            return failNoImage(cb, call);
        MTLSize at = levelSize(t, level);
        if (t.textureType == MTLTextureTypeTextureBuffer && t.buffer && x->_raw.bpp) {
            // an M4 Max fills a texture buffer's region along x alone, row 0 whatever its y, up to its buffer's end
            // past the texture's own texels (macOS 27.2.1)
            region.origin.y = 0;
            region.size.height = 1;
            at.width = (t.buffer.length - std::min(t.bufferOffset, t.buffer.length)) / x->_raw.bpp;
        }
        const uint32_t bw = x->_raw.blockW, bh = x->_raw.blockH;
        // a compressed format's region counts blocks
        cut(region.origin.x, region.size.width, uint32_t((at.width + bw - 1) / bw), f.x0, f.x1);
        cut(region.origin.y, region.size.height, uint32_t((at.height + bh - 1) / bh), f.y0, f.y1);
        if (t.textureType == MTLTextureType3D)
            cut(region.origin.z, region.size.depth, uint32_t(at.depth), f.z0, f.z1);
        if (f.x0 == f.x1 || f.y0 == f.y1 || f.z0 == f.z1)
            return;
        const double rgba[4] = {color.red, color.green, color.blue, color.alpha};
        uint8_t packed[16];
        fillUnit(x, packed, gm20b::fillTexel(uint32_t(packedIn), rgba, packed), f);
    } else {
        return notRawTexture(cb, t);
    }
    [cb retain:t];
    if ([(id)t isKindOfClass:[SwitchMetalTexture class]])
        declareGpuWrites((SwitchMetalTexture *)t);
    Cmd m = cmd(OpFillTexture);
    m.obj = (__bridge void *)t;
    m.c = uint64_t(level) | uint64_t(slice) << 32;
    m.x = cb->_extra.size();
    const uint64_t r[4] = {region.origin.x, region.origin.y, region.size.width, region.size.height};
    cb->_extra.append(r, r + 4);
    for (double v : {color.red, color.green, color.blue, color.alpha}) {
        const float x = float(v);
        uint32_t bits;
        memcpy(&bits, &x, sizeof bits);
        cb->_extra.push_back(bits);
    }
    uint64_t words[gm20b::kFillRecordWords];
    gm20b::packFill(f, words);
    cb->_extra.append(words, words + gm20b::kFillRecordWords);
    [cb record:m];
}

}  // namespace

@interface SwitchMetalBlitEncoder (Fill)
@end

@implementation SwitchMetalBlitEncoder (Fill)

- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
    SwitchMetalTexture *x = [(id)t isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)t : nil;
    if (!x || x->_device != _cb->_queue->_device) {
        failCommandBuffer(_cb, @"generateMipmapsForTexture: requires a texture of this device",
                          MTLCommandBufferErrorInvalidResource, PredTexForeign);
        return;
    }
    if (x->_descriptor.storageMode == MTLStorageModeMemoryless) {
        failCommandBuffer(_cb, @"generateMipmapsForTexture: cannot access a memoryless texture",
                          MTLCommandBufferErrorInvalidResource, PredCallBlit);
        return;
    }
    const uint32_t format = uint32_t(x->_descriptor.pixelFormat);
    if (x->_descriptor.mipmapLevelCount <= 1)
        return;
    // Plain Metal leaves these bytes unchanged (mipmap_plan.rs leaves_as_is), despite API validation's assertions.
    if (gm20b::zetaFormatIndex(format) < gm20b::kZetaFormats || format == MTLPixelFormatX32_Stencil8)
        return skipInvalidBlit("generateMipmapsForTexture:", "of a depth or stencil format");
    __unsafe_unretained SwitchMetalTexture *owner = x->_levelsOwner ?: x;
    // its levels are the copy engine's copies of level 0's corner (level_zero.h), as an M4 Max reads them; an M4 Max's
    // generateMipmapsForTexture: of one completes and changes nothing it reads (validation on and off: m2v-textures13
    // probes/pmips/pmips.txt)
    if (owner->_raw.levelZero)
        return;
    if (!owner->_raw.chain) {
        failCommandBuffer(_cb, @"generateMipmapsForTexture: a native mipmapped texture has no mip chain",
                          MTLCommandBufferErrorInternal);
        return;
    }
    if (x->_raw.blockW > 1 || x->_raw.blockH > 1)
        return skipInvalidBlit("generateMipmapsForTexture:", "of a compressed format, which Metal does not filter");
    if (gm20b::fx::integerFormat(x->_raw.format) && x->_raw.type == gm20b::fx::kTexThreeD) {
        SWITCH_REFUSE(
            "gm20b", "mipmaps.state",
            "generateMipmapsForTexture: of a 3D texture of integer format %u: no engine of the device takes a "
            "level's planes from the level above's (the 2D engine moves one plane): MTLCommandBufferErrorInternal",
            x->_raw.format);
        failCommandBuffer(_cb, @"generateMipmapsForTexture: no engine makes a 3D integer texture's levels",
                          MTLCommandBufferErrorInternal);
        return;
    }
    if (!gm20b::fx::integerFormat(x->_raw.format) && !mipRouted(x)) {
        SWITCH_REFUSE("gm20b", "mipmaps.state",
                      "generateMipmapsForTexture: of format %u: the device's pack holds no mip state for it: "
                      "MTLCommandBufferErrorInternal",
                      x->_raw.format);
        failCommandBuffer(_cb, @"generateMipmapsForTexture: the pack lacks this format's mip state",
                          MTLCommandBufferErrorInternal);
        return;
    }
    declareGpuWrites(x);
    uint64_t probe2d = 0;
#if SWITCH_METAL_DIAGNOSTICS
    if (gRM->raw->probeMipCandidates.load(std::memory_order_acquire))
        probe2d = gm20b::fx::twodCandidateFormat(x->_raw.format);
#endif
    setBinding(_cb, OpMipmaps, 0, 0, t, probe2d);
}

- (void)fillTexture:(id<MTLTexture>)t
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color
{
    recordFill(_cb, t, level, slice, region, color, t.pixelFormat);
}

// The colour packed in `format` instead of the texture's own, repeated or cut at its texel (EV FRT-fil4-m4)
- (void)fillTexture:(id<MTLTexture>)t
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color
        pixelFormat:(MTLPixelFormat)format
{
    recordFill(_cb, t, level, slice, region, color, format);
}

@end
