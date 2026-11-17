// SPDX-License-Identifier: MIT
// See raw_turn_copy.h.
#include "raw_turn_copy.h"

#include <algorithm>
#include <cstring>

#include "raw_gpu.h"

namespace raw {

bool CommandBuffer::turnCopy(const Texture &dst, Texture &src, const TurnMap &map, const char **why,
                             const TurnMask *mask, uint32_t width, uint32_t height)
{
    if (!dev_->sampleable(src)) {
        *why = "no texture header for a turn copy's source";
        return false;
    }
    return turnCopy(dst, static_cast<const Texture &>(src), map, why, mask, width, height);
}

bool CommandBuffer::turnCopy(const Texture &dst, const Texture &src, const TurnMap &map, const char **why,
                             const TurnMask *mask, uint32_t width, uint32_t height)
{
    TurnMap bounded = map;
    bounded.x0 = std::max(bounded.x0, 0);
    bounded.y0 = std::max(bounded.y0, 0);
    bounded.x1 = std::min(bounded.x1, int32_t(src.width));
    bounded.y1 = std::min(bounded.y1, int32_t(src.height));
    if (dst.zeta && zetaHasStencil(dst.format) && !mask && src.samples == 1 &&
        zetaColourTakes(dst, src, StencilSample::Zero)) {
        return zetaColour(dst, src, StencilSample::Zero, bounded, 0, 0, why);
    }
    static const DepthStencilState depthWrites = [] {
        DepthStencilDesc desc;
        desc.depthCompare = 7;  // MTLCompareFunctionAlways
        desc.depthWrite = true;
        return DepthStencilState(desc);
    }();
    const bool copies = !dst.zeta || zetaHasDepth(dst.format), stencil = dst.zeta && zetaHasStencil(dst.format);
    const uint32_t format = dst.zeta ? dst.format : turnCopyFormat(dst.bpp);
    const char *state = !copies    ? nullptr
                        : dst.zeta ? (mask ? nullptr : turnDepthState(dst.format))
                        : mask     ? turnMaskedState(dst.bpp)
                                   : turnCopyState(dst.bpp);
    const FormatInfo *f = dst.zeta ? nullptr : formatInfo(format);
    Pipeline *pl = state ? dev_->pipeline(Pack::fnv(state, strlen(state))) : nullptr;
    if ((copies && !pl) || (!dst.zeta && !f) || src.bpp != dst.bpp || src.zeta != dst.zeta ||
        (dst.zeta && src.format != dst.format)) {
        *why = "no turn copy state for the attachment's format";
        return false;
    }
    Texture target = dst, source = src, stencilView;
    target.format = source.format = format;
    target.rotation = 0;
    if (!(source.tic = dst.zeta || src.format == format ? src.tic : dev_->viewTic(src, f->tic0))) {
        *why = "no texture header for a turn copy's source";
        return false;
    }
    PassDesc p;
    p.width = width;
    p.height = height;
    if (dst.zeta) {
        p.depthStencil.texture = &target;
    } else {
        p.color[0].texture = &target;
        p.color[0].load = ColorAttachment::DontCare;
        p.count = 1;
    }
    RenderEncoder *e = render(p);
    if (!e) {
        *why = "a turn copy's target the raw path cannot render";
        return false;
    }
    if (copies) {
        e->setPipeline(pl);
        if (dst.zeta)
            e->setDepthStencil(&depthWrites);
        e->setTexture(0, &source);
        if (mask) {
            const MaskedTurnMap masked{bounded, mask->tiles};
            e->setTexture(1, mask->mask);
            e->setBytes(2, 0, &masked, sizeof masked);
        } else {
            e->setBytes(2, 0, &bounded, sizeof bounded);
        }
        if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return false;
    }
    if (stencil && !stencilBits(*e, source, stencilView, dst.format, bounded, StencilSample::Zero, why, true))
        return false;
    e->end();
    return true;
}

bool CommandBuffer::depthConvert(const Texture &dst, const Texture &src, const char **why)
{
    static const DepthStencilState depthWrites = [] {
        DepthStencilDesc desc;
        desc.depthCompare = 7;  // MTLCompareFunctionAlways
        desc.depthWrite = true;
        return DepthStencilState(desc);
    }();
    const char *state = turnDepthState(dst.format, dst.samples);
    Pipeline *pl = state && dst.zeta && src.zeta && zetaHasDepth(src.format) && src.width == dst.width &&
                           src.height == dst.height && src.samples == dst.samples
                       ? dev_->pipeline(Pack::fnv(state, strlen(state)))
                       : nullptr;
    if (!pl) {
        *why = "no depth conversion state for the formats";
        return false;
    }
    if (!src.tic) {
        *why = "no texture header for a depth conversion's source";
        return false;
    }
    Texture target = dst;
    target.rotation = 0;
    PassDesc p;
    p.depthStencil.texture = &target;
    RenderEncoder *e = render(p);
    if (!e) {
        *why = "a depth conversion's target the raw path cannot render";
        return false;
    }
    const TurnMap identity;
    e->setPipeline(pl);
    e->setDepthStencil(&depthWrites);
    e->setTexture(0, &src);
    e->setBytes(2, 0, &identity, sizeof identity);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return false;
    e->end();
    return true;
}

bool Device::sampleable(Texture &t)
{
    std::lock_guard<raw::Mutex> l(lock_);
    if (t.tic)
        return true;
    if (!(t.tic = allocTic()))
        return false;
    writeTic(t);
    return true;
}

Texture *CommandBuffer::turnScratch(uint32_t format, uint32_t width, uint32_t height, uint32_t tag, uint32_t samples)
{
    Texture m;
    uint64_t bytes = 0, align = 0;
    const bool zeta = zetaFormatIndex(format) < kZetaFormats;
    if (zeta ? !zetaShape(width, height, format, samples, m, bytes, align)
             : samples != 1 || !shape(width, height, format, m))
        return nullptr;
    m.memoryless = true;
    std::lock_guard<raw::Mutex> l(dev_->lock_);
    Texture *b = dev_->backing(m, tag, true, false);
    if (b)
        backings_.push_back(b);
    return b;
}

}  // namespace raw
