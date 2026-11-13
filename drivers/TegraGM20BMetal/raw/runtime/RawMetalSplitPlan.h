// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_raster_area.h"

inline raw::Pipeline *nativeOf(const raw::RasterState &state)
{
    __unsafe_unretained RMPipeline *pipeline = (__bridge RMPipeline *)state.pipeline;
    auto *variant = static_cast<raw::Variant<raw::Pipeline> *>(state.nativeVariant);
    return variant                ? variant->value()
           : state.nativePipeline ? static_cast<raw::Pipeline *>(state.nativePipeline)
           : pipeline             ? pipeline->_raw
                                  : nullptr;
}

// The colors the pass's draws bind as storage images, and of those as 2D arrays (raw::PassDesc::storageMask,
// arrayedStorageMask)
__attribute__((objc_externally_retained)) inline void passStorageMasks(RMCommandBuffer *cb, size_t begin,
                                                                       raw::PassDesc &p)
{
    cb->_plan.forEachRasterGroup(begin, [&](const raw::RasterState &state, uint32_t) {
        if (const raw::Pipeline *native = nativeOf(state)) {
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
    if (cb->_forceWholePrograms || rmWholePrograms())
        return;
#endif
    cb->_plan.forEachRasterGroup(begin, [&](const raw::RasterState &state, uint32_t draws) {
        encoder.addSplitCandidates(nativeOf(state), draws,
                                   raw::rasterPixelLimit(state, encoder.areaWidth(), encoder.areaHeight()));
    });
    encoder.finishSplitPlan();
}
