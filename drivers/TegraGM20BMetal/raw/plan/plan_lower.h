// SPDX-License-Identifier: MIT
// Lowering as data: the emitter calls a planned graph turns into, in order, each naming the kit function
// (kits/SwitchGM20BEmit) or, where the kit has none yet, the raw runtime code that emits it today. The wiring agent maps
// each op onto its call; nothing here writes a word.
#pragma once
#include "plan_build.h"
#include <vector>

namespace raw::plan {

enum class Emitter : uint8_t {
    TiledCacheFlush,
    WaitForIdle,
    InvalidateShaderCaches,
    InvalidateShaderCachesNoWfi,
    InvalidateTextureData,
    InvalidateTextureHeaders,
    InvalidateSamplers,
    PixelShaderBarrier,
    ColorTarget,
    RenderArea,
    Clear,
    CopyRect,
    DrawMacro,
    ZetaTarget,
    ProgramState,
    Bindings,
    TwoDBlit,
    CopyFill,
    CopyRemap,
    Dispatch,
    ComputeBarrier,
    SetReference,
    SemaphoreAcquire,
    SemaphoreRelease,
    Present,
    Count
};

struct EmitterInfo {
    Emitter emitter;
    bool inKit;
    const char *call;  // the kit function, or the raw code that emits it until the kit has it
};

const EmitterInfo &emitterInfo(Emitter e);

// arg: a Barriers mask for the barrier emitters (the shader-cache parts of it map onto SwitchGM20BEmitBarrier.h's
// switch_gm20b_shader_caches), an attachment index, a draw index, CopyNonPipelined for an ordered copy, or kNone
struct EmitOp {
    Emitter emitter;
    uint32_t node;
    uint32_t arg;
    uint16_t level = 0;  // a copy's level: one copy rectangle a level, its layers in one call
};

std::vector<EmitOp> lower(const WorkGraph &g, const Recording &r, const JobContext &job);

// The barrier emitters one Barriers mask turns into, in kit order: tiled flush, shader caches or WFI, texture data,
// pixel barrier, headers, samplers
void lowerBarriers(std::vector<EmitOp> &out, Barriers b, uint32_t node, Engine engine);

}  // namespace raw::plan
