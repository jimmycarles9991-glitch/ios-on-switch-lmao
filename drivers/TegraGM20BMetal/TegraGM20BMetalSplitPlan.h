// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "raster_area.h"

// The pipeline a draw of `state` takes
inline gm20b::Pipeline *nativeOf(const gm20b::RasterState &state)
{
    __unsafe_unretained SwitchMetalPipeline *pipeline = (__bridge SwitchMetalPipeline *)state.pipeline;
    auto *variant = static_cast<gm20b::Variant<gm20b::Pipeline> *>(state.nativeVariant);
    gm20b::Pipeline *drawn = variant                ? variant->value()
                             : state.nativePipeline ? static_cast<gm20b::Pipeline *>(state.nativePipeline)
                             : pipeline             ? pipeline->_raw
                                                    : nullptr;
    return drawn;
}

// The colors the pass's draws bind as storage images, and of those as 2D arrays (gm20b::PassDesc::storageMask,
// arrayedStorageMask)
__attribute__((objc_externally_retained)) inline void passStorageMasks(SwitchMetalCommandBuffer *cb, size_t begin,
                                                                       gm20b::PassDesc &p)
{
    cb->_plan.forEachRasterGroup(begin, [&](const gm20b::RasterState &state, uint32_t) {
        if (const gm20b::Pipeline *native = nativeOf(state)) {
            p.storageMask |= native->storageBits;
            if (gm20b::layeredRecord(native->rec->arrayLengthWord))
                p.arrayedStorageMask |= native->storageBits;
        }
    });
}

__attribute__((objc_externally_retained)) inline void planSplitPrograms(SwitchMetalCommandBuffer *cb, size_t begin,
                                                                        gm20b::RenderEncoder &encoder)
{
    SWITCH_METAL_ENCODE_SPAN(SplitPlan);
    encoder.beginSplitPlan();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb->_forceWholePrograms)
        return;
#endif
    cb->_plan.forEachRasterGroup(begin, [&](const gm20b::RasterState &state, uint32_t draws) {
        encoder.addSplitCandidates(nativeOf(state), draws,
                                   gm20b::rasterPixelLimit(state, encoder.areaWidth(), encoder.areaHeight()));
    });
    encoder.finishSplitPlan();
}
