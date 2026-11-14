// SPDX-License-Identifier: MIT
// The barrier each encoder of a command buffer opens with, by the engine of the work encoded before it: today's drain
// (switch_gm20b_pass_barrier, WAIT_FOR_IDLE) or a relaxed form where a HW Test probe proved the chip orders the change
// without it. Production takes each rule's form (Rule); hw_test_app's barrier probes force the others (raw_probe_forms.h).
//  - rule barrier.switch_drain: a change of subchannel waits for the engine left behind (nvk_cmd_buffer.c:678-687), so
//    no WAIT_FOR_IDLE goes in at 3D to copy or 2D, copy or 2D to 3D, and 3D to compute; the tiled flush that ends a pass
//    and the NO_WFI invalidates stay. gpu.x1.subchannel_switch_drain held in all 13 cells on boots 8, 14, 15, 19 and 21
//    (hazard arm differing in every cell, 30,646 switches on boot 21). Nothing compute wrote was read after a switch
//    there, and no copy fed compute: those changes keep the drain.
//  - rule barrier.entry_no_drain: a job opens with the planner's invalidates (what it reads: headers, samplers,
//    constants, shader and texture data) and no flush or wait, and its first encoder with nothing, after the job before
//    it ended with its fence's RELEASE_WFI on the same channel (raw_event_release.h). Judged by gpu.x1.job_boundary_idle
//    against entry_none, the first arm there that can see a hazard at a job's entry.
//  - rule barrier.pass_pixel: between two passes, PIXEL_SHADER_BARRIER and the texture invalidate it orders (0x0f74),
//    where raw_pass_hazard.h finds every dependency on the passes since the last drain in a cell gpu.x1.pass_pixel_barrier
//    held in.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "raw_compute_opening.h"
#include "raw_push.h"

namespace raw {

enum class BarrierEngine : uint8_t {
    None,  // nothing yet: the job's first work
    ThreeD,
    Copy,  // a blit encoder: the copy and 2D engines
    Compute
};

enum class PassForm : uint8_t {
    Rule,
    Today,
    PixelOrdered,  // PIXEL_SHADER_BARRIER, then the texture invalidate it orders (0x0f74)
    None
};

enum class EntryForm : uint8_t {
    Rule,
    Today,
    Planner,  // headers, samplers, constants, shader and texture data invalidated, nothing flushed or waited for
    Bare,     // headers and samplers only (and the instructions where the heap grew, as today)
    None      // the instructions where the heap grew alone: what job 0 left in every other cache stays
};

enum class SwitchForm : uint8_t {
    Rule,
    Today,
    NoWfi,         // the engine change instead of the wait; the tiled flush and the invalidates stay (NO_WFI forms)
    NoWfiNoFlush,  // and no tiled flush at the end of a pass
    None           // no wait, flush or data invalidate at an engine change
};

struct BarrierForms {
    PassForm pass = PassForm::Rule;
    EntryForm entry = EntryForm::Rule;
    SwitchForm switchForm = SwitchForm::Rule;
};

// What an opening emitted: today's drain, nothing (a relaxed entry's first encoder), a relaxed pass or engine change
enum class Opened : uint8_t {
    Today,
    First,
    Pass,
    Switch
};

constexpr bool switchDrainProven(BarrierEngine before, BarrierEngine next)
{
    return (before == BarrierEngine::ThreeD && (next == BarrierEngine::Copy || next == BarrierEngine::Compute)) ||
           (before == BarrierEngine::Copy && next == BarrierEngine::ThreeD);
}

constexpr SwitchForm switchFormFor(SwitchForm asked, BarrierEngine before, BarrierEngine next)
{
    if (asked != SwitchForm::Rule)
        return asked;
    return switchDrainProven(before, next) ? SwitchForm::NoWfi : SwitchForm::Today;
}

constexpr bool relaxed(SwitchForm f)
{
    return f != SwitchForm::Rule && f != SwitchForm::Today;
}

constexpr bool relaxed(EntryForm f)
{
    return f != EntryForm::Rule && f != EntryForm::Today;
}

constexpr EntryForm entryFormFor(EntryForm asked)
{
    return asked != EntryForm::Rule ? asked : EntryForm::Planner;
}

constexpr bool keepsTiledFlush(SwitchForm f)
{
    return f != SwitchForm::NoWfiNoFlush && f != SwitchForm::None;
}

namespace barrier_forms_detail {

// raw_kit.h's kit(), which this header cannot include beside the host tests (raw_transfer.h needs the kernel's ABI)
template <typename Emit, typename... Args> void barrier(Push &p, Emit emit, Args &&...args)
{
    nv_push n;
    nv_push_init(&n, p.at, size_t(p.end - p.at), SUBC_MASK_ALL);
    emit(&n, static_cast<Args &&>(args)...);
    p.at = n.end;
}

}  // namespace barrier_forms_detail

// A submission's prologue after its stamp, for an entry form other than today's: what the job reads invalidated by the
// form, nothing flushed or waited for (the job before ended with its fence's RELEASE_WFI). Answers the
// INVALIDATE_SHADER_CACHES_NO_WFI data word commit gives the INSTRUCTION bit where the heap grew
inline uint32_t *entryOpening(Push &p, EntryForm f)
{
    if (f != EntryForm::None) {
        p.m(NVB197_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
        p.m(NVB197_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
    }
    constexpr uint32_t data = NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE),
                       constant = NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE);
    p.inc(NVB197_INVALIDATE_SHADER_CACHES_NO_WFI, {f == EntryForm::Planner ? data | constant : 0u});
    uint32_t *word = p.at - 1;
    if (f == EntryForm::Planner)
        barrier_forms_detail::barrier(p, switch_gm20b_invalidate_texture_data);
    return word;
}

// A pass's opening (today switch_gm20b_pass_barrier without the tiled flush, which the pass before ends with)
inline Opened passOpening(Push &p, PassForm pass, SwitchForm switchForm, EntryForm entry, BarrierEngine before)
{
    if (before == BarrierEngine::None && relaxed(entry))
        return Opened::First;
    if (before == BarrierEngine::ThreeD && (pass == PassForm::PixelOrdered || pass == PassForm::None)) {
        if (pass == PassForm::PixelOrdered)
            barrier_forms_detail::barrier(p, switch_gm20b_pixel_shader_barrier);
        return Opened::Pass;
    }
    if ((before == BarrierEngine::Copy || before == BarrierEngine::Compute) && relaxed(switchForm)) {
        if (switchForm != SwitchForm::None) {
            barrier_forms_detail::barrier(p, switch_gm20b_invalidate_shader_caches_no_wfi,
                                          uint32_t(SWITCH_GM20B_SHADER_DATA | SWITCH_GM20B_SHADER_CONSTANTS));
            barrier_forms_detail::barrier(p, switch_gm20b_invalidate_texture_data);
        }
        return Opened::Switch;
    }
    barrier_forms_detail::barrier(p, switch_gm20b_pass_barrier, false);
    return Opened::Today;
}

// A blit encoder's opening: what the render work before wrote is complete before the copy engine reads it (NVK: a WFI on
// the 3D subchannel; the switch to the copy subchannel then waits for the 3D engine anyway)
inline Opened blitOpening(Push &p, SwitchForm switchForm, EntryForm entry, BarrierEngine before)
{
    if (before == BarrierEngine::None && relaxed(entry))
        return Opened::First;
    if (before != BarrierEngine::None && before != BarrierEngine::Copy && relaxed(switchForm))
        return Opened::Switch;
    p.m(NVB197_WAIT_FOR_IDLE, 0);
    return Opened::Today;
}

// A compute encoder's opening: the scheduler's and the compute class's header and sampler caches always; the flush and
// the data invalidates by the form (today's is raw_compute_opening.h's)
inline Opened computeEncoderOpening(Push &p, SwitchForm switchForm, EntryForm entry, BarrierEngine before)
{
    constexpr uint32_t dataNoWfi = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE),
                       constantNoWfi = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE);
    const bool first = before == BarrierEngine::None && relaxed(entry);
    const bool switched = before != BarrierEngine::None && before != BarrierEngine::Compute && relaxed(switchForm);
    if (!first && !switched) {
        computeOpening(p);
        return Opened::Today;
    }
    if (switched && switchForm != SwitchForm::None) {
        p.m(NVB1C0_INVALIDATE_SHADER_CACHES_NO_WFI, dataNoWfi | constantNoWfi, SUBC_COMPUTE);
        p.m(NVB1C0_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    }
    computeOpeningHeaders(p);
    return first ? Opened::First : Opened::Switch;
}

}  // namespace raw
