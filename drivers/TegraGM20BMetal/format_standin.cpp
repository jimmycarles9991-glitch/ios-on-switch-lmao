// SPDX-License-Identifier: MIT
// See format_standin.h.
#include "format_standin.h"

#include "attachment_target.h"
#include "gpu.h"
#include "xr_pools.h"

namespace gm20b {
namespace {

// `t`'s words as texels of integer format `format`, read through a header of their own (not its sampled stand-in, which a
// bind takes in their place); false where none can be made
bool wordsView(Device &d, Texture &t, Texture &view, uint32_t format)
{
    if (!d.sampleable(t))
        return false;
    view = t;
    view.format = format;
    view.sampled = nullptr;
    view.standIn = nullptr;
    view.tic = d.viewTic(t, formatInfo(format)->tic0);
    return view.tic != 0;
}

// What an extended-range store reads beside its stand-in (format_standin.h formatStoreKeeps): the codes it stores into,
// at texture 1 (a surface's alpha plane at texture 2), and whether it keeps the texels no draw changed, at buffer 0; its
// targets loaded where it keeps them, a pool image's copied in
struct StoreReads {
    Texture *codes = nullptr, *alphas = nullptr;
    uint32_t keep = 0;
};

// One pass of `state` over all of `target` (and `secondTarget` at colour 1), reading `source` at texture 0 (and
// `secondSource` at texture 1)
bool drawAll(CommandBuffer &rc, Pipeline *state, Texture &target, Texture &source, const char **why,
             Texture *secondTarget = nullptr, Texture *secondSource = nullptr, const StoreReads *reads = nullptr)
{
    PassDesc p;
    p.color[0].texture = &target;
    p.color[1].texture = secondTarget;
    for (ColorAttachment &a : p.color) {
        a.load = reads && reads->keep ? ColorAttachment::Load : ColorAttachment::DontCare;
        a.store = true;
    }
    p.count = secondTarget ? 2 : 1;
    RenderEncoder *e = rc.render(p, why);
    if (!e)
        return false;
    e->setPipeline(state);
    e->setTexture(0, &source);
    if (secondSource)
        e->setTexture(1, secondSource);
    if (reads) {
        e->setTexture(1, reads->codes);
        if (reads->alphas)
            e->setTexture(2, reads->alphas);
        e->setBytes(2, 0, &reads->keep, sizeof reads->keep);
    }
    e->setViewport(0, 0, target.width, target.height, 0, 1);
    e->setScissor(0, 0, target.width, target.height);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return false;
    e->end();
    return true;
}

// Layer `layer` of pass image `t` (a whole texture, or a level's or slice's part of one, gm20b::attachmentTarget) as a texture
// of its own with no header: a 3D level's planes lie a plane apart (one plane a block), any other layers layerBytes apart
Texture layerOf(const Texture &t, uint32_t layer)
{
    Texture one = t;
    one.storage = t.storageRange();
    const uint32_t blockRows = 8u << t.blockHeight;
    one.va +=
        layer * (t.type == fx::kTexThreeD ? uint64_t(t.pitch) * ((t.height + blockRows - 1) / blockRows * blockRows)
                                          : t.layerBytes);
    one.type = fx::kTexTwoD;
    one.layers = one.depth = 1;
    one.blockDepth = 0;
    one.tic = 0;
    one.chain = nullptr;
    one.sampled = one.standIn = nullptr;
    return one;
}

// A multisampled image as the copy engine moves it: one sample a texel over its sample grid (fx::msLayout)
bool sampleGridOf(const Texture &t, Texture &grid)
{
    grid = t;
    if (t.samples <= 1)
        return true;
    fx::MipChain c;
    if (!fx::msLayout(t.width, t.height, t.samples, t.bpp, c))
        return false;
    grid.width = t.width << c.sxLog2;
    grid.height = t.height << c.syLog2;
    grid.samples = 1;
    return true;
}

// The copy engine's moves of `layers` layers between a texture of a copied format (formatStandInCopied) and its stand-in
bool copyLayers(CommandBuffer &rc, const Texture &t, const Texture &standIn, uint32_t layers, bool toStandIn)
{
    BlitEncoder *b = rc.blit();
    bool laid = true;
    for (uint32_t l = 0; l < layers && laid; l++) {
        Texture texels, held;
        laid = sampleGridOf(layerOf(t, l), texels) && sampleGridOf(layerOf(standIn, l), held);
        if (!laid)
            break;
        const Texture &from = toStandIn ? texels : held, &to = toStandIn ? held : texels;
        if (t.format == kR8UnormSrgb)
            b->copyFirstByte(from, to);
        else if (t.format == kRg8UnormSrgb)
            b->copyFirstTwoBytes(from, to);
        else
            b->copyArgb8(from, to, toStandIn);
    }
    b->end();
    return laid;
}

// The header layer `layer` of the image the pass renders `a` of reads in one.format: made on first use and freed with the
// header of the texture it is a part of
uint32_t layerTic(Device &d, const ColorAttachment &a, const Texture &one, uint32_t layer)
{
    return a.image ? d.attachmentTic(*a.image, one, a.level, a.slice + layer)
                   : d.attachmentTic(*a.texture, one, 0, layer);
}

// A one-layer image's fill: from both planes of one held in a surface's two planes, else through its pool image where its
// rows are 16 but not 32 bytes apart (Texture::sampled), else its own header or its words'
bool fillOne(Device &d, CommandBuffer &rc, Pipeline *load, Texture &t, Texture &s, const char **why)
{
    if (t.xrPlanes) {
        if (!d.sampleable(*t.luma) || !d.sampleable(*t.chroma)) {
            *why = "no texture header for a stand-in's fill";
            return false;
        }
        fillXrPools(rc, t);
        return drawAll(rc, load, s, *t.luma, why, nullptr, t.chroma);
    }
    if (t.sampled) {
        BlitEncoder *b = rc.blit();
        b->copyTexels(t, 0, 0, 0, *t.sampled, 0, 0, 0, t.width, t.height);
        b->end();
    }
    Texture words;
    const bool loadsWords = formatLoadsWords(t.format);
    if (loadsWords ? !wordsView(d, t, words, formatStoreView(t.format)) : !d.sampleable(t)) {
        *why = "no texture header for a stand-in's fill";
        return false;
    }
    return drawAll(rc, load, s, loadsWords ? words : t, why);
}

}  // namespace

Texture *formatStandInFor(Device &d, CommandBuffer &rc, const ColorAttachment &a, uint32_t layers, uint32_t slot,
                          bool fill, const char **why)
{
    Texture &t = *a.texture;
    const uint32_t format = formatStandIn(t.format);
    Pipeline *load = d.ownPipeline(t.xrPlanes ? xrTargetState(t.format, XrStep::Load, 1, true)
                                              : formatLoadState(t.format, t.samples));
    Pipeline *store = d.ownPipeline(t.xrPlanes ? xrTargetState(t.format, XrStep::Store, 1, true)
                                               : formatStoreState(t.format, t.samples));
    if (!format || (!formatStandInCopied(t.format) && !t.memoryless && (!load || !store))) {
        *why = "a target of a format the GM20B renders none of, with no stand-in route (the pack has no load or store "
               "state for it at its sample count)";
        return nullptr;
    }
    layers = std::max(layers, 1u);
    if (layers > std::max(t.layers, 1u)) {
        *why = "a layered pass of more layers than its attachment holds from its slice";
        return nullptr;
    }
    Texture *s = rc.turnScratch(format, t.width, t.height, kFormatStandInTag + slot, t.samples, layers);
    if (!s) {
        *why = "no stand-in for a target of a format the GM20B renders none of";
        return nullptr;
    }
    if (!fill || t.memoryless)
        return s;
    if (formatStandInCopied(t.format)) {
        if (copyLayers(rc, t, *s, layers, true))
            return s;
        *why = "a multisampled target of a copied format with no sample grid";
        return nullptr;
    }
    if (layers == 1)
        return fillOne(d, rc, load, t, *s, why) ? s : nullptr;
    const uint32_t read = formatLoadsWords(t.format) ? formatStoreView(t.format) : t.format;
    for (uint32_t l = 0; l < layers; l++) {
        Texture from = layerOf(t, l), to = layerOf(*s, l);
        from.format = read;
        if (!(from.tic = layerTic(d, a, from, l))) {
            *why = "no texture header for a stand-in's fill";
            return nullptr;
        }
        if (!drawAll(rc, load, to, from, why))
            return nullptr;
    }
    return s;
}

bool storeFormatStandIn(Device &d, CommandBuffer &rc, const ColorAttachment &a, uint32_t layers, Texture &standIn,
                        bool cleared, const char **why, bool keep)
{
    Texture &t = *a.texture;
    layers = std::max(layers, 1u);
    if (formatStandInCopied(t.format)) {
        if (copyLayers(rc, t, standIn, layers, false))
            return true;
        *why = "a multisampled target of a copied format with no sample grid";
        return false;
    }
    if (!d.sampleable(standIn)) {
        *why = "no texture header for a stand-in's store";
        return false;
    }
    if (t.xrPlanes) {  // planes that overlap through their pools, then into their rows (xr_pools.h)
        const bool pools = xrPlanesOverlap(t);
        Texture &words = pools ? *t.luma->sampled : *t.luma;
        Texture *alpha = pools && t.chroma->sampled ? t.chroma->sampled : t.chroma;
        if (!d.sampleable(words) || !d.sampleable(*alpha)) {
            *why = "no texture header for a stand-in's store";
            return false;
        }
        const StoreReads reads = {&words, alpha, keep};
        return drawAll(rc, d.ownPipeline(xrTargetState(t.format, XrStep::Store, 1, true)), words, standIn, why, alpha,
                       nullptr, &reads) &&
               (!pools || !storeXrPlaneRows(d, rc, t, cleared, why));
    }
    Pipeline *store = d.ownPipeline(formatStoreState(t.format, t.samples));
    const bool reads = formatStoreKeeps(t.format);
    for (uint32_t l = 0; l < layers; l++) {
        // the texture's words as the store state writes them: rendered as they lie where NVK's linear-render rule takes
        // the rows, else through a pool image the pass copies back (Texture::shadowTarget)
        Texture words = layers == 1 ? t : layerOf(t, l);
        // a turned texture's stand-in is laid out as its plane: stored into it straight, not turned again
        words.rotation = 0;
        words.format = formatStoreView(t.format);
        words.tic = 0;
        words.sampled = nullptr;
        words.renderable = t.blockLinear || !((t.va | t.pitch) & 127);
        words.shadowTarget = !words.renderable;
        Texture held = layers == 1 ? standIn : layerOf(standIn, l);
        if (layers > 1 && !(held.tic = d.attachmentTic(standIn, held, 0, l))) {
            *why = "no texture header for a stand-in's store";
            return false;
        }
        // the codes the store keeps, read through a header of their own over the texture's rows
        Texture codes = layers == 1 ? t : layerOf(t, l);
        if (reads) {
            codes.format = formatStoreView(t.format);
            if (layers == 1 ? !wordsView(d, t, codes, codes.format) : !(codes.tic = layerTic(d, a, codes, l))) {
                *why = "no texture header for a stand-in's store";
                return false;
            }
        }
        const StoreReads kept = {&codes, nullptr, keep};
        if (!drawAll(rc, store, words, held, why, nullptr, nullptr, reads ? &kept : nullptr))
            return false;
    }
    return true;
}

bool formatMipPasses(Device &d, CommandBuffer &rc, Texture &t, uint32_t first, uint32_t last, uint32_t firstLayer,
                     uint32_t layers, uint32_t format, const char **why)
{
    Pipeline *mip = d.ownPipeline(formatMipState(format));
    if (!mip || !t.chain || t.type == fx::kTexThreeD || last >= t.chain->layout.levels) {
        *why = "a chain of a format the GM20B renders none of with no mip state, or a 3D one";
        return false;
    }
    const fx::MipChain &c = t.chain->layout;
    const uint32_t view = formatMipView(format);
    for (uint32_t layer = firstLayer; layer < firstLayer + layers; layer++)
        for (uint32_t l = first; l < last; l++) {
            Texture from, to, above;
            if (!attachmentTarget(t, l, layer, from) || !attachmentTarget(t, l + 1, layer, to)) {
                *why = "a mip step's level of a format the GM20B renders none of that cannot be read or drawn";
                return false;
            }
            from.format = format;
            if (!(from.tic = d.attachmentTic(t, from, l, layer)) || !wordsView(d, from, above, view)) {
                *why = "a mip step's level of a format the GM20B renders none of that cannot be read or drawn";
                return false;
            }
            to.format = view;
            to.renderable = true;
            PassDesc p;
            p.color[0].texture = &to;
            p.color[0].load = ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            RenderEncoder *e = rc.render(p, why);
            if (!e)
                return false;
            const FormatMipStep step = {{c.level[l].width, c.level[l].height},
                                        {c.level[l + 1].width, c.level[l + 1].height}};
            e->setPipeline(mip);
            e->setTexture(0, &above);
            e->setBytes(2, 0, &step, sizeof step);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return false;
            e->end();
        }
    return true;
}

}  // namespace gm20b
