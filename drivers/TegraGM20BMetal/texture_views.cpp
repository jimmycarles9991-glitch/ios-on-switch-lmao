// SPDX-License-Identifier: MIT
// See gpu.h (texture views and the headers of levels, attachments and views).
#include "lock.h"
#include "fx/fx_image.h"
#include "attachment_target.h"
#include "gpu.h"
#include "multisample.h"
#include "srgb_pair.h"
#include "target.h"
#include "view.h"
#include "xr.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "gm20b_gpu_internal.h"

namespace gm20b {

// A chain's header over layers from `va`, levels [baseLevel, +levels) seen as `type`; a block format's extent in texels
// (NIL's extent_px), as layBlocks writes it
static void chainViewHeader(const Texture &of, const fx::MipChain &c, uint64_t va, const FormatInfo &f,
                            uint32_t baseLevel, uint32_t levels, uint32_t type, uint32_t w[8])
{
    fx::mipChainTic(c, va, f.tic0, f.srgb, baseLevel, levels, w, type);
    if (of.blockW > 1 || of.blockH > 1) {
        setField(w, 143, 128, of.texelWidth - 1);
        setField(w, 175, 160, of.texelHeight - 1);
    }
}

// A depth/stencil texture viewed in its own format, or in X32_Stencil8 (261) for its stencil: NVK's stencil-only header
// (stencil_bits.h kStencilViews) over the same image. The view keeps its texture's format, which every zeta table
// is indexed by; only its header reads the stencil.
bool Device::createZetaView(const Texture &of, uint32_t format, Texture &t)
{
    constexpr uint32_t kX32Stencil8 = 261;
    const uint32_t stencilWord0 = format == kX32Stencil8 ? stencilViewWord0(of.format) : 0;
    if ((format != of.format && !stencilWord0) || of.memoryless)
        return false;
    std::lock_guard<gm20b::Mutex> l(lock_);
    const Texture::Chain *chain = of.chain;
    t = of;
    t.storage = of.storageRange();
    memcpy(t.swizzle, kIdentitySwizzle, 4);
    t.view = true;
    t.renderable = of.renderable && !stencilWord0;
    t.shadowTarget = false;
    t.import = nullptr;
    t.span = {};
    t.lease = nullptr;
    t.chain = nullptr;
    if (!(t.tic = allocTic()))
        return false;
    if (chain) {  // every level, as the texture's own header names them
        uint32_t w[8];
        fx::mipChainTic(chain->layout, chain->va, kZetaFormatInfo[zetaFormatIndex(of.format)].tic0, false, 0,
                        chain->layout.levels, w, of.type);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
    } else {
        writeTic(t);
    }
    if (stencilWord0)
        reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic)[0] = stencilWord0;
    noteTicWrite();
    return true;
}

// A depth texture's layers as an array or cube: NIL's array header, where the image's layers lie as NIL's array lays them
// (the header's layer stride is the one it implies)
bool zetaArrayHeader(const Texture &t, uint32_t type, uint32_t w[8])
{
    fx::MipChain c;
    if (!fx::arrayLayout(t.width, t.height, t.layers, 1, t.bpp, c) || c.yLog2 != t.blockHeight ||
        c.arrayStride != t.layerBytes)
        return false;
    fx::mipChainTic(c, t.va, kZetaFormatInfo[zetaFormatIndex(t.format)].tic0, false, 0, 1, w, type);
    return true;
}

bool zetaImageLayout(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, uint32_t layers,
                     uint32_t levels, uint8_t type, bool sampled, Texture &t, fx::MipChain &c, uint64_t &bytes,
                     uint64_t &align)
{
    if (!samples || !layers || layers > 2048 || !levels || (samples > 1 && (layers > 1 || levels > 1)) ||
        !zetaShape(width, height, format, samples, t, bytes, align))
        return false;
    if (layers == 1 && levels == 1)
        return true;
    t.layers = layers;
    t.type = type;
    if (levels == 1) {
        uint32_t header[8];
        bytes = (t.layerBytes * layers + align - 1) / align * align;
        return !sampled || zetaArrayHeader(t, type, header);
    }
    if (!fx::arrayLayout(width, height, layers, levels, t.bpp, c))
        return false;
    t.pitch = c.level[0].rowBytes;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = c.arrayStride;
    bytes = c.bytes;
    align = std::max(align, c.align);
    return true;
}

// The images view `t` of `of` in `format` samples beside its own header, each the same part of a texture of `of`'s layout
// (`like` makes it): over `of`'s pool image where `of` is sampled through one (its rows off 32); over `of`'s stand-in where
// the view reads `of`'s transfer; over a stand-in of its own of `of`'s whole layout where it reads another transfer of the
// same codes (BGR10_XR <-> BGR10_XR_sRGB, BGRA10_XR <-> BGRA10_XR_sRGB, a plain format's bits as an extended-range or
// RG8Unorm_sRGB one), which that header object owns. A view in a plain format reads the codes themselves. False: no memory
// or header for one; a pack with no conversion state for the transfer makes the view without a stand-in.
static bool viewCompanions(Device &d, const Texture &of, uint32_t format, Texture &t,
                           const std::function<bool(const Texture &, uint32_t, Texture &)> &like)
{
    const auto made = [&](const Texture &over, Texture *&out) {
        out = new Texture;
        if (like(over, over.format == of.format ? format : over.format, *out))
            return true;
        delete out;
        out = nullptr;
        return false;
    };
    if (of.sampled) {
        if (!made(*of.sampled, t.sampled))
            return false;
        t.tic = t.sampled->tic;
    }
    if (!xrSampled(format) && !srgbPair(format))
        return true;
    if (of.standIn && format == of.format)
        return made(*of.standIn, t.standIn);
    Texture codes = of;
    codes.format = format;
    codes.standIn = codes.sampled = nullptr;
    if (!d.createStandIn(codes))
        return !srgbPair(format) && !d.ownPipeline(xrConversionOf(format, codes.xrPlanes));
    Texture *own = codes.standIn;
    if (!made(*own, t.standIn)) {
        d.destroyTexture(*own);
        delete own;
        return false;
    }
    t.standIn->standIn = own;
    return true;
}

bool Device::createView(const Texture &of, uint32_t format, Texture &t)
{
    if (of.zeta)
        return createZetaView(of, format, t);
    // Metal views a converted image (planar, YCbCr) in its own format, which the image's header reads with its conversion's
    // channels (ycbcrImageHeader); a two-plane extended-range one held in its planes in its own format or its sRGB twin's,
    // through a header pair as its own (the twin's sampled through a stand-in of its own: viewCompanions)
    const FormatInfo *f = formatInfo(format);
    if (!f || f->bpp != of.bpp || f->blockW != of.blockW || f->blockH != of.blockH || of.memoryless ||
        (of.converted() && format != of.format) || (of.xrPlanes && !twoPlaneXr(format)))
        return false;
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        t = of;
        t.storage = of.storageRange();
        memcpy(t.swizzle, kIdentitySwizzle, 4);
        t.view = true;
        t.format = format;
        t.renderable = f->rt != 0 && (t.blockLinear || linearTarget(t));
        t.shadowTarget = f->rt != 0 && !t.blockLinear && !linearTarget(t);
        // Native-plane lookup needs the parent import; a view never releases its non-owning reference.
        t.import = of.nativePlane != ~0u ? of.import : nullptr;
        t.span = {};
        t.lease = nullptr;
        t.chain = nullptr;  // a mipmapped texture's view: every level sampled, level 0 rendered and copied
        t.standIn = t.sampled = t.storeView = nullptr;
        t.tic = 0;
        if (!of.sampled && !(t.tic = of.xrPlanes ? allocTicPair() : allocTic()))
            return false;
        noteView(of, t);
        if (of.xrPlanes || of.converted()) {
            memcpy(tic_.cpu + 32 * t.tic, tic_.cpu + 32 * of.tic, of.xrPlanes ? 64 : 32);
            noteTicWrite();
        } else if (of.chain) {
            uint32_t w[8];
            chainViewHeader(of, of.chain->layout, of.va, *f, 0, of.chain->layout.levels, of.type, w);
            memcpy(tic_.cpu + 32 * t.tic, w, 32);
            noteTicWrite();
        } else if (!of.sampled) {
            writeTic(t);
        }
    }
    if (viewCompanions(*this, of, format, t, [&](const Texture &over, uint32_t overFormat, Texture &out) {
            return createView(over, overFormat, out);
        }))
        return true;
    destroyTexture(t);
    return false;
}

bool Device::createPartView(const Texture &parent, uint32_t format, uint32_t type, uint32_t baseLevel, uint32_t levels,
                            uint32_t baseLayer, uint32_t layers, Texture &t)
{
    // the submit worker may move the parent's memory (private_comp.h compressedFromPlain), which it writes under lock_
    Texture of;
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        of = parent;
    }
    const bool cube = type == fx::kTexCube || type == fx::kTexCubeArray, volume = type == fx::kTexThreeD;
    const bool oneLayer = type == fx::kTexOneD || type == fx::kTexTwoD || volume;
    const FormatInfo *f = of.zeta ? nullptr : formatInfo(format);
    const bool sameTexels =
        of.zeta ? format == of.format : f && f->bpp == of.bpp && f->blockW == of.blockW && f->blockH == of.blockH;
    // A multisampled image is viewed whole, as 2D or a one-layer 2D array (NIL's pitch header is TWO_D_NO_MIPMAP for
    // either); a pitch one as its slices too (a linear 2D array's, layerBytes apart); a two-plane extended-range or converted
    // one whole, as createView views it (its header pair or image header copied, the type asked written), its stand-in
    // viewed as the type asked
    const bool flat = levels == 1 && !of.chain && (type == fx::kTexTwoD || type == fx::kTexTwoDArray);
    const bool whole = flat && !baseLevel && !baseLayer && layers == 1;
    // a packed 10-bit image of several levels (Texture::levelZero) is a chain like any other, its levels viewed as such
    // (an M4 Max makes views of levels 1-2, 2 and 0 of three at those levels' sizes: the packed-levels views probe)
    const bool chained = of.converted() && of.levelZero && of.chain && type == fx::kTexTwoD;
    if (((of.converted() || of.xrPlanes) &&
         ((!whole && !chained) || (of.xrPlanes && !twoPlaneXr(format)) || (of.converted() && format != of.format))) ||
        (of.samples > 1 && !whole) || (!of.blockLinear && !flat) || !sameTexels || of.memoryless || !levels ||
        !layers || (of.type == fx::kTexThreeD) != volume || (oneLayer && layers != 1) ||
        (cube && (layers % 6 || of.width != of.height)) || (of.zeta && type == fx::kTexOneD))
        return false;
    Texture image;
    uint64_t layerBytes = 0;
    if (of.chain) {
        const fx::MipChain &c = of.chain->layout;
        if (baseLevel + levels > c.levels || baseLayer + layers > c.layers ||
            (baseLevel && !chainLevelImage(of, baseLevel, image)))
            return false;
        if (!baseLevel)
            image = of;
        if (chained && baseLevel) {  // a level's image holds its texels, not the conversion its header reads them by
            image.ycbcr = of.ycbcr;
            image.levelZero = true;
            image.luma = of.luma;
            image.chroma = of.chroma;
            image.addressMap = of.addressMap;
            memcpy(image.shape, of.shape, sizeof image.shape);
        }
        image.cpu = nullptr;
        layerBytes = c.arrayStride;
    } else {
        if (baseLevel || levels != 1 || baseLayer + layers > std::max<uint32_t>(1, of.layers) ||
            (baseLayer && !of.layerBytes))
            return false;
        image = of;
        layerBytes = of.layerBytes;
        image.cpu = of.cpu ? of.cpu + baseLayer * layerBytes : nullptr;
    }
    image.va += baseLayer * layerBytes;
    image.layers = layers;
    image.layerBytes = layerBytes;
    uint32_t zetaHeader[8];
    if (of.zeta && of.chain) {
        fx::MipChain c = of.chain->layout;
        c.layers = layers;
        fx::mipChainTic(c, of.chain->va + baseLayer * layerBytes, kZetaFormatInfo[zetaFormatIndex(of.format)].tic0,
                        false, baseLevel, levels, zetaHeader, type);
    } else if (of.zeta && type != fx::kTexTwoD && !zetaArrayHeader(image, type, zetaHeader)) {
        return false;
    }
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        t = image;
        if (parent.va != of.va) {
            t.va = t.va - of.va + parent.va;
            t.cpu = nullptr;
            t.comptags = parent.comptags;
        }
        t.storage = parent.storageRange();
        memcpy(t.swizzle, kIdentitySwizzle, 4);
        t.view = true;
        t.format = format;
        t.type = uint8_t(type);
        t.renderable = (f ? f->rt != 0 : of.renderable) && type == fx::kTexTwoD && levels == 1;
        t.shadowTarget = false;
        t.import = nullptr;
        t.span = {};
        t.lease = nullptr;
        t.chain = nullptr;
        t.standIn = t.sampled = t.storeView = nullptr;
        t.tic = 0;
        if (!of.sampled && !(t.tic = of.xrPlanes ? allocTicPair() : allocTic()))
            return false;
        noteView(parent, t);
        if (of.xrPlanes || (of.converted() && !chained)) {
            memcpy(tic_.cpu + 32 * t.tic, tic_.cpu + 32 * of.tic, of.xrPlanes ? 64 : 32);
            if (of.converted() && type != fx::kTexTwoD)  // a block-linear image's header (the planes' are pitch)
                setField(reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic), 154, 151, type);
            noteTicWrite();
        } else if (of.zeta && (of.chain || type != fx::kTexTwoD)) {
            memcpy(tic_.cpu + 32 * t.tic, zetaHeader, 32);
            noteTicWrite();
        } else if (of.sampled) {
            // sampled through a view of its texture's pool image (viewCompanions)
        } else if (!of.chain) {
            writeTic(t);
            if (of.samples > 1) {
                setField(reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic), 154, 151, type);
                noteTicWrite();
            }
        } else {
            fx::MipChain c = of.chain->layout;
            c.layers = layers;
            uint32_t w[8];
            chainViewHeader(of, c, of.chain->va + baseLayer * layerBytes, *f, baseLevel, levels, type, w);
            if (chained)  // the conversion's channels (ycbcrImageHeader)
                w[0] = reinterpret_cast<const uint32_t *>(tic_.cpu + 32 * of.tic)[0];
            memcpy(tic_.cpu + 32 * t.tic, w, 32);
            noteTicWrite();
        }
    }
    if (of.zeta ||
        viewCompanions(*this, parent, format, t, [&](const Texture &over, uint32_t overFormat, Texture &out) {
            return createPartView(over, overFormat, type, baseLevel, levels, baseLayer, layers, out);
        }))
        return true;
    destroyTexture(t);
    return false;
}

bool Device::createZetaOver(const ZetaMemory &m, uint32_t width, uint32_t height, uint32_t format, bool sampled,
                            Texture &t, const char **why, uint32_t layers)
{
    uint64_t bytes, align;
    *why = !zetaShape(width, height, format, 1, t, bytes, align) ? "a size or format the driver renders no zeta of"
           : !m.va                                               ? "its image is not bound"
           : !m.tiled                                            ? "its image is pitch-linear"
           : m.blockDepth                                        ? "its image's blocks are deeper than one plane"
           : m.bpp != t.bpp                                      ? "its texel size is not the format's"
           : m.pitch < width * t.bpp || m.pitch % 64             ? "its row stride is not whole GOBs of its texels"
           : m.bytes < m.layerBytes * layers || uint64_t(m.pitch) * height > m.bytes
               ? "its image is smaller than its rows"
           : sampled && m.pitch != (width * t.bpp + 63) / 64 * 64
               ? "its row stride is not the one a texture header implies"
               : nullptr;
    if (*why)
        return false;
    t.va = m.va;
    t.storage = {m.va, m.bytes, false};
    t.pitch = m.pitch;
    t.blockHeight = m.blockHeight;
    t.layerBytes = m.layerBytes;
    t.layers = layers;
    t.comptags = m.compressed;
    if (!sampled || layers > 1)
        return true;
    if (!(t.tic = allocTic())) {
        *why = "no texture header is left";
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::stencilTemp(Texture &t)
{
    constexpr uint32_t kRG8Unorm = 30;
    const bool fifthByte = (t.zeta && t.format == kZetaFormat) || (twoPlaneXr(t.format) && t.bpp == 8 && !t.xrPlanes);
    if (__atomic_load_n(&t.stencilTemp, __ATOMIC_ACQUIRE) || !fifthByte)
        return __atomic_load_n(&t.stencilTemp, __ATOMIC_ACQUIRE) != nullptr;
    Texture *temp = new Texture;
    if (!createTexture(t.width, t.height, kRG8Unorm, *temp)) {
        delete temp;
        return false;
    }
    __atomic_store_n(&t.stencilTemp, temp, __ATOMIC_RELEASE);
    return true;
}

bool Device::sampleRows(Texture &t)
{
    uint32_t gx = 0, gy = 0;
    sampleGrid(t.samples, gx, gy);
    if (__atomic_load_n(&t.sampleRows, __ATOMIC_ACQUIRE) || !gy)
        return true;
    Texture *rows = new Texture;
    if (!createTexture(t.width << gx, t.height << gy, t.format, *rows)) {
        delete rows;
        return false;
    }
    __atomic_store_n(&t.sampleRows, rows, __ATOMIC_RELEASE);
    return true;
}

bool Device::createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t, uint32_t samples,
                              uint32_t layers)
{
    uint64_t bytes, align;
    const bool zeta = zetaFormatIndex(format) < kZetaFormats;
    if (!layers || layers > 2048 || (layers > 1 && samples > 1) ||
        (zeta ? !zetaShape(width, height, format, samples, t, bytes, align) : !shape(width, height, format, t)) ||
        (layers > 1 && !zeta && xrForm(format) != XrForm::None))
        return false;
    t.layers = layers;
    fx::MipChain c;
    if (samples > 1 && !zeta && (xrForm(format) != XrForm::None || !fx::msLayout(width, height, samples, t.bpp, c)))
        return false;
    t.memoryless = true;
    t.samples = uint8_t(samples);
    compPrearm(t);
    return true;
}

uint32_t Device::attachmentTic(const Texture &t, const Texture &part, uint32_t level, uint32_t slice)
{
    if (level && !slice && t.chain && t.type == fx::kTexTwoD && part.format == t.format)
        return levelTic(t, level);
    // above every key viewTic keeps in views_ (word 0, a sample grid below bit 48)
    const uint64_t key = 1ull << 63 | uint64_t(part.format) << 32 | uint64_t(slice) << 4 | level;
    if (part.zeta) {
        uint32_t w[8];
        const uint32_t z = zetaFormatIndex(part.format);
        if (!t.zeta || !t.tic || part.format != t.format || z >= kZetaFormats ||
            !attachmentHeader(part, kZetaFormatInfo[z].tic0, false, w))
            return 0;
        // Depth conversion headers are prepared by concurrent recording threads, outside the encode view cache.
        std::lock_guard<gm20b::Mutex> l(lock_);
        const auto made = partTics_.equal_range(t.tic);
        for (auto m = made.first; m != made.second; ++m)
            if (m->second.first == key)
                return m->second.second;
        const uint32_t tic = allocTic();
        if (!tic)
            return 0;
        memcpy(tic_.cpu + 32 * tic, w, 32);
        noteTicWrite();
        partTics_.emplace(t.tic, std::make_pair(key, tic));
        return tic;
    }
    if (!t.tic)
        return 0;
    if (const uint32_t seen = views_.find(t.tic, key))
        return seen;
    const FormatInfo *f = formatInfo(part.format);
    uint32_t w[8];
    if (!f || !(part.samples > 1
                    ? multisampleHeader(part.width, part.height, part.samples, part.bpp, part.va, f->tic0, f->srgb, w)
                    : attachmentHeader(part, f->tic0, f->srgb, w)))
        return 0;
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    const auto made = partTics_.equal_range(t.tic);
    for (auto m = made.first; m != made.second; ++m)
        if (m->second.first == key) {
            views_.keep(t.tic, key, m->second.second);
            return m->second.second;
        }
    const uint32_t tic = allocTic();
    if (!tic)
        return 0;
    memcpy(tic_.cpu + 32 * tic, w, 32);
    noteTicWrite();
    partTics_.emplace(t.tic, std::make_pair(key, tic));
    views_.keep(t.tic, key, tic);
    return tic;
}

uint32_t Device::levelTic(const Texture &t, uint32_t level, bool codes)
{
    const FormatInfo *f = t.zeta ? nullptr : formatInfo(t.format);
    const uint32_t z = t.zeta ? zetaFormatIndex(t.format) : kZetaFormats;
    const bool zeta = t.zeta && z < kZetaFormats;
    if (!t.chain || (!f && !zeta) || (zeta && codes) || level >= t.chain->layout.levels)
        return 0;
    uint32_t &slot = (codes ? t.chain->levelCodesTic : t.chain->levelTic)[level];
    if (const uint32_t made = __atomic_load_n(&slot, __ATOMIC_ACQUIRE))
        return made;
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    uint32_t tic = slot;
    if (!tic && (tic = allocTic())) {
        uint32_t w[8];
        fx::mipChainTic(t.chain->layout, t.chain->va, f ? f->tic0 : kZetaFormatInfo[z].tic0, f && f->srgb && !codes,
                        level, 1, w, t.type == fx::kTexThreeD ? fx::kTexThreeD : fx::kTexTwoD);
        memcpy(tic_.cpu + 32 * tic, w, 32);
        noteTicWrite();
        __atomic_store_n(&slot, tic, __ATOMIC_RELEASE);
    }
    return tic;
}

uint32_t Device::viewTic(const Texture &t, uint32_t word0, ViewAccess access, bool asArray)
{
    uint8_t sxLog2 = 0, syLog2 = 0;
    if (access == ViewAccess::Storage && t.samples > 1) {
        fx::MipChain grid;
        if (!fx::msLayout(t.width, t.height, t.samples, t.bpp, grid))
            return 0;
        sxLog2 = grid.sxLog2;
        syLog2 = grid.syLog2;
    }
    const uint64_t key = word0 | uint64_t(sxLog2) << 32 | uint64_t(syLog2) << 40 | (asArray ? 1ull << 48 : 0);
    if (!t.tic)
        return 0;
    if (const uint32_t seen = views_.find(t.tic, key))
        return seen;
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    const auto views = viewTics_.equal_range(t.tic);
    for (auto v = views.first; v != views.second; ++v)
        if (v->second.first == key) {
            views_.keep(t.tic, key, v->second.second);
            return v->second.second;
        }
    const uint32_t tic = allocTic();
    if (!tic)
        return 0;
    uint32_t own[8], w[8];
    memcpy(own, headerNow(t.tic), 32);
    viewHeader(own, word0, w, sxLog2, syLog2, asArray);
    memcpy(tic_.cpu + 32 * tic, w, 32);
    noteTicWrite();
    viewTics_.emplace(t.tic, std::make_pair(key, tic));
    views_.keep(t.tic, key, tic);
    return tic;
}

uint32_t Device::arrayViewTic(const Texture &t)
{
    constexpr uint64_t kArrayViewKey = 1ull << 49;
    if (!t.tic)
        return 0;
    if (const uint32_t seen = views_.find(t.tic, kArrayViewKey))
        return seen;
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    const auto views = viewTics_.equal_range(t.tic);
    for (auto v = views.first; v != views.second; ++v)
        if (v->second.first == kArrayViewKey) {
            views_.keep(t.tic, kArrayViewKey, v->second.second);
            return v->second.second;
        }
    const uint32_t tic = allocTic();
    if (!tic)
        return 0;
    uint32_t own[8], w[8];
    memcpy(own, headerNow(t.tic), 32);
    arrayViewHeader(own, w);
    memcpy(tic_.cpu + 32 * tic, w, 32);
    noteTicWrite();
    viewTics_.emplace(t.tic, std::make_pair(kArrayViewKey, tic));
    views_.keep(t.tic, kArrayViewKey, tic);
    return tic;
}

uint32_t Device::attachmentViewTic(const Texture &owner, const AttachmentView &v, const char **why)
{
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    constexpr const char *kNoHeader = "framebuffer fetch from a target the driver cannot sample";
    if (!owner.tic) {
        *why = kNoHeader;
        return 0;
    }
    const auto views = attachmentViewTics_.equal_range(owner.tic);
    for (auto it = views.first; it != views.second; ++it)
        if (it->second.first == v)
            return it->second.second;
    uint32_t w[8];
    bool headed = false;
    if (v.part) {
        fx::MipChain c;
        uint64_t base = 0;
        headed = attachmentImage(owner, c, base) && attachmentViewHeader(c, base, owner.type == fx::kTexThreeD, v, w);
    } else {
        uint32_t own[8];
        memcpy(own, headerNow(owner.tic), 32);
        headed = ownViewHeader(own, v, w);
    }
    if (!headed) {
        *why = kNoHeader;
        return 0;
    }
    const uint32_t tic = allocTic();
    if (!tic) {
        *why = "no room for the framebuffer storage view";
        return 0;
    }
    memcpy(tic_.cpu + 32 * tic, w, 32);
    noteTicWrite();
    attachmentViewTics_.emplace(owner.tic, std::make_pair(v, tic));
    return tic;
}

static bool chainLevelView(const Texture &t, uint32_t level, uint32_t rtFormat, Texture &out)
{
    ColorTargetDesc d;
    if (!t.chain || !chainLevelDesc(t.chain->layout, t.chain->va, level, rtFormat, d))
        return false;
    out = Texture();
    out.storage = t.storageRange();
    out.va = d.va;
    out.comptags = t.comptags;  // a level is rendered and filled with the chain's compression
    out.width = t.chain->layout.level[level].width;
    out.height = d.height;
    out.format = t.format;
    out.pitch = d.pitch;
    out.bpp = t.bpp;
    out.blockLinear = true;
    out.blockHeight = d.blockHeight;
    out.layerBytes = t.chain->layout.layers > 1 ? t.chain->layout.arrayStride : d.layerBytes;
    out.layers = t.chain->layout.layers;
    if (t.type == fx::kTexThreeD) {  // the level's planes in its own blocks' depth (the copy engine's 3D addressing)
        out.type = fx::kTexThreeD;
        out.depth = t.chain->layout.level[level].depth;
        out.blockDepth = t.chain->layout.level[level].zLog2;
    }
    out.blockW = t.blockW;
    out.blockH = t.blockH;
    out.zeta = t.zeta;  // a depth chain's level is zeta memory: its copies move the aspect Metal names
    out.stencilTemp = t.stencilTemp;
    out.texelWidth = std::max(1u, t.texelWidth >> level);
    out.texelHeight = std::max(1u, t.texelHeight >> level);
    return true;
}

bool chainLevelTarget(const Texture &t, uint32_t level, Texture &out)
{
    const FormatInfo *f = formatInfo(t.format);
    if (!f || !f->rt || !chainLevelView(t, level, f->rt, out))
        return false;
    out.renderable = true;
    return true;
}

bool chainLevelImage(const Texture &t, uint32_t level, Texture &out)
{
    return chainLevelView(t, level, ~0u, out);
}

}  // namespace gm20b
