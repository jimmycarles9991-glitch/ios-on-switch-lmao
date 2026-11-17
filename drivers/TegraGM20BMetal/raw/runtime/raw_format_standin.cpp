// SPDX-License-Identifier: MIT
// See raw_format_standin.h.
#include "raw_format_standin.h"

#include "raw_attachment_target.h"
#include "raw_gpu.h"

namespace raw {
namespace {

Pipeline *packState(Device &d, const char *name)
{
    return name ? d.pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

// `t`'s words as texels of integer format `format`, read through a header of their own; false where none can be made
bool wordsView(Device &d, Texture &t, Texture &view, uint32_t format = 53 /* R32Uint */)
{
    if (!d.sampleable(t))
        return false;
    view = t;
    view.format = format;
    view.sampled = nullptr;
    view.tic = d.viewTic(t, formatInfo(format)->tic0);
    return view.tic != 0;
}

// One pass of `state` over all of `target`, reading `source` at texture 0
bool drawAll(CommandBuffer &rc, Pipeline *state, Texture &target, Texture &source, const char **why)
{
    PassDesc p;
    p.color[0].texture = &target;
    p.color[0].load = ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    RenderEncoder *e = rc.render(p, why);
    if (!e)
        return false;
    e->setPipeline(state);
    e->setTexture(0, &source);
    e->setViewport(0, 0, target.width, target.height, 0, 1);
    e->setScissor(0, 0, target.width, target.height);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return false;
    e->end();
    return true;
}

}  // namespace

Texture *formatStandInFor(Device &d, CommandBuffer &rc, Texture &t, uint32_t slot, bool fill, const char **why)
{
    const uint32_t format = formatStandIn(t.format);
    if (!format || (t.format != kR8UnormSrgb &&
                    (!packState(d, formatLoadState(t.format)) || !packState(d, formatStoreState(t.format))))) {
        *why = "a target of a format the GM20B renders none of, with no stand-in route (the pack has no load or store "
               "state for it)";
        return nullptr;
    }
    Texture *s = rc.turnScratch(format, t.width, t.height, kFormatStandInTag + slot);
    if (!s) {
        *why = "no stand-in for a target of a format the GM20B renders none of";
        return nullptr;
    }
    if (!fill)
        return s;
    if (t.format == kR8UnormSrgb) {
        BlitEncoder *b = rc.blit();
        b->copyFirstByte(t, *s);
        b->end();
        return s;
    }
    // a texture of rows 16 but not 32 bytes apart is read through its pool image (Texture::sampled)
    if (t.sampled) {
        BlitEncoder *b = rc.blit();
        b->copyTexels(t, 0, 0, 0, *t.sampled, 0, 0, 0, t.width, t.height);
        b->end();
    }
    Texture words;
    if (t.format == kRgb9e5Float ? !wordsView(d, t, words) : !d.sampleable(t)) {
        *why = "no texture header for a stand-in's fill";
        return nullptr;
    }
    return drawAll(rc, packState(d, formatLoadState(t.format)), *s, t.format == kRgb9e5Float ? words : t, why)
               ? s
               : nullptr;
}

bool storeFormatStandIn(Device &d, CommandBuffer &rc, Texture &t, Texture &standIn, const char **why)
{
    if (t.format == kR8UnormSrgb) {
        BlitEncoder *b = rc.blit();
        b->copyFirstByte(standIn, t);
        b->end();
        return true;
    }
    // the texture's words as the store state writes them: rendered as they lie where NVK's linear-render rule takes the
    // rows, else through a pool image the pass copies back (Texture::shadowTarget)
    Texture words = t;
    words.format = formatStoreView(t.format);
    words.tic = 0;
    words.sampled = nullptr;
    words.renderable = t.blockLinear || !((t.va | t.pitch) & 127);
    words.shadowTarget = !words.renderable;
    if (!d.sampleable(standIn)) {
        *why = "no texture header for a stand-in's store";
        return false;
    }
    return drawAll(rc, packState(d, formatStoreState(t.format)), words, standIn, why);
}

bool formatMipPasses(Device &d, CommandBuffer &rc, Texture &t, uint32_t first, uint32_t last, uint32_t firstLayer,
                     uint32_t layers, uint32_t format, const char **why)
{
    const char *state = formatMipState(format);
    Pipeline *mip = state ? d.pipeline(Pack::fnv(state, strlen(state))) : nullptr;
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

}  // namespace raw
