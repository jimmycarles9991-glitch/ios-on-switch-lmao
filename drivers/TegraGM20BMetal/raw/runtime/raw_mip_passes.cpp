// SPDX-License-Identifier: MIT
// generateMipmapsForTexture's steps on the 3D engine (raw_mip.h's pack states): one pass a layer and level, level l + 1 of
// each layer drawn from a header of level l of that layer alone (an sRGB chain's: its codes), the two levels' sizes its
// step. Each pass's prologue barrier orders its reads after the pass before wrote.
#include "raw_attachment_target.h"
#include "raw_gpu.h"

namespace raw {

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

}  // namespace raw
