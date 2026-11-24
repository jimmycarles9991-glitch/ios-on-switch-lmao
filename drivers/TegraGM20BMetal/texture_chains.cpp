// SPDX-License-Identifier: MIT
// See gpu.h (lines, volumes, arrays, mip chains and block-compressed chains, each laid out by createLaidOut).
#include "lock.h"
#include "fx/fx_image.h"
#include "fx/fx_mip_probe.h"
#include "block_base.h"
#include "gpu.h"

#include <algorithm>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

bool Device::createLine(uint32_t width, uint32_t format, Texture &t, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Line;
    ask.width = width;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createVolume(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, bool view2d, Texture &t,
                          uint32_t levels, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Volume;
    ask.width = width;
    ask.height = height;
    ask.depth = depth;
    ask.levels = levels;
    ask.format = format;
    ask.view2d = view2d;
    return createLaidOut(ask, t, at);
}

bool Device::createArray(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t type, Texture &t,
                         uint32_t levels, const ArenaAt *at, uint32_t compressKind)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Array;
    ask.compressKind = compressKind;
    ask.width = width;
    ask.height = height;
    ask.layers = layers;
    ask.levels = levels;
    ask.format = format;
    ask.type = uint8_t(type);
    return createLaidOut(ask, t, at);
}

void Device::layImage(const fx::MipChain &c, const FormatInfo *f, uint8_t type, Texture &t)
{
    const bool volume = type == fx::kTexThreeD;
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.type = type;
    t.layers = volume ? 1 : c.layers;
    t.layerBytes = volume ? c.bytes : c.arrayStride;
    t.depth = c.depth;
    t.blockDepth = c.zLog2;
    if (c.levels == 1) {
        writeTic(t);
        return;
    }
    t.chain = new Texture::Chain;
    t.chain->layout = c;
    t.chain->va = t.va;
    t.chain->fmt2d = volume ? 0 : fx::twodChainFormat(t.format);
    uint32_t w[8];
    fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, c.levels, w, type);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
    SWITCH_METAL_DIAGNOSTIC(stats.chains++);
    SWITCH_METAL_DIAGNOSTIC(stats.chainBytes += c.bytes);
}

bool Device::createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t,
                              const ArenaAt *at, uint32_t compressKind)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Mips;
    ask.compressKind = compressKind;
    ask.width = width;
    ask.height = height;
    ask.levels = levels;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createBlockTextureOver(void *bytes, uint64_t length, uint32_t width, uint32_t height, uint32_t format,
                                    const fx::MipChain &layout, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    if (!blockCompressed(f) || !bytes || length < layout.bytes || layout.bpp != f->bpp ||
        !shape(width, height, format, t))
        return false;
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        if (ownChunkMemory(bytes, length, "newTextureWithBytesNoCopy"))
            return false;
    }
    const bool inPlace = blockImageInPlace(uintptr_t(bytes), layout);
    const uint32_t kind = inPlace ? TGM_LAYOUT_KIND : kPteKindPitch;
    cleanToMemory(bytes, layout.bytes);
    Import *i = new Import;
    if (!k_.importPages(bytes, length, i->bo, 0, true) || !k_.bindImport(bytes, i->bo, kind)) {
        delete i;
        return false;
    }
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    i->kind = kind;
    i->refs = 1;
    i->base = uintptr_t(bytes);
    i->bytes = length;
    SWITCH_METAL_DIAGNOSTIC(stats.imports++);
    SWITCH_METAL_DIAGNOSTIC(stats.importsMade++);
    SWITCH_METAL_DIAGNOSTIC(stats.importBytes += i->bo.bytes);
    if (!inPlace && !blAlloc(l, layout.bytes, layout.align, t.span)) {
        release(i, deferred);
        return false;
    }
    if ((!inPlace && !(t.imagePitchVa = blPitchVa(t.span))) || !(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(l, t.span, Memory::Bl, none, deferred);
        release(i, deferred);
        return false;
    }
    if (inPlace) {
        t.import = i;
        t.va = i->bo.va;
        t.cpu = static_cast<uint8_t *>(bytes);
    } else {
        t.callerPages = i;
        t.va = t.span.va;
        t.cpu = t.span.cpu;
    }
    t.storage = {t.va, layout.bytes, inPlace};
    layBlocks(layout, f, width, height, uint8_t(layout.layers > 1 ? fx::kTexTwoDArray : fx::kTexTwoD), t);
    return true;
}

bool Device::createBlockTexture(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, uint32_t format,
                                uint8_t type, Texture &t, uint32_t depth, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Block;
    ask.width = width;
    ask.height = height;
    ask.depth = depth;
    ask.levels = levels;
    ask.layers = layers;
    ask.format = format;
    ask.type = type;
    return createLaidOut(ask, t, at);
}

void Device::layBlocks(const fx::MipChain &layout, const FormatInfo *f, uint32_t width, uint32_t height, uint8_t type,
                       Texture &t)
{
    t.pitch = layout.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = layout.yLog2;
    t.layers = layout.layers;
    t.layerBytes = type == fx::kTexThreeD ? layout.bytes : layout.arrayStride;
    t.depth = layout.depth;
    t.blockDepth = layout.zLog2;
    t.type = type;
    uint32_t w[8];
    fx::mipChainTic(layout, t.va, f->tic0, f->srgb, 0, layout.levels, w, t.type);
    setField(w, 143, 128, width - 1);  // WIDTH_MINUS_ONE in texels (NIL's extent_px)
    setField(w, 175, 160, height - 1);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);
    if (layout.levels > 1) {
        t.chain = new Texture::Chain;
        t.chain->layout = layout;
        t.chain->va = t.va;
    }
}

}  // namespace gm20b
