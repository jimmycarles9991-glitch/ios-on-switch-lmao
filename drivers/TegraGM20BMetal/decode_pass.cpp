// SPDX-License-Identifier: MIT
// See decode_pass.h.
#include "decode_pass.h"

#include "attachment_target.h"
#include "gpu.h"

namespace gm20b {

namespace {

// The part of `image` a pass draws level `level`, layer or plane `layer` of: a one-layer pitch image as it is (through
// its block-linear shadow where GM20B renders none of its rows), else its block-linear part
bool decodeTarget(const Texture &image, uint32_t level, uint32_t layer, Texture &to)
{
    if (!image.blockLinear && image.layers <= 1 && !level && !layer) {
        to = image;
        to.tic = 0;
        to.chain = nullptr;
        to.sampled = to.standIn = nullptr;
        pitchTargetForm(to);
        return true;
    }
    return attachmentTarget(image, level, layer, to);
}

}  // namespace

bool decodePass(Device &d, CommandBuffer &rc, const Texture &image, const DecodedFormat &f, uint32_t level,
                uint32_t layer, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const DecodeSource &source,
                const char **why)
{
    Pipeline *state = d.ownPipeline(decodeStateName(f.state));
    Texture to;
    if (!state || !source.buffer || !decodeTarget(image, level, layer, to) || x >= to.width || y >= to.height) {
        *why = "a decoded format's upload with no decode state in the pack, or into no part of its image a pass draws";
        return false;
    }
    to.format = decodeTargetFormat(f.state);
    PassDesc p;
    p.color[0].texture = &to;
    p.color[0].load = ColorAttachment::Load;
    p.color[0].store = true;
    p.count = 1;
    RenderEncoder *e = rc.render(p, why);
    if (!e)
        return false;
    // the buffer is bound at a 256-byte boundary, the blocks' place past it carried in the step
    const uint64_t bound = source.offset & ~uint64_t(255);
    const DecodeStep step = decodeStep(f, x, y, to.width, to.height, source.rowBytes, uint32_t(source.offset - bound));
    e->setPipeline(state);
    e->setBytes(2, 0, &step, sizeof step);
    e->setBuffer(2, 1, source.buffer, bound);
    e->setViewport(0, 0, to.width, to.height, 0, 1);
    e->setScissor(x, y, std::min(width, to.width - x), std::min(height, to.height - y));
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return false;
    e->end();
    return true;
}

}  // namespace gm20b
