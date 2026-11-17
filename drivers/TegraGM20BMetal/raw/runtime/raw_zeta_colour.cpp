// SPDX-License-Identifier: MIT
// See raw_zeta_colour.h.
#include "raw_zeta_colour.h"

#include <cstring>

#include "raw_gpu.h"
#include "raw_tiled_cache.h"

namespace raw {

Pipeline *Device::stencilColourPipeline(uint32_t format, uint32_t samples, StencilSample pick)
{
    const char *name = stencilColourState(format, samples, pick);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

bool CommandBuffer::zetaColourTakes(const Texture &into, const Texture &src, StencilSample pick)
{
#if SWITCH_RAW_DIAGNOSTICS
    if (dev_->probeStencilBits.load(std::memory_order_relaxed))
        return false;
#endif
    return into.zeta && src.format == into.format && zetaColourWrites(into) &&
           dev_->stencilColourPipeline(into.format, src.samples, pick);
}

bool CommandBuffer::zetaColour(const Texture &into, const Texture &src, StencilSample pick, const TurnMap &map,
                               uint32_t width, uint32_t height, const char **why)
{
    Pipeline *pl = dev_->stencilColourPipeline(into.format, src.samples, pick);
    Texture target = zetaColourView(into);
#if SWITCH_RAW_DIAGNOSTICS
    // hw_test_app gpu.raw.zeta_colour's direct arm: the colour ROP over the zeta's own pages with the tiled cache off for
    // the draw, the configuration nvc0 draws into kind 0xc3 in
    const bool direct = dev_->probeZetaDirect.load(std::memory_order_relaxed);
    const bool untiled = direct && tiledCache(dev_->k_.info());
    if (direct)
        target.zetaPages = false;
#endif
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
#if SWITCH_RAW_DIAGNOSTICS
    if (untiled) {
        if (e->p_.room() < 8)
            e->reserve();
        e->p_.m(SET_TILED_CACHE_ENABLE, 0);
    }
#endif
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return false;
#if SWITCH_RAW_DIAGNOSTICS
    if (untiled) {
        if (e->p_.room() < 8)
            e->reserve();
        e->p_.m(SET_TILED_CACHE_ENABLE, 1);
    }
#endif
    RAW_DIAGNOSTIC(dev_->stats.zetaColourDraws++);
    e->end();
    return true;
}

}  // namespace raw
