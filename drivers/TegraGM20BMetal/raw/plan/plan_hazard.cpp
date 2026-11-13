// SPDX-License-Identifier: MIT
#include "plan_hazard.h"

namespace raw::plan {

namespace {

constexpr ClauseInfo kClauses[] = {
    {Clause::Execution, "the earlier node completes",
     "nvk_cmd_buffer.c:521-523 (any source stage: NVK_BARRIER_WFI); :678-687 (WFI on the last subchannel, the switch "
     "drains the others)",
     "RenderEncoder::end's pass prologue (switch_gm20b_pass_barrier); CommandBuffer::blit; the ComputeEncoder "
     "constructor"},
    {Clause::FlushStorage, "storage writes leave the SMs' L1",
     "nvk_cmd_buffer.c:525-526 (SHADER_STORAGE_WRITE: FLUSH_SHADER_DATA); :663-675 (INVALIDATE_SHADER_CACHES "
     ".flush_data, itself a WFI)",
     "RenderEncoder::end after storage writes; RenderEncoder::prepare's storage barrier"},
    {Clause::TiledCache, "binned colour writes leave the tiled cache",
     "nvk_tiled_cache.h:65-72, nvk_cmd_buffer.c:431, nvk_event.c:184 (this fork's 7c0b96132, not upstream: at a "
     "release "
     "and at a command buffer's end)",
     "RenderEncoder::end at every pass that drew (raw_tiled_cache.h tiledCacheDrain)"},
    {Clause::TextureRead, "a texture-unit read of written memory",
     "nvk_cmd_buffer.c:565-567 (INPUT_ATTACHMENT/SHADER_SAMPLED_READ: INVALIDATE_TEX_DATA); :571-580 (blit and "
     "resolve reads); :818-829 (INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI)",
     "RenderEncoder::end's pass prologue"},
    {Clause::StorageRead, "a storage read of written memory",
     "nvk_cmd_buffer.c:569-570 (SHADER_STORAGE_READ: INVALIDATE_SHADER_DATA); :851-863 "
     "(INVALIDATE_SHADER_CACHES_NO_WFI "
     ".global_data)",
     "RenderEncoder::end's pass prologue"},
    {Clause::ConstantRead, "a GPU-written buffer read as constants",
     "nvk_cmd_buffer.c:560-563 (UNIFORM_READ: SHADER_DATA and CONSTANT); :851-863",
     "RenderEncoder::end's pass prologue drops constants at every pass"},
    {Clause::IndirectRead, "GPU-written arguments read by the push fetcher",
     "nvk_cmd_buffer.c:550-558 (INDIRECT_COMMAND_READ: MME data, constants); :880-891 (SET_REFERENCE)",
     "RenderEncoder::indirect at every indirect draw"},
    {Clause::PixelOrdered, "a fetch of a colour an earlier draw of the pass wrote",
     "nvk_cmd_buffer.c:922-1032 (this fork's 42fed4abc: PIXEL_SHADER_BARRIER and the pixel-ordered texture invalidate "
     "0x0f74 for a colour write to a fragment read)",
     "RenderEncoder::prepare and fetchBarrier"},
    {Clause::InPassStorage, "a storage access meets colour writes in the pass",
     "nvk_cmd_buffer.c:521-526 and :569-570 (WFI, flush, shader data; NVK drops no texture lines for a storage read: "
     "raw's texture invalidate is kept until probed)",
     "RenderEncoder::prepare: tiled drain, flush and data, texture data (raw_attachment_order.h)"},
    {Clause::InterlockOrder, "a storage access after one in the interlock",
     "none: the interlock's ticket orders them (NVK exposes fragment shader interlock, nvk_physical_device.c:733-734)",
     "RenderEncoder::prepare"},
    {Clause::CopyOrder, "a copy after a copy of the same memory",
     "nvk_cmd_buffer.c:700-712 (the copy engine's wait: LAUNCH_DMA NON_PIPELINED, FLUSH_ENABLE)",
     "kits/SwitchGM20BEmit/SwitchGM20BEmitCopy.h:48 data_transfer_type"},
    {Clause::WriteAfterRead, "a write after a read",
     "nvk_cmd_buffer.c:541-588 (invalidates follow the destination's reads only): execution alone",
     "RenderEncoder::end's pass prologue"},
    {Clause::CrossPassPixel, "a colour write and a fragment read in different passes, pixel-ordered (probe-gated)",
     "nvk_cmd_buffer.c:1013-1032 (this fork: the pixel-ordered dependency also across vkCmdBeginRendering)", "none"},
    {Clause::FetchOnly, "vertex fetch, a copy-engine read or the display after a write",
     "nvk_cmd_buffer.c:541-588: VERTEX_ATTRIBUTE_READ and INDEX_READ invalidate nothing; TRANSFER_READ at COPY "
     "invalidates texture and shader data (:576-580) because NVK's copies can be shaders, the copy engine reads L2",
     "CommandBuffer::blit's WFI"},
};
static_assert(sizeof kClauses / sizeof *kClauses == size_t(Clause::Count), "a row for each clause");

bool colour(Path p)
{
    return p == Path::Rop || p == Path::Zrop;
}

bool fragmentSide(const Access &a)
{
    return a.engine == Engine::ThreeD && a.path != Path::Storage &&
           (colour(a.path) || (a.path == Path::Texture && a.stage == Stage::Fragment));
}

}  // namespace

const ClauseInfo &clauseInfo(Clause c)
{
    return kClauses[size_t(c)];
}

bool conflicts(const Access &earlier, const Access &later, Scope scope)
{
    if (!earlier.write && !later.write)
        return false;
    return !(scope == Scope::InPass && earlier.path == Path::Storage && later.path == Path::Storage);
}

bool onChannel(Engine e)
{
    return e == Engine::ThreeD || e == Engine::TwoD || e == Engine::Copy || e == Engine::Compute || e == Engine::Inline;
}

bool switchDrains(Engine from, Engine to)
{
    return from != to && onChannel(from) && onChannel(to);
}

Requirement hazard(const Access &earlier, const Access &later, Scope scope, bool tiledCache)
{
    Requirement r;
    const auto add = [&](Clause c, Barriers b) {
        r.barriers |= b;
        r.clauses |= 1u << uint32_t(c);
    };
    if (!conflicts(earlier, later, scope)) {
        if (scope == Scope::InPass && earlier.path == Path::Storage && later.path == Path::Storage)
            r.clauses |= 1u << uint32_t(Clause::InterlockOrder);
        return r;
    }
    const Barriers tiled = tiledCache && colour(earlier.path) && earlier.write ? TiledCacheFlush : 0;
    if (scope == Scope::InPass) {
        if (earlier.path == Path::Storage || later.path == Path::Storage)
            add(Clause::InPassStorage,
                (tiledCache ? TiledCacheFlush : 0) | FlushShaderData | InvalidateShaderData | InvalidateTextureData);
        else if (earlier.write && colour(earlier.path) && later.path == Path::Texture)
            add(Clause::PixelOrdered, PixelShaderBarrier);
        return r;
    }
    if (later.path == Path::Display) {
        add(Clause::FetchOnly, 0);
        if (tiled)
            add(Clause::TiledCache, tiled);
        if (earlier.path == Path::Storage)
            add(Clause::FlushStorage, FlushShaderData);
        return r;
    }
    if (earlier.engine == Engine::Copy && later.engine == Engine::Copy) {
        add(Clause::CopyOrder, CopyNonPipelined);
        return r;
    }
    if (tiled)
        add(Clause::TiledCache, tiled);
    if (earlier.path == Path::Storage && earlier.write)
        add(Clause::FlushStorage, FlushShaderData);
    else
        add(earlier.write ? Clause::Execution : Clause::WriteAfterRead, WaitForIdle);
    if (earlier.write && !later.write) {
        switch (later.path) {
        case Path::Texture:
            add(Clause::TextureRead, InvalidateTextureData);
            break;
        case Path::Storage:
            add(Clause::StorageRead, InvalidateShaderData);
            break;
        case Path::Constant:
            add(Clause::ConstantRead, InvalidateConstants | InvalidateShaderData);
            break;
        case Path::Indirect:
            add(Clause::IndirectRead, SetReference | InvalidateConstants);
            break;
        default:
            add(Clause::FetchOnly, 0);
            break;
        }
    }
    if (fragmentSide(earlier) && fragmentSide(later) && earlier.path != Path::Zrop && later.path != Path::Zrop) {
        r.relaxed = (r.barriers & ~(WaitForIdle | InvalidateTextureData)) | PixelShaderBarrier;
        r.clauses |= 1u << uint32_t(Clause::CrossPassPixel);
    }
    return r;
}

Barriers required(const Requirement &r, const Caps &caps)
{
    return r.relaxed && caps.selectable(Rule::PassPixelBarrier) ? r.relaxed : r.barriers;
}

}  // namespace raw::plan
