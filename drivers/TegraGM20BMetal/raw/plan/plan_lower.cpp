// SPDX-License-Identifier: MIT
#include "plan_lower.h"

namespace raw::plan {

namespace {

constexpr EmitterInfo kEmitters[] = {
    {Emitter::TiledCacheFlush, true, "switch_gm20b_tiled_cache_flush"},
    {Emitter::WaitForIdle, true, "switch_gm20b_wait_for_idle"},
    {Emitter::InvalidateShaderCaches, true, "switch_gm20b_invalidate_shader_caches"},
    {Emitter::InvalidateShaderCachesNoWfi, true, "switch_gm20b_invalidate_shader_caches_no_wfi"},
    {Emitter::InvalidateTextureData, true, "switch_gm20b_invalidate_texture_data"},
    {Emitter::InvalidateTextureHeaders, true, "switch_gm20b_invalidate_texture_headers"},
    {Emitter::InvalidateSamplers, true, "switch_gm20b_invalidate_samplers"},
    {Emitter::PixelShaderBarrier, true, "switch_gm20b_pixel_shader_barrier"},
    {Emitter::ColorTarget, true, "switch_gm20b_color_target"},
    {Emitter::RenderArea, true, "switch_gm20b_render_area"},
    {Emitter::Clear, true, "switch_gm20b_clear"},
    {Emitter::CopyRect, true, "switch_gm20b_copy_rect"},
    {Emitter::DrawMacro, true, "CALL_MME_MACRO(SWITCH_GM20B_MACRO_*) from gen/SwitchGM20BMacros.h"},
    {Emitter::ZetaTarget, false, "raw_gpu.cpp zetaTargetWords, zetaClearValues"},
    {Emitter::ProgramState, false, "raw_emit.cpp record state words, raw_gpu.cpp RenderEncoder::prepare"},
    {Emitter::Bindings, false, "raw_descriptors.h set-0 block, raw_dynamic_push.h root words"},
    {Emitter::TwoDBlit, false, "raw/fx/fx_mip.cpp (2D engine blit, own texture invalidate)"},
    {Emitter::CopyFill, false, "raw_gpu.cpp BlitEncoder::fillBytes (copy engine constant remap)"},
    {Emitter::CopyRemap, false, "raw_gpu.cpp BlitEncoder::convertPlanar (copy engine remaps)"},
    {Emitter::Dispatch, false, "raw_compute.cpp ComputeEncoder::dispatchRegion"},
    {Emitter::ComputeBarrier, false, "raw_compute.cpp compute-class INVALIDATE_SHADER_CACHES and texture invalidate"},
    {Emitter::SetReference, false, "host SET_REFERENCE (RenderEncoder::indirect)"},
    {Emitter::SemaphoreAcquire, false, "raw_shared_events.cpp, the job's waits (TegraGM20BABI SubmitAddress)"},
    {Emitter::SemaphoreRelease, false, "raw_shared_events.cpp, the job's signals"},
    {Emitter::Present, false, "the kernel's present job (gpu_present.c)"},
};
static_assert(sizeof kEmitters / sizeof *kEmitters == size_t(Emitter::Count), "an entry for each emitter");

void op(std::vector<EmitOp> &out, Emitter e, uint32_t node, uint32_t arg = kNone)
{
    out.push_back({e, node, arg});
}

// switch_gm20b_copy_rect copies one level's layers: a copy over several levels is one call a level
void copyLevels(std::vector<EmitOp> &out, const Recording &r, const Node &n, uint32_t index, uint32_t ordered)
{
    const ResourceFacts &to = r.resources[n.to];
    const uint32_t levels = to.kind != ResourceFacts::Texture ? 1
                            : n.toRange.levels == UINT16_MAX  ? uint32_t(to.levels) - n.toRange.level
                                                              : n.toRange.levels;
    for (uint32_t l = 0; l < levels; ++l)
        out.push_back({Emitter::CopyRect, index, l ? kNone : ordered, uint16_t(n.toRange.level + l)});
}

void ownPassBody(std::vector<EmitOp> &out, const Node &n, uint32_t index)
{
    op(out, Emitter::ColorTarget, index, 0);
    op(out, Emitter::RenderArea, index);
    for (uint32_t i = 0; i < n.runs.count; ++i) {
        op(out, Emitter::ProgramState, index, n.runs.first + i);
        op(out, Emitter::Bindings, index, n.runs.first + i);
        op(out, Emitter::DrawMacro, index, kNone);
    }
}

void pass(std::vector<EmitOp> &out, const WorkGraph &g, const Recording &r, const Node &n, uint32_t index)
{
    if (!n.continues && n.pass != kNone) {
        const PassDesc &p = r.passes[n.pass];
        for (uint32_t j = 0; j < 8; ++j)
            if (p.color[j].texture != kNone)
                op(out, Emitter::ColorTarget, index, j);
        if (p.depth.texture != kNone || p.stencil.texture != kNone)
            op(out, Emitter::ZetaTarget, index);
        op(out, Emitter::RenderArea, index);
        for (uint32_t j = 0; j < 8; ++j)
            if (p.color[j].texture != kNone && p.color[j].load == Load::Clear)
                op(out, Emitter::Clear, index, j);
    }
    for (uint32_t i = 0; i < n.runs.count; ++i) {
        const DrawRun &run = g.runs[n.runs.first + i];
        lowerBarriers(out, run.before, index, Engine::ThreeD);
        op(out, Emitter::ProgramState, index, n.runs.first + i);
        op(out, Emitter::Bindings, index, n.runs.first + i);
        for (uint32_t d = 0; d < run.draws.count; ++d)
            op(out, Emitter::DrawMacro, index, run.draws.first + d);
    }
}

}  // namespace

const EmitterInfo &emitterInfo(Emitter e)
{
    return kEmitters[size_t(e)];
}

void lowerBarriers(std::vector<EmitOp> &out, Barriers b, uint32_t node, Engine engine)
{
    if (!b)
        return;
    if (b & TiledCacheFlush)
        op(out, Emitter::TiledCacheFlush, node);
    const Barriers parts = b & kShaderCaches;
    const bool compute = engine == Engine::Compute;
    if ((b & FlushShaderData) || (parts && (b & WaitForIdle)))
        op(out, compute ? Emitter::ComputeBarrier : Emitter::InvalidateShaderCaches, node, parts);
    else if (parts)
        op(out, compute ? Emitter::ComputeBarrier : Emitter::InvalidateShaderCachesNoWfi, node, parts);
    else if (b & WaitForIdle)
        op(out, Emitter::WaitForIdle, node);
    if (b & InvalidateTextureData)
        op(out, compute ? Emitter::ComputeBarrier : Emitter::InvalidateTextureData, node, InvalidateTextureData);
    if (b & PixelShaderBarrier)
        op(out, Emitter::PixelShaderBarrier, node);
    if (b & InvalidateTextureHeaders)
        op(out, Emitter::InvalidateTextureHeaders, node);
    if (b & InvalidateSamplers)
        op(out, Emitter::InvalidateSamplers, node);
    if (b & SetReference)
        op(out, Emitter::SetReference, node);
}

std::vector<EmitOp> lower(const WorkGraph &g, const Recording &r, const JobContext &job)
{
    std::vector<EmitOp> out;
    lowerBarriers(out, g.entry | (job.heapPlaced ? InvalidateInstructions : 0), kNone, Engine::ThreeD);
    for (uint32_t i = 0; i < g.nodes.size(); ++i) {
        const Node &n = g.nodes[i];
        if (n.kind == NodeKind::Wait) {
            op(out, Emitter::SemaphoreAcquire, i, n.source);
            lowerBarriers(out, n.before, i, Engine::ThreeD);
            continue;
        }
        lowerBarriers(out, n.before & Barriers(~CopyNonPipelined), i, n.engine);
        const uint32_t ordered = (n.before & CopyNonPipelined) ? uint32_t(CopyNonPipelined) : kNone;
        switch (n.kind) {
        case NodeKind::RenderPass:
            pass(out, g, r, n, i);
            break;
        case NodeKind::Clear:
            if (n.engine == Engine::TwoD) {
                op(out, Emitter::TwoDBlit, i);
            } else {
                pass(out, g, r, n, i);
            }
            break;
        case NodeKind::Copy:
            if (n.engine == Engine::TwoD)
                op(out, Emitter::TwoDBlit, i);
            else
                copyLevels(out, r, n, i, ordered);
            break;
        case NodeKind::Fill:
            op(out, Emitter::CopyFill, i, ordered);
            break;
        case NodeKind::Blit:
            op(out, Emitter::TwoDBlit, i);
            break;
        case NodeKind::Resolve:
        case NodeKind::MipChain:
        case NodeKind::Convert:
            if (n.engine == Engine::ThreeD)
                ownPassBody(out, n, i);
            else if (n.engine == Engine::TwoD)
                op(out, Emitter::TwoDBlit, i);
            else
                op(out, n.rule == Rule::PlanarCodes ? Emitter::CopyRemap : Emitter::CopyRect, i, ordered);
            break;
        case NodeKind::Dispatch:
            op(out, Emitter::Dispatch, i, n.source);
            break;
        case NodeKind::Present:
            op(out, Emitter::Present, i, n.source);
            break;
        case NodeKind::Wait:
        case NodeKind::Signal:
            op(out, Emitter::SemaphoreRelease, i, n.source);
            break;
        case NodeKind::Count:
            break;
        }
    }
    lowerBarriers(out, g.exit, kNone, Engine::ThreeD);
    return out;
}

}  // namespace raw::plan
