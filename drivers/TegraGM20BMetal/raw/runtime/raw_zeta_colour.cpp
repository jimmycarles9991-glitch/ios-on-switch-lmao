// SPDX-License-Identifier: MIT
// See raw_zeta_colour.h.
#include "raw_zeta_colour.h"

#include <cstring>

#include "raw_chip_facts.h"
#include "raw_gpu.h"

namespace raw {

bool Device::zetaColourTaken() const
{
#if SWITCH_RAW_DIAGNOSTICS
    if (probeZetaColour.load(std::memory_order_relaxed))
        return true;
#endif
    return kColourRopWritesZeta;
}

Pipeline *Device::stencilColourPipeline(uint32_t format, uint32_t samples, StencilSample pick)
{
    const char *name = stencilColourState(format, samples, pick);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

bool CommandBuffer::zetaColourTakes(const Texture &into, const Texture &src, StencilSample pick)
{
    return into.zeta && src.format == into.format && zetaColourWrites(into, dev_->zetaColourTaken()) &&
           dev_->stencilColourPipeline(into.format, src.samples, pick);
}

bool CommandBuffer::zetaColour(const Texture &into, const Texture &src, StencilSample pick, const TurnMap &map,
                               uint32_t width, uint32_t height, const char **why)
{
    Pipeline *pl = dev_->stencilColourPipeline(into.format, src.samples, pick);
    Texture target = zetaColourView(into);
    RenderEncoder *e = pl ? render(zetaColourPass<PassDesc>(target, width, height)) : nullptr;
    if (!e) {
        *why = "a stencil plane the raw path cannot render as colour";
        return false;
    }
    const bool depth = src.samples == 1 ? zetaHasDepth(into.format)
                                        : pick == StencilSample::LeastDepth || pick == StencilSample::GreatestDepth;
    Texture view;
    if (!bindStencilSource(*e, src, view, into.format, depth, why))
        return false;
    e->setPipeline(pl);
    const StencilBitsArgs args = stencilBitsArgs(map, src.samples, pick);
    e->setBytes(2, 0, &args, sizeof args);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return false;
    RAW_DIAGNOSTIC(dev_->stats.zetaColourDraws++);
    e->end();
    return true;
}

}  // namespace raw
