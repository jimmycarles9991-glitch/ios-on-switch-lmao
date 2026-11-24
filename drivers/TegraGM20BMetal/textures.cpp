// SPDX-License-Identifier: MIT
// See gpu.h (textures the device lays out, their memory and their release).
#include "lock.h"
#include "fx/fx_image.h"
#include "block_base.h"
#include "gpu.h"
#include "srgb_pair.h"
#include "fx/fx_mip_probe.h"
#include "multisample.h"
#include "texel_rows.h"
#include "TegraGM20BLayout.h"

#include <mach/mach.h>
#include <algorithm>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

bool shape(uint32_t width, uint32_t height, uint32_t format, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    if (!f || !width || !height || width > 16384 || height > 16384)
        return false;
    t = {};
    t.blockW = f->blockW;
    t.blockH = f->blockH;
    t.texelWidth = width;
    t.texelHeight = height;
    t.width = (width + f->blockW - 1) / f->blockW;
    t.height = (height + f->blockH - 1) / f->blockH;
    t.format = format;
    t.bpp = f->bpp;
    t.renderable = f->rt != 0;
    return true;
}

// NIL's layout of a block-compressed image: its blocks tiled, the block height chosen by its height in texels (Tiling::choose
// reads extent_px) and clamped to its rows of blocks
static void blockLayout(const Texture &t, tgm_layout &bl)
{
    const uint8_t y =
        uint8_t(std::min<uint32_t>(tgm_layout_y_log2(t.texelHeight), tgm_layout_log2_ceil((t.height + 7) / 8)));
    const uint64_t block = 512ull << y;
    bl.y_log2 = y;
    bl.rows = (t.height + (8u << y) - 1) / (8u << y) * (8u << y);
    bl.align = std::max<uint64_t>(block, 4096);
    bl.bytes = (uint64_t(bl.row_bytes) * bl.rows + bl.align - 1) / bl.align * bl.align;
}

// createTexture's block-linear layout of shaped `t`, where it is asked tiled and its shape tiles: every kFormats format, a
// colour one under colour kind 0xFE (one the GM20B renders none of too, as its chains are) or a block-compressed one
// (NVK's optimal image)
static bool plainTiled(const Texture &t, bool tiled, tgm_layout &bl)
{
    const bool blocks = t.blockW > 1 || t.blockH > 1;
    if (!tiled || !tgm_layout_tiled(t.width, t.height, t.bpp, &bl))
        return false;
    if (blocks)
        blockLayout(t, bl);
    return true;
}

static void tiledRows(Texture &t, const tgm_layout &bl)
{
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
}

static void chainRows(Texture &t, const fx::MipChain &c, uint64_t layerBytes)
{
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = layerBytes;
}

bool Device::createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc, bool tiled,
                           const ArenaAt *at)
{
    if (at) {
        TextureAsk ask;
        ask.width = width;
        ask.height = height;
        ask.format = format;
        ask.tiled = tiled;
        return createLaidOut(ask, t, at);
    }
    if (!shape(width, height, format, t))
        return false;
    const bool blocks = t.blockW > 1 || t.blockH > 1;
    t.pitch = (t.width * t.bpp + 255) & ~255u;
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    const uint64_t bytes = uint64_t(t.pitch) * t.height;
    tgm_layout bl;
    const bool laid = plainTiled(t, tiled, bl);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeCompressNext.exchange(false) && laid && !wc && !blocks && compressedKind(t.bpp, 0))
        return probeCompressible(bl, t, deferred);
#endif
    if (laid && blAlloc(l, bl.bytes, bl.align, t.span)) {
        tiledRows(t, bl);
    } else if (blocks) {
        return false;
    } else if (wc && wcAlloc(l, bytes, t.span)) {
        t.wc = true;
        SWITCH_METAL_DIAGNOSTIC(stats.wcBytes += t.span.bytes);
    } else {
        if (wc)
            SWITCH_METAL_DIAGNOSTIC(stats.wcFallbacks++);
        if (!poolAlloc(l, bytes, t.span))
            return false;
    }
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(l, t.span, memoryOf(t), none, deferred);
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::layout(const TextureAsk &ask, Texture &t, uint64_t &bytes, uint64_t &align, uint32_t &kind,
                    fx::MipChain *chain)
{
    fx::MipChain own;
    fx::MipChain &c = chain ? *chain : own;
    const FormatInfo *f = formatInfo(ask.format);
    kind = fx::kBlockLinearKind;
    switch (ask.kind) {
    case TextureAsk::Plain: {
        tgm_layout bl;
        if (!shape(ask.width, ask.height, ask.format, t))
            return false;
        if (plainTiled(t, ask.tiled, bl)) {
            tiledRows(t, bl);
            bytes = bl.bytes;
            align = bl.align;
            return true;
        }
        if (t.blockW > 1 || t.blockH > 1)
            return false;
        t.pitch = (t.width * t.bpp + 255) & ~255u;
        bytes = uint64_t(t.pitch) * t.height;
        align = 256;
        kind = kPteKindPitch;
        return true;
    }
    case TextureAsk::Line: {
        tgm_layout bl;
        if (!shape(ask.width, 1, ask.format, t) || !tgm_layout_tiled(ask.width, 1, t.bpp, &bl))
            return false;
        tiledRows(t, bl);
        t.type = fx::kTexOneD;
        bytes = bl.bytes;
        align = bl.align;
        return true;
    }
    case TextureAsk::Mips:
        if (!f || ask.levels < 2 || blockCompressed(f) ||
            !fx::mipChainLayout(ask.width, ask.height, ask.levels, f->bpp, c) ||
            !shape(ask.width, ask.height, ask.format, t))
            return false;
        chainRows(t, c, c.bytes);
        break;
    case TextureAsk::Volume:
        if (!shape(ask.width, ask.height, ask.format, t) ||
            !fx::volumeLayout(ask.width, ask.height, ask.depth, ask.levels, t.bpp, ask.view2d, c))
            return false;
        chainRows(t, c, c.bytes);
        t.type = fx::kTexThreeD;
        t.depth = ask.depth;
        t.blockDepth = c.zLog2;
        break;
    case TextureAsk::Array:
        if (!shape(ask.width, ask.height, ask.format, t) ||
            !fx::arrayLayout(ask.width, ask.height, ask.layers, ask.levels, t.bpp, c))
            return false;
        chainRows(t, c, c.arrayStride);
        t.type = ask.type;
        t.layers = ask.layers;
        break;
    case TextureAsk::Multisample:
        if (!shape(ask.width, ask.height, ask.format, t) || blockCompressed(f) ||
            !fx::msLayout(t.width, t.height, ask.samples, t.bpp, c))
            return false;
        chainRows(t, c, uint64_t(c.level[0].rowBytes) * c.level[0].rows);
        t.samples = uint8_t(ask.samples);
        break;
    case TextureAsk::Zeta:
        if (!zetaImageLayout(ask.width, ask.height, ask.format, ask.samples, ask.layers, ask.levels, ask.type,
                             ask.sampled, t, c, bytes, align))
            return false;
        align = std::max(align, kBlAlign);
        kind = kZetaFormatInfo[zetaFormatIndex(ask.format)].kind;
        return true;
    case TextureAsk::Block:
        if (!blockCompressed(f) || !shape(ask.width, ask.height, ask.format, t) ||
            !(ask.type == fx::kTexThreeD ? fx::volumeLayout(ask.width, ask.height, ask.depth, ask.levels, f->bpp, false,
                                                            c, f->blockW, f->blockH)
                                         : fx::blockChainLayout(ask.width, ask.height, ask.levels, ask.layers,
                                                                f->blockW, f->blockH, f->bpp, c)))
            return false;
        chainRows(t, c, c.arrayStride);
        break;
    }
    bytes = c.bytes;
    align = c.align;
    return true;
}

// An arena's block-linear memory at NVK's image alignment, as m2v's twin over it in place takes it
static void arenaRounded(uint32_t kind, uint64_t &bytes, uint64_t &align)
{
    if (kind == kPteKindPitch)
        return;
    bytes = (bytes + kBlAlign - 1) & ~(kBlAlign - 1);
    align = std::max(align, kBlAlign);
}

bool Device::footprint(const TextureAsk &ask, uint64_t &bytes, uint64_t &align)
{
    Texture t;
    uint32_t kind = 0;
    if (!layout(ask, t, bytes, align, kind))
        return false;
    arenaRounded(kind, bytes, align);
    if (!ask.standIn)
        return true;
    // the stand-in of the texture `ask` makes: its kind, levels, layers, planes or samples (layout() leaves t.chain unset,
    // which standInAsk reads for levels)
    TextureAsk in = ask;
    in.format = srgbPair(ask.format) ? kSrgbPairStandInFormat : kXrStandInFormat;
    in.tiled = true;
    in.standIn = false;
    in.compressKind = 0;
    uint64_t standIn = 0, standInAlign = 0;
    if (!footprint(in, standIn, standInAlign))
        return false;
    bytes = (bytes + standInAlign - 1) / standInAlign * standInAlign + standIn;
    align = std::max(align, standInAlign);
    return true;
}

// Its codes' shape (xr_parts.h converts the pair texel for texel): a one-level 2D texture's tiled, else the kind
// and levels, layers, planes or samples of the texture's own layout
TextureAsk Device::standInAsk(const Texture &t)
{
    TextureAsk ask;
    ask.width = t.width;
    ask.height = t.height;
    ask.format = srgbPair(t.format) ? kSrgbPairStandInFormat : kXrStandInFormat;
    ask.tiled = true;
    ask.levels = t.chain ? t.chain->layout.levels : 1;
    if (t.samples > 1) {
        ask.kind = TextureAsk::Multisample;
        ask.samples = t.samples;
    } else if (t.type == fx::kTexThreeD) {
        ask.kind = TextureAsk::Volume;
        ask.depth = t.depth;
        ask.view2d = t.blockDepth == 0;
    } else if (t.type == fx::kTexOneD) {
        ask.kind = TextureAsk::Line;
    } else if (t.type != fx::kTexTwoD || t.layers > 1) {
        ask.kind = TextureAsk::Array;
        ask.type = t.type;
        ask.layers = std::max(t.layers, 1u);
    } else if (t.chain) {
        ask.kind = TextureAsk::Mips;
    }
    return ask;
}

bool Device::createLaidOut(const TextureAsk &ask, Texture &t, const ArenaAt *at)
{
    uint64_t bytes = 0, align = 0;
    uint32_t kind = 0;
    Texture::Chain *chain = ask.kind == TextureAsk::Mips ? new Texture::Chain : nullptr;
    const bool image = ask.kind == TextureAsk::Volume || ask.kind == TextureAsk::Array || ask.kind == TextureAsk::Block;
    const bool zetaLevels = ask.kind == TextureAsk::Zeta && ask.levels > 1;
    fx::MipChain imageLayout;
    if ((!at && ask.kind == TextureAsk::Plain) || !layout(ask, t, bytes, align, kind,
                                                          chain                 ? &chain->layout
                                                          : image || zetaLevels ? &imageLayout
                                                                                : nullptr)) {
        delete chain;
        return false;
    }
    if (at)
        arenaRounded(kind, bytes, align);
    const bool zeta = ask.kind == TextureAsk::Zeta;
    const uint32_t z = zetaFormatIndex(ask.format);
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    Span owned;
    Bo comp;
    const bool compressed = ask.compressKind && !at && !zeta && takeCompressed(bytes, ask.compressKind, comp);
    const bool placed = compressed || (at     ? arenaSpan(l, *at, bytes, align, kind, t)
                                       : zeta ? tiledAlloc(l, zChunks_[z], kind, bytes, align, owned)
                                              : blAlloc(l, bytes, align, t.span));
    if (!placed) {
        delete chain;
        return false;
    }
    const Span &span = zeta && !at ? owned : t.span;
    // every texture's header is made here, before another thread can see the texture: encoders read t.tic unlocked
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        if (compressed)
            compReadyPush(comp, ask.compressKind);
        else if (zeta && !at)
            tiledFree(zChunks_[z], owned, deferred);
        else
            freeSpan(l, t.span, memoryOf(t), none, deferred);
        delete chain;
        return false;
    }
    t.va = span.va;
    if (!at) {
        SWITCH_METAL_DIAGNOSTIC(t.heldBytes = bytes);
        SWITCH_METAL_DIAGNOSTIC(stats.textureBytes += bytes);
    }
    if (compressed) {
        noteCompressed(ask, comp, bytes, t);
    } else if (zeta) {
        t.storage = {span.va, bytes, false, span.storage};
        if (!at)
            ownedZeta_[span.va] = {z, owned};
    } else {
        t.cpu = span.cpu;
    }
    if (chain) {
        const FormatInfo *f = formatInfo(ask.format);
        chain->va = t.va;
        chain->fmt2d = fx::twodChainFormat(ask.format);
        uint32_t w[8];
        fx::mipChainTic(chain->layout, chain->va, f->tic0, f->srgb, 0, ask.levels, w);
        // the texture's header: the chain's (visible at the next header invalidate)
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
        t.chain = chain;
        SWITCH_METAL_DIAGNOSTIC(stats.chains++);
        SWITCH_METAL_DIAGNOSTIC(stats.chainBytes += chain->layout.bytes);
    } else if (ask.kind == TextureAsk::Block) {
        layBlocks(imageLayout, formatInfo(ask.format), ask.width, ask.height, ask.type, t);
    } else if (image) {
        layImage(imageLayout, formatInfo(ask.format), t.type, t);
    } else if (zetaLevels) {
        t.chain = new Texture::Chain;
        t.chain->layout = imageLayout;
        t.chain->va = t.va;
        uint32_t w[8];
        fx::mipChainTic(imageLayout, t.va, kZetaFormatInfo[z].tic0, false, 0, ask.levels, w, t.type);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
    } else {
        writeTic(t);
    }
    return true;
}

void Device::destroyTexture(Texture &t)
{
#if SWITCH_METAL_DIAGNOSTICS
    if (glassMemOn.load(std::memory_order_relaxed))
        glassMem->forget(t.storageRange().va ? t.storageRange().va : t.va, t.view ? t.tic : 0);
#endif
    if (t.view) {
        // its stand-in's and pool image's views are its own (viewCompanions); every other pointer is its texture's
        if (t.sampled)
            t.tic = 0;  // its header is the pool image view's
        for (Texture *own : {t.standIn, t.sampled})
            if (own) {
                destroyTexture(*own);
                delete own;
            }
        std::lock_guard<gm20b::Mutex> l(lock_);
        forgetView(t);
        if (t.tic && t.xrPlanes)
            freeTicPair(t.tic);
        else if (t.tic)
            freeTic(t.tic);
        t = {};
        return;
    }
    closeSparseWindow(t);
    if (t.xrPlanes && t.chroma && t.chroma->memoryless)
        t.chroma->tic = 0;  // its header is the pair's second, freed with the pair
    for (Texture *plane : {t.luma, t.chroma, t.planarStandIn, t.stencilTemp, t.sampleRows})
        if (plane) {
            destroyTexture(*plane);
            delete plane;
        }
    t.luma = t.chroma = t.planarStandIn = t.stencilTemp = t.sampleRows = nullptr;
    delete t.ycbcrStore;
    t.ycbcrStore = nullptr;
    if (t.standIn) {
        destroyTexture(*t.standIn);
        delete t.standIn;
        t.standIn = nullptr;
    }
    if (t.storeView) {
        destroyTexture(*t.storeView);
        delete t.storeView;
        t.storeView = nullptr;
    }
    if (t.sampled) {  // its header is the pool image's
        destroyTexture(*t.sampled);
        delete t.sampled;
        t.sampled = nullptr;
        t.tic = 0;
    }
    DeferredFrees deferred(*this);
    std::unique_lock<gm20b::Mutex> l(lock_);
    if (t.tic && t.xrPlanes) {
        freeTicPair(t.tic);
    } else if (t.tic) {
        freeTic(t.tic);
    }
    if (t.zeta)
        freeOwnedZeta(t.va, deferred);
    if (t.chain) {
        for (const uint32_t *tics : {t.chain->levelTic, t.chain->levelCodesTic})
            for (uint32_t level = 0; level < fx::kMaxLevels; level++)
                if (tics[level])
                    freeTic(tics[level]);
        delete t.chain;
        t.chain = nullptr;
    }
    if (t.import)
        release(t.import, deferred);
    if (t.texelRows) {
        release(t.texelRows->import, deferred);
        delete t.texelRows;
    }
    if (t.callerPages)
        release(t.callerPages, deferred);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (auto it = probeCompressed.find(t.va); t.comptags && !t.span.valid() && it != probeCompressed.end()) {
        deferred.add({it->second});
        probeCompressed.erase(it);
    }
#endif
    releasePrivateCompressed(t, deferred);
    freeSpan(l, t.span, memoryOf(t), t.lease, deferred);
    SWITCH_METAL_DIAGNOSTIC(stats.textureBytes -= t.heldBytes);
    t = {};
}

bool Device::createStandIn(Texture &t, const ArenaAt *at)
{
    const bool pair = srgbPair(t.format);
    if (t.standIn || (!pair && !ownPipeline(xrConversionOf(t.format, t.xrPlanes))))
        return t.standIn != nullptr;
    Texture *s = new Texture;
    const TextureAsk ask = standInAsk(t);
    const bool made = ask.kind == TextureAsk::Plain ? createTexture(t.width, t.height, ask.format, *s, false, true, at)
                                                    : createLaidOut(ask, *s, at);
    if (!made) {
        delete s;
        return false;
    }
    if (pair)
        swizzle(*s, kSrgbPairSwizzle);
    t.standIn = s;
    return true;
}

Texture::StorageRange Texture::storageRange() const
{
    if (storage.bytes)
        return storage;
    if (span.bytes)
        return {span.va, span.bytes, false, span.storage};
    if (chain)
        return {chain->va, chain->layout.bytes, false};
    if ((blockLinear || nativeGeneration) && layerBytes && layers && layerBytes <= UINT64_MAX / layers)
        return {va, layerBytes * layers, false};
    return {};
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
bool Device::probeRetarget(const Texture &a, const Texture *to)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    if (!a.tic || (to && !to->tic))
        return false;
    if (to)
        memcpy(tic_.cpu + 32 * a.tic, tic_.cpu + 32 * to->tic, 32);
    else
        writeTic(a);
    return true;
}

bool Device::probeCompressible(const tgm_layout &bl, Texture &t, DeferredFrees &deferred)
{
    const uint32_t kind = compressedKind(t.bpp, 0);
    Bo bo;
    if (k_.allocCompressible(compBytes(bl.bytes), bo) != KERN_SUCCESS)
        return false;
    if (k_.bindCompressed(bo, kind) != KERN_SUCCESS) {
        deferred.add({bo});
        return false;
    }
    t.va = bo.va;
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
    t.comptags = true;
    if (!(t.tic = allocTic())) {
        deferred.add({bo});
        return false;
    }
    probeCompressed[bo.va] = bo;
    writeTic(t);
    return true;
}
#endif

}  // namespace gm20b
