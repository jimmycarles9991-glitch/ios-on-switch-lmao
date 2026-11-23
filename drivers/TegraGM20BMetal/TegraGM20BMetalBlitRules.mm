// SPDX-License-Identifier: MIT
// See TegraGM20BMetalBlitRules.h.
#import "TegraGM20BMetalBlitRules.h"
#import "TegraGM20BMetalBlitImages.h"
#include "blit_planes.h"
#include "decoded.h"

bool bufferRows(uint64_t offset, uint64_t bytesPerRow, uint32_t rows, uint32_t rowBytes, uint32_t bpp, bool intoTexture,
                BufferRows &out)
{
    const uint32_t word = std::min<uint32_t>(bpp, 4);
    if (offset % bpp == 0 && bytesPerRow % bpp == 0 && (rows < 2 || bytesPerRow >= rowBytes)) {  // Metal's rules
        out = {offset, bytesPerRow, true, word};
        return true;
    }
    if (rows > 1 && bytesPerRow < rowBytes && !intoTexture)
        return false;
    const uint64_t pitch = bytesPerRow - bytesPerRow % bpp;
    if (intoTexture && rows > 1 && pitch % 16 == 0 && offset % 16 == 0) {
        if (offset || pitch < rowBytes)
            return false;
        out = {0, pitch, true, word};
        return true;
    }
    out = {offset - offset % word, bytesPerRow, offset % word == 0 && bytesPerRow % word == 0, word};
    return true;
}

// Row y of `r` (offset and bytesPerRow as given to bufferRows)
uint64_t bufferRow(const BufferRows &r, uint64_t offset, uint64_t bytesPerRow, uint32_t y)
{
    if (r.uniform)
        return r.first + y * r.pitch;
    const uint64_t at = offset + uint64_t(y) * bytesPerRow;
    return at - at % r.word;
}

MTLSize levelSize(id<MTLTexture> t, NSUInteger level)
{
    auto at = [&](NSUInteger n) {
        return std::max<NSUInteger>(1, n >> level);
    };
    return MTLSizeMake(at(t.width), at(t.height), t.textureType == MTLTextureType3D ? at(t.depth) : 1);
}

NSUInteger sliceCount(id<MTLTexture> t)
{
    return gm20b::textureLayers(uint32_t(t.textureType), uint32_t(std::min<NSUInteger>(t.arrayLength, 2048)));
}

bool inElements(SwitchMetalTexture *t, MTLOrigin &o, MTLSize &size)
{
    if (o.x > UINT32_MAX || o.y > UINT32_MAX || size.width > UINT32_MAX - t->_raw.blockW ||
        size.height > UINT32_MAX - t->_raw.blockH)
        return false;
    uint32_t x = uint32_t(o.x), y = uint32_t(o.y), w = uint32_t(size.width), h = uint32_t(size.height);
    if (!gm20b::toElements(t->_raw, x, y, w, h))
        return false;
    o.x = x, o.y = y, size.width = w, size.height = h;
    return true;
}

bool copySideOk(SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size)
{
    if (!t->_gm20bOk || t->_raw.memoryless || level >= t->_descriptor.mipmapLevelCount || !inElements(t, o, size))
        return false;
    gm20b::Texture view;
    if (!blitImage(t, uint32_t(level), view))
        return false;
    return slice < view.layers && o.x <= view.width && size.width <= view.width - o.x && o.y <= view.height &&
           size.height <= view.height - o.y && o.z <= view.depth && size.depth <= view.depth - o.z;
}

bool bufferPlanes(uint64_t offset, uint64_t row, uint64_t image, MTLSize size, uint32_t bpp, uint64_t length,
                  bool intoTexture)
{
    if (!bpp || !size.width || !size.height || !size.depth || size.width > UINT32_MAX / bpp ||
        size.height > UINT32_MAX || row > UINT32_MAX || offset > length)
        return false;
    BufferRows rows;
    if (!bufferRows(offset, row, uint32_t(size.height), uint32_t(size.width * bpp), bpp, intoTexture, rows))
        return false;
    const uint64_t pitch = rows.uniform ? rows.pitch : row;
    const uint64_t first = rows.uniform ? rows.first : offset;
    if (size.height > 1 && pitch > (UINT64_MAX - first) / (size.height - 1))
        return false;
    const uint64_t last = bufferRow(rows, offset, row, uint32_t(size.height - 1));
    if (last > length || size.width * bpp > length - last)
        return false;
    const uint64_t planeEnd = last + size.width * bpp;
    if (size.depth <= 1)
        return planeEnd <= length;
    return image % bpp == 0 && rows.uniform && planeEnd >= offset && planeEnd - offset <= image &&
           image <= (length - planeEnd) / (size.depth - 1);
}

// Plain Metal's aspect (m2v copy_plan.rs copy_unit, EV SPCrp-probe1-m4 on an Apple M4): a Depth32Float_Stencil8 texture's
// stencil where StencilFromDepthStencil names it, else its depth; its stencil view's stencil whatever the option; every
// other texture its texel, any option ignored
int blitAspect(SwitchMetalTexture *t, MTLBlitOption options)
{
    if (!t->_raw.zeta || t->_raw.format != gm20b::kZetaFormat)
        return gm20b::kAspectWhole;
    if (t->_descriptor.pixelFormat != MTLPixelFormatX32_Stencil8 && !(options & MTLBlitOptionStencilFromDepthStencil))
        return gm20b::kAspectDepth;
    SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    @synchronized(owner) {
        return gRM->raw->stencilTemp(owner->_raw) ? gm20b::kAspectStencil : kAspectNoMemory;
    }
}

uint32_t blitUnit(SwitchMetalTexture *t, int aspect)
{
    if (t->_raw.converted())
        return gm20b::planeUnit(t->_raw);
    return gm20b::bufferUnit(t->_raw, uint8_t(aspect));
}

SwitchMetalTexture *gm20bTexture(id<MTLTexture> t)
{
    return [(id)t isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)t : nil;
}

void notRawTexture(SwitchMetalCommandBuffer *cb, id<MTLTexture> t)
{
    gm20bCallInvalid(cb, PredTexForeign, uint64_t(t.pixelFormat), 0, "a blit of a texture not made by this device");
}

static constexpr const char *kMemorylessSide = "of a memoryless texture";

void skipInvalidBlit(const char *call, const char *why)
{
    gm20bSkipped(PredCallBlit);
    SWITCH_REFUSE_DETAIL("gm20b", "blit.invalid", uintptr_t(call) * 31 + uintptr_t(why),
                         "%s %s: Metal's validation error (MTLBlitCommandEncoder.h), nothing copied", call, why);
}

void invalidBlit(SwitchMetalCommandBuffer *cb, const char *call, const char *why)
{
    if (why == kMemorylessSide)
        return gm20bCallInvalid(cb, PredCallBlit, 0, 0, "a blit of a memoryless texture");
    skipInvalidBlit(call, why);
}

void cutToLevel(const char *call, SwitchMetalTexture *t, NSUInteger level, MTLOrigin o, MTLSize &size)
{
    if (level >= t->_descriptor.mipmapLevelCount)
        return;
    MTLSize at = levelSize((id<MTLTexture>)t, level);
    const NSUInteger bw = t->_gm20bOk ? t->_raw.blockW : 1, bh = t->_gm20bOk ? t->_raw.blockH : 1;
    at.width = (at.width + bw - 1) / bw * bw;
    at.height = (at.height + bh - 1) / bh * bh;
    if (o.x >= at.width || o.y >= at.height || o.z >= at.depth ||
        (size.width <= at.width - o.x && size.height <= at.height - o.y && size.depth <= at.depth - o.z))
        return;
    size.width = std::min(size.width, at.width - o.x);
    size.height = std::min(size.height, at.height - o.y);
    size.depth = std::min(size.depth, at.depth - o.z);
    constexpr const char *cut = "of a region past its level: the part inside copied";
    gm20bSkipped(PredCallBlit);
    SWITCH_REFUSE_DETAIL("gm20b", "blit.invalid", uintptr_t(call) * 31 + uintptr_t(cut),
                         "%s %s: Metal's validation error (MTLBlitCommandEncoder.h)", call, cut);
}

void roundToBlocks(const char *call, SwitchMetalTexture *t, MTLOrigin &o)
{
    const NSUInteger bw = t->_gm20bOk ? t->_raw.blockW : 1, bh = t->_gm20bOk ? t->_raw.blockH : 1;
    if (!(o.x % bw) && !(o.y % bh))
        return;
    o.x -= o.x % bw;
    o.y -= o.y % bh;
    constexpr const char *rounded = "into an origin off its format's blocks: rounded down to its block";
    gm20bSkipped(PredCallBlit);
    SWITCH_REFUSE_DETAIL("gm20b", "blit.invalid", uintptr_t(call) * 31 + uintptr_t(rounded),
                         "%s %s: Metal's validation error (MTLBlitCommandEncoder.h)", call, rounded);
}

void flatSlice(const char *call, SwitchMetalTexture *t, NSUInteger &slice)
{
    if (!slice || t->_descriptor.textureType != MTLTextureType2D)
        return;
    slice = 0;
    constexpr const char *flat = "from a slice of a 2D texture past its one: slice 0 copied";
    gm20bSkipped(PredCallBlit);
    SWITCH_REFUSE_DETAIL("gm20b", "blit.invalid", uintptr_t(call) * 31 + uintptr_t(flat),
                         "%s %s: Metal's validation error (MTLBlitCommandEncoder.h)", call, flat);
}

void copyAcrossFormats(const char *call)
{
    constexpr const char *across = "between textures of two pixel formats, one held through planes: its bytes copied";
    gm20bSkipped(PredCallBlit);
    SWITCH_REFUSE_DETAIL("gm20b", "blit.invalid", uintptr_t(call) * 31 + uintptr_t(across),
                         "%s %s: Metal's validation error (MTLBlitCommandEncoder.h)", call, across);
}

void skipUndefinedFill(const char *call)
{
    gm20bSkipped(PredCallBlit);
    SWITCH_REFUSE("gm20b", "blit.undefined",
                  "%s of a texture held through planes: undefined on Apple's GPU (it writes plane 0 with a value of no "
                  "colour), nothing written",
                  call);
}

void failNoImage(__unsafe_unretained SwitchMetalCommandBuffer *cb, const char *call)
{
    SWITCH_REFUSE("gm20b", "blit.image",
                  "%s: the driver holds no image of a side Metal's rules take: MTLCommandBufferErrorInternal", call);
    failCommandBuffer(cb, @"a blit side the driver holds no image of", MTLCommandBufferErrorInternal);
}

void failNoStencilTemp(__unsafe_unretained SwitchMetalCommandBuffer *cb, const char *call)
{
    SWITCH_REFUSE(
        "gm20b", "blit.stencil_memory",
        "%s: no memory for a texture's copy temp (Depth32Float_Stencil8's stencil, RGB10A8_2P_XR10's alpha, a 4x "
        "texture's sample rows): MTLCommandBufferErrorOutOfMemory",
        call);
    failCommandBuffer(cb, @"no memory for a copy's byte temp", MTLCommandBufferErrorOutOfMemory);
}

const char *copySideInvalid(SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size)
{
    if (t->_descriptor.storageMode == MTLStorageModeMemoryless)
        return kMemorylessSide;
    if (level >= t->_descriptor.mipmapLevelCount || slice >= sliceCount((id<MTLTexture>)t))
        return "of a level or slice past the texture's";
    // a level that ends inside a block holds that whole block (m2v copy_plan.rs)
    MTLSize at = levelSize((id<MTLTexture>)t, level);
    const NSUInteger bw = t->_gm20bOk ? t->_raw.blockW : 1, bh = t->_gm20bOk ? t->_raw.blockH : 1;
    at.width = (at.width + bw - 1) / bw * bw;
    at.height = (at.height + bh - 1) / bh * bh;
    if (o.x > at.width || size.width > at.width - o.x || o.y > at.height || size.height > at.height - o.y ||
        o.z > at.depth || size.depth > at.depth - o.z)
        return "of a region past its level";
    return nullptr;
}

namespace {
// Metal's copy unit of a texture between textures (m2v blit_plan.rs texture_unit): a stencil view's byte, a combined
// depth/stencil format's both aspects (into its own format alone), every other format's texel or block
struct CopyUnit {
    uint32_t bytes = 0, blockW = 1, blockH = 1;
    bool ownFormatOnly = false;

    bool operator==(const CopyUnit &o) const
    {
        return bytes == o.bytes && blockW == o.blockW && blockH == o.blockH && !ownFormatOnly && !o.ownFormatOnly;
    }
};

CopyUnit copyUnit(SwitchMetalTexture *t)
{
    if (t->_descriptor.pixelFormat == MTLPixelFormatX32_Stencil8)
        return {1, 1, 1, false};
    // a decoded format's block, not its stored image's texel, and into its own format alone: the stored images of two
    // forms hold other values of the same blocks
    if (const gm20b::DecodedFormat *f = gm20b::decodedFormat(uint32_t(t->_descriptor.pixelFormat)))
        return {f->blockBytes, f->blockW, f->blockH, true};
    if (t->_descriptor.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
        return {8, 1, 1, true};
    return {t->_raw.bpp, t->_raw.blockW, t->_raw.blockH, false};
}
}  // namespace

const char *textureCopyInvalid(SwitchMetalTexture *a, NSUInteger ss, NSUInteger sl, MTLOrigin so, MTLSize size,
                               SwitchMetalTexture *b, NSUInteger ds, NSUInteger dl, MTLOrigin dorg)
{
    if (a->_descriptor.sampleCount != b->_descriptor.sampleCount)
        return "between textures of other sample counts";
    if (a->_descriptor.pixelFormat != b->_descriptor.pixelFormat && !(copyUnit(a) == copyUnit(b)))
        return "between formats of other copy units";
    if (const char *side = copySideInvalid(a, ss, sl, so, size))
        return side;
    return copySideInvalid(b, ds, dl, dorg, size);
}

const char *bufferCopyInvalid(SwitchMetalTexture *t, NSUInteger slice, NSUInteger level, MTLOrigin o, MTLSize size,
                              uint64_t offset, uint64_t row, uint64_t image, uint64_t length, int aspect,
                              bool intoTexture)
{
    const uint32_t samples = uint32_t(t->_descriptor.sampleCount);
    if (samples > 1 && t->_raw.zeta)
        return "of a multisampled depth or stencil texture";
    if (const char *side = copySideInvalid(t, slice, level, o, size))
        return side;
    MTLSize elements = size;
    if (!inElements(t, o, elements))
        return "from an origin off the format's blocks";
    const uint32_t unit = blitUnit(t, aspect);
    if (samples > 1) {
        // a buffer row holds each texel's samples in order (BlitEncoder::copyBufferSamples)
        BufferRows rows;
        if (!bufferRows(offset, row, uint32_t(elements.height), uint32_t(elements.width * samples * unit), unit,
                        intoTexture, rows) ||
            !rows.uniform)
            return "of a multisampled texture at buffer rows off its texels or shorter than its samples";
        elements.width *= samples;
    }
    if (!bufferPlanes(offset, row, image, elements, unit, length, intoTexture))
        return "of buffer rows, images or an offset of part texels or past the buffer";
    return nullptr;
}

bool sameSizeLevels(id<MTLTexture> s, id<MTLTexture> d, NSUInteger &sl, NSUInteger &dl)
{
    auto same = [](MTLSize a, MTLSize b) {
        return a.width == b.width && a.height == b.height && a.depth == b.depth;
    };
    sl = dl = 0;
    for (NSUInteger l = 0; l < s.mipmapLevelCount; l++)
        if (same(levelSize(s, l), levelSize(d, 0))) {
            sl = l;
            return true;
        }
    for (NSUInteger l = 1; l < d.mipmapLevelCount; l++)
        if (same(levelSize(d, l), levelSize(s, 0))) {
            dl = l;
            return true;
        }
    return false;
}
