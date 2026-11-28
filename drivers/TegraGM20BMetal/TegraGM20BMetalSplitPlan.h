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
                                                                       gm20b::PassDesc &p, bool own = false)
{
    const void *lastPipe = nullptr, *lastNative = nullptr, *lastVar = nullptr;
    gm20b::Pipeline *lastDrawn = nullptr;
    bool haveLast = false;
    cb->_plan.forEachRasterGroup(
        begin,
        [&](const gm20b::RasterState &state, uint32_t) {
            gm20b::Pipeline *native = lastDrawn;
            if (!haveLast || state.pipeline != lastPipe || state.nativePipeline != lastNative ||
                state.nativeVariant != lastVar) {
                lastPipe = state.pipeline;
                lastNative = state.nativePipeline;
                lastVar = state.nativeVariant;
                native = lastDrawn = nativeOf(state);
                haveLast = true;
            }
            if (native) {
                p.storageMask |= native->storageBits;
                if (native->fetchBits)
                    p.mayFetch = true;
                if (native->storageBits && gm20b::layeredRecord(native->rec->arrayLengthWord))
                    p.arrayedStorageMask |= native->storageBits;
            } else {
                p.mayFetch = true;
            }
        },
        own);
}

__attribute__((objc_externally_retained)) inline void planSplitPrograms(SwitchMetalCommandBuffer *cb, size_t begin,
                                                                        gm20b::RenderEncoder &encoder, bool own = false)
{
    SWITCH_METAL_ENCODE_SPAN(SplitPlan);
    encoder.beginSplitPlan();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb->_forceWholePrograms)
        return;
#endif
    const void *lastPipe = nullptr, *lastNative = nullptr, *lastVar = nullptr;
    gm20b::Pipeline *lastDrawn = nullptr;
    const gm20b::SplitSource *lastSource = nullptr;
    bool haveLast = false;
    bool added = false;
    cb->_plan.forEachRasterGroup(
        begin,
        [&](const gm20b::RasterState &state, uint32_t draws) {
            if (!haveLast || state.pipeline != lastPipe || state.nativePipeline != lastNative ||
                state.nativeVariant != lastVar) {
                lastPipe = state.pipeline;
                lastNative = state.nativePipeline;
                lastVar = state.nativeVariant;
                lastDrawn = nativeOf(state);
                lastSource = lastDrawn ? __atomic_load_n(&lastDrawn->splitSource, __ATOMIC_ACQUIRE) : nullptr;
                haveLast = true;
            }
            if (lastSource) {
                encoder.addSplitCandidates(lastDrawn, draws,
                                           gm20b::rasterPixelLimit(state, encoder.areaWidth(), encoder.areaHeight()));
                added = true;
            }
        },
        own);
    if (added)
        encoder.finishSplitPlan();
}
