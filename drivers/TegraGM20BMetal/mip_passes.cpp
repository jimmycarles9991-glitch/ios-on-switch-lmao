// SPDX-License-Identifier: MIT
// generateMipmapsForTexture's steps on the 3D engine (mip.h's pack states): one pass a layer and level, level l + 1 of
// each layer drawn from a header of level l of that layer alone (an sRGB chain's: its codes), the two levels' sizes its
// step. Each pass's prologue barrier orders its reads after the pass before wrote.
#include "attachment_target.h"
#include "gpu.h"
#include "mip.h"

namespace gm20b {

namespace {
// Level `level` of chain `t` (a 2D chain's layer `layer` alone, a 3D chain's volume) read as words of integer format `view`
bool wordLevel(Device &d, Texture &t, uint32_t level, uint32_t layer, uint32_t format, uint32_t view, Texture &out)
{
    const FormatInfo *f = formatInfo(format), *v = formatInfo(view);
    if (!f || !v)
        return false;
    if (t.type == fx::kTexThreeD) {
        out = t;
        out.tic = d.levelTic(t, level, f->srgb);
    } else {
        if (!attachmentTarget(t, level, layer, out))
            return false;
        out.format = format;
        out.tic = d.attachmentTic(t, out, level, layer);
    }
    if (!out.tic)
        return false;
    out.format = view;
    out.sampled = nullptr;
    out.standIn = nullptr;
    out.tic = d.viewTic(out, v->tic0);
    return out.tic != 0;
}
}  // namespace

bool wordMipPasses(Device &d, CommandBuffer &rc, Texture &t, uint32_t first, uint32_t last, uint32_t firstLayer,
                   uint32_t layers, uint32_t format, const char **why)
{
    const bool volume = t.type == fx::kTexThreeD;
    Pipeline *mip = d.ownPipeline(volume ? wordMip3dState(format) : wordMipState(format));
    if (!mip || !t.chain || last >= t.chain->layout.levels) {
        *why = "a chain of words the pack has no mip state for, or levels past it";
        return false;
    }
    const fx::MipChain &c = t.chain->layout;
    const uint32_t view = wordMipView(format);
    for (uint32_t layer = firstLayer; layer < firstLayer + (volume ? 1 : layers); layer++)
        for (uint32_t l = first; l < last; l++) {
            Texture above;
            if (!wordLevel(d, t, l, layer, format, view, above)) {
                *why = "no header for a mip step's level of words";
                return false;
            }
            const fx::MipLevel &from = c.level[l], &into = c.level[l + 1];
            for (uint32_t z = 0; z < (volume ? into.depth : 1u); z++) {
                Texture to;
                if (!attachmentTarget(t, l + 1, volume ? z : layer, to)) {
                    *why = "a mip step's level of words the 3D engine cannot draw";
                    return false;
                }
                to.format = view;
                to.renderable = true;
                to.layers = 1;
                PassDesc p;
                p.color[0].texture = &to;
                p.color[0].load = ColorAttachment::DontCare;
                p.color[0].store = true;
                p.count = 1;
                p.width = into.width;
                p.height = into.height;
                RenderEncoder *e = rc.render(p, why);
                if (!e)
                    return false;
                e->setPipeline(mip);
                e->setTexture(0, &above);
                const uint32_t step[8] = {from.width, from.height, into.width, into.height,
                                          from.depth, into.depth,  z,          0};
                e->setBytes(2, 0, step, volume ? sizeof step : 4 * sizeof *step);
                if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                    return false;
                e->end();
            }
        }
    return true;
}

bool CommandBuffer::mipPasses(Texture &t, Pipeline &mip, uint32_t first, uint32_t last, uint32_t firstLayer,
                              uint32_t layers, uint32_t format, const char **why)
{
    const fx::MipChain &c = t.chain->layout;
    if (t.type == fx::kTexThreeD || last >= c.levels) {
        *why = "a 3D texture's levels or levels past its chain on the 3D engine";
        return false;
    }
    const FormatInfo *f = formatInfo(format);
    const bool codes = f && f->srgb;
    const uint32_t codesFormat = format == 81 ? 80 : format == 71 ? 70 : format;
    for (uint32_t layer = firstLayer; layer < firstLayer + layers; layer++)
        for (uint32_t l = first; l < last; l++) {
            Texture from = t, to;
            if (c.layers == 1 && format == t.format) {
                from.tic = dev_->levelTic(t, l, codes);
                if (!chainLevelTarget(t, l + 1, to)) {
                    *why = "a mip step's level the 3D engine cannot draw";
                    return false;
                }
                to.tic = dev_->levelTic(t, l + 1);
            } else {
                Texture part;
                if (!attachmentTarget(t, l, layer, part) || !attachmentTarget(t, l + 1, layer, to)) {
                    *why = "a mip step's level the 3D engine cannot read or draw";
                    return false;
                }
                part.format = codesFormat;
                from.tic = dev_->attachmentTic(t, part, l, layer);
                to.layers = 1;
                to.format = format;
                to.tic = dev_->attachmentTic(t, to, l + 1, layer);
            }
            from.format = format;
            to.format = format;
            to.renderable = f->rt != 0;
            if (!from.tic) {
                *why = "no header for a mip step's level";
                return false;
            }
            PassDesc p;
            p.color[0].texture = &to;
            p.color[0].load = ColorAttachment::DontCare;
            p.color[0].store = true;
            p.count = 1;
            p.width = c.level[l + 1].width;
            p.height = c.level[l + 1].height;
            RenderEncoder *e = render(p);
            if (!e) {
                *why = "no pass into a mip step's level";
                return false;
            }
            e->setPipeline(&mip);
            e->setTexture(0, &from);
            const uint32_t step[4] = {c.level[l].width, c.level[l].height, p.width, p.height};
            e->setBytes(2, 0, step, sizeof step);
            if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
                return false;
            e->end();
        }
    return true;
}

}  // namespace gm20b
