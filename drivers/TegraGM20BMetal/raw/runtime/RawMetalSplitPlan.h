// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_raster_area.h"

// The pipeline a draw of `state` takes: in a pass that holds extended-range shadows, its shadowed variant (RawMetalXrShadow.h)
inline raw::Pipeline *nativeOf(const raw::RasterState &state, const RMXrShadowTwins *shadows = nullptr)
{
    __unsafe_unretained RMPipeline *pipeline = (__bridge RMPipeline *)state.pipeline;
    auto *variant = static_cast<raw::Variant<raw::Pipeline> *>(state.nativeVariant);
    raw::Pipeline *drawn = variant                ? variant->value()
                           : state.nativePipeline ? static_cast<raw::Pipeline *>(state.nativePipeline)
                           : pipeline             ? pipeline->_raw
                                                  : nullptr;
    return rmXrShadowed(shadows, state.pipeline, variant, drawn);
}

inline const RMXrShadowTwins *passShadows(RMCommandBuffer *cb, size_t begin)
{
    return rmXrShadowTwinsOf(cb, cb->_plan.begin(begin).pass);
}

// The colors the pass's draws bind as storage images, and of those as 2D arrays (raw::PassDesc::storageMask,
// arrayedStorageMask)
__attribute__((objc_externally_retained)) inline void passStorageMasks(RMCommandBuffer *cb, size_t begin,
                                                                       raw::PassDesc &p)
{
    const RMXrShadowTwins *shadows = passShadows(cb, begin);
    cb->_plan.forEachRasterGroup(begin, [&](const raw::RasterState &state, uint32_t) {
        if (const raw::Pipeline *native = nativeOf(state, shadows)) {
            p.storageMask |= native->storageBits;
            if (raw::layeredRecord(native->rec->arrayLengthWord))
                p.arrayedStorageMask |= native->storageBits;
        }
    });
}

__attribute__((objc_externally_retained)) inline void planSplitPrograms(RMCommandBuffer *cb, size_t begin,
                                                                        raw::RenderEncoder &encoder)
{
    RAW_ENCODE_SPAN(SplitPlan);
    encoder.beginSplitPlan();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb->_forceWholePrograms)
        return;
#endif
    const RMXrShadowTwins *shadows = passShadows(cb, begin);
    cb->_plan.forEachRasterGroup(begin, [&](const raw::RasterState &state, uint32_t draws) {
        encoder.addSplitCandidates(nativeOf(state, shadows), draws,
                                   raw::rasterPixelLimit(state, encoder.areaWidth(), encoder.areaHeight()));
    });
    encoder.finishSplitPlan();
}
