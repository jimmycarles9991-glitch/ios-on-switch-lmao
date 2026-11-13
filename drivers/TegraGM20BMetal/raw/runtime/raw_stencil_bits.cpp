// SPDX-License-Identifier: MIT
// See raw_stencil_bits.h.
#include "raw_stencil_bits.h"

#include <array>
#include <cstring>

#include "raw_gpu.h"

namespace raw {

Pipeline *Device::stencilBitsPipeline(uint32_t format, uint32_t samples, StencilSample pick)
{
    const char *name = stencilBitsState(format, samples, pick);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

bool CommandBuffer::bindStencilSource(RenderEncoder &e, const Texture &src, Texture &view, uint32_t format, bool depth,
                                      const char **why)
{
    const uint32_t word0 = stencilViewWord0(format);
    if (!word0) {
        *why = "no stencil header for the attachment's format";
        return false;
    }
    if (!src.tic) {
        *why = "a stencil source without a texture header";
        return false;
    }
    view = src;
    view.tic = word0 == kZetaFormatInfo[zetaFormatIndex(format)].tic0 ? src.tic : dev_->viewTic(src, word0);
    if (!view.tic) {
        *why = "no texture header for a stencil source's stencil";
        return false;
    }
    if (depth) {
        e.setTexture(0, &src);  // its depth, read as its float's bits
        e.setTexture(1, &view);
    } else {
        e.setTexture(0, &view);
    }
    return true;
}

bool CommandBuffer::stencilBits(RenderEncoder &e, const Texture &src, Texture &view, uint32_t format,
                                const TurnMap &map, StencilSample pick, const char **why, bool zeroFirst)
{
    static const std::array<DepthStencilState, kStencilBits> states = [] {
        std::array<DepthStencilState, kStencilBits> s;
        for (uint32_t k = 0; k < kStencilBits; k++)
            s[k] = DepthStencilState(stencilBitState(k));
        return s;
    }();
    static const DepthStencilState zero = [] {
        DepthStencilDesc d = stencilBitState(0);
        d.front.writeMask = d.back.writeMask = 0xff;
        return DepthStencilState(d);
    }();
    Pipeline *pl = src.format == format ? dev_->stencilBitsPipeline(format, src.samples, pick) : nullptr;
    if (!pl) {
        *why = "no stencil bits state for the attachment's format";
        return false;
    }
    if (!bindStencilSource(e, src, view, format,
                           pick == StencilSample::LeastDepth || pick == StencilSample::GreatestDepth, why))
        return false;
    e.setPipeline(pl);
    e.setStencilReference(kStencilBitReference, kStencilBitReference);
    if (zeroFirst) {
        StencilBitsArgs args = stencilBitsArgs(map, src.samples, pick);
        args.bit = 0;
        e.setStencilReference(0, 0);
        e.setDepthStencil(&zero);
        e.setBytes(2, 0, &args, sizeof args);
        if (!e.draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
            return false;
        RAW_DIAGNOSTIC(dev_->stats.stencilBitDraws++);
        e.setStencilReference(kStencilBitReference, kStencilBitReference);
    }
    return forEachStencilBit(stencilBitsArgs(map, src.samples, pick), [&](uint32_t k, const StencilBitsArgs &args) {
        e.setDepthStencil(&states[k]);
        e.setBytes(2, 0, &args, sizeof args);
        RAW_DIAGNOSTIC(dev_->stats.stencilBitDraws++);
        return e.draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why);
    });
}

bool CommandBuffer::stencilResolve(Texture &into, const Texture &from, StencilSample pick, uint32_t width,
                                   uint32_t height, const char **why)
{
    if (from.samples > 1 && zetaColourTakes(into, from, pick))
        return zetaColour(into, from, pick, TurnMap{}, width, height, why);
    PassDesc p;
    p.depthStencil.texture = &into;
    p.depthStencil.clearStencil = true;
    p.width = width;
    p.height = height;
    RenderEncoder *e = from.samples > 1 ? render(p) : nullptr;
    if (!e) {
        *why = "a stencil resolve texture the raw path cannot render";
        return false;
    }
    Texture view;
    if (!stencilBits(*e, from, view, into.format, TurnMap{}, pick, why))
        return false;
    e->end();
    return true;
}

}  // namespace raw
