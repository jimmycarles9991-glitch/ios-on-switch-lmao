// SPDX-License-Identifier: MIT
// hw_test_app's barrier probes (app27/X1BarrierProbes.swift), debug builds only: the forms one command buffer's barriers
// take instead of today's, so a probe compares a relaxed form's pixels and GPU time with today's on the same work.
//
// The relaxed forms are what raw/plan (branch x1-plan) lowers once a probe passed: between two passes whose dependency is
// colour written to fragments sampling it, the tiled flush (the earlier pass's body keeps it) and switch_gm20b_pixel_shader
// _barrier (plan_hazard.cpp CrossPassPixel); at a job's entry, no flush or wait, only the invalidates of what the job reads
// (plan_barriers.cpp entry with barrier.entry_no_drain); at an engine change, the change itself in place of the wait
// (barrier.switch_drain). PixelNoWfi and Bare are the readings of the probe brief (0x1288 after PIXEL_SHADER_BARRIER; an
// entry with no data invalidates at all); None removes the barrier, the arm that shows a probe can see a hazard at all.
#pragma once
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <cstdint>
#include <cstring>
#include <string>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "raw_compute_opening.h"
#include "raw_push.h"

namespace raw {

namespace probe_forms_detail {

// raw_kit.h's kit(), which this header cannot include beside the host tests (raw_transfer.h needs the kernel's ABI)
template <typename Emit, typename... Args> void barrier(Push &p, Emit emit, Args &&...args)
{
    nv_push n;
    nv_push_init(&n, p.at, size_t(p.end - p.at), SUBC_MASK_ALL);
    emit(&n, static_cast<Args &&>(args)...);
    p.at = n.end;
}

}  // namespace probe_forms_detail

enum class PassForm : uint8_t {
    Today,
    PixelOrdered,  // PIXEL_SHADER_BARRIER, then the texture invalidate it orders (0x0f74)
    PixelNoWfi,    // PIXEL_SHADER_BARRIER, then INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI (0x1288)
    None
};

enum class EntryForm : uint8_t {
    Today,
    Planner,  // headers, samplers, constants, shader and texture data invalidated, nothing flushed or waited for
    Bare      // headers and samplers only (and the instructions where the heap grew, as today)
};

enum class SwitchForm : uint8_t {
    Today,
    NoWfi,         // the engine change instead of the wait; the tiled flush and the invalidates stay (NO_WFI forms)
    NoWfiNoFlush,  // and no tiled flush at the end of a pass
    None           // no wait, flush or data invalidate at an engine change
};

enum class ProbeEngine : uint8_t {
    None,
    ThreeD,
    Copy,
    Compute
};

struct ProbeForms {
    PassForm pass = PassForm::Today;
    EntryForm entry = EntryForm::Today;
    SwitchForm switchForm = SwitchForm::Today;
    bool resolveTwoD = false;
    // an indexed draw's index fetch ends this many bytes past its offset (SET_INDEX_BUFFER_C/D), its indices never
    // padded: the hardware's answer past the limit is the draw's (0: today's)
    uint32_t indexLimit = 0;
    // a storage form's draws take its barrier twin wherever it has one (HW Test gpu.raw.xr_two_plane's barrier cells)
    bool barrierFetch = false;

    bool any() const
    {
        return pass != PassForm::Today || entry != EntryForm::Today || switchForm != SwitchForm::Today || resolveTwoD ||
               indexLimit || barrierFetch;
    }

    bool keepsTiledFlush() const
    {
        return switchForm != SwitchForm::NoWfiNoFlush && switchForm != SwitchForm::None;
    }
};

// Where a command buffer's barriers took a form other than today's (0 everywhere: the probe's work never met one)
struct ProbeTaken {
    uint32_t passes = 0, entries = 0, firsts = 0, switches = 0, flushes = 0, resolves = 0, ops = 0, indexLimits = 0;

    void add(const ProbeTaken &o)
    {
        passes += o.passes, entries += o.entries, firsts += o.firsts, switches += o.switches;
        flushes += o.flushes, resolves += o.resolves, ops += o.ops, indexLimits += o.indexLimits;
    }

    std::string text() const
    {
        return "passes=" + std::to_string(passes) + " entries=" + std::to_string(entries) +
               " firsts=" + std::to_string(firsts) + " switches=" + std::to_string(switches) +
               " flushes=" + std::to_string(flushes) + " resolves=" + std::to_string(resolves) +
               " ops=" + std::to_string(ops) + " index_limits=" + std::to_string(indexLimits);
    }
};

// "pass=<today|pixel|pixel_nowfi|none> entry=<today|planner|bare> switch=<today|nowfi|nowfi_noflush|none>
// resolve=<draw|twod> index=<bytes> fetch=<today|barrier>", any subset in any order; false on anything else
inline bool parseProbeForms(const char *text, ProbeForms &out)
{
    ProbeForms f;
    std::string s(text ? text : "");
    size_t at = 0;
    while (at < s.size()) {
        const size_t end = std::min(s.find(' ', at), s.size());
        const std::string field = s.substr(at, end - at);
        at = end + 1;
        if (field.empty())
            continue;
        const size_t eq = field.find('=');
        if (eq == std::string::npos)
            return false;
        const std::string key = field.substr(0, eq), value = field.substr(eq + 1);
        if (key == "pass" && value == "today")
            f.pass = PassForm::Today;
        else if (key == "pass" && value == "pixel")
            f.pass = PassForm::PixelOrdered;
        else if (key == "pass" && value == "pixel_nowfi")
            f.pass = PassForm::PixelNoWfi;
        else if (key == "pass" && value == "none")
            f.pass = PassForm::None;
        else if (key == "entry" && value == "today")
            f.entry = EntryForm::Today;
        else if (key == "entry" && value == "planner")
            f.entry = EntryForm::Planner;
        else if (key == "entry" && value == "bare")
            f.entry = EntryForm::Bare;
        else if (key == "switch" && value == "today")
            f.switchForm = SwitchForm::Today;
        else if (key == "switch" && value == "nowfi")
            f.switchForm = SwitchForm::NoWfi;
        else if (key == "switch" && value == "nowfi_noflush")
            f.switchForm = SwitchForm::NoWfiNoFlush;
        else if (key == "switch" && value == "none")
            f.switchForm = SwitchForm::None;
        else if (key == "resolve" && (value == "draw" || value == "twod"))
            f.resolveTwoD = value == "twod";
        else if (key == "index" && !value.empty() && value.size() < 9 &&
                 value.find_first_not_of("0123456789") == std::string::npos && std::stoul(value))
            f.indexLimit = uint32_t(std::stoul(value));
        else if (key == "fetch" && (value == "today" || value == "barrier"))
            f.barrierFetch = value == "barrier";
        else
            return false;
    }
    out = f;
    return true;
}

// A pass's prologue barrier (today switch_gm20b_pass_barrier without the tiled flush, which the pass before ends with),
// by what ran before it: nothing (the job's first work), a pass, or another engine
inline void probePassPrologue(Push &p, const ProbeForms &f, ProbeEngine before, ProbeTaken &taken)
{
    if (before == ProbeEngine::None && f.entry != EntryForm::Today) {
        taken.firsts++;
        return;
    }
    if (before == ProbeEngine::ThreeD && f.pass != PassForm::Today) {
        taken.passes++;
        switch (f.pass) {
        case PassForm::PixelOrdered:
            probe_forms_detail::barrier(p, switch_gm20b_pixel_shader_barrier);
            return;
        case PassForm::PixelNoWfi:
            p.m(NVB197_PIXEL_SHADER_BARRIER, 0);
            probe_forms_detail::barrier(p, switch_gm20b_invalidate_texture_data);
            return;
        default:
            return;
        }
    }
    if ((before == ProbeEngine::Copy || before == ProbeEngine::Compute) && f.switchForm != SwitchForm::Today) {
        taken.switches++;
        if (f.switchForm != SwitchForm::None) {
            probe_forms_detail::barrier(p, switch_gm20b_invalidate_shader_caches_no_wfi,
                                        uint32_t(SWITCH_GM20B_SHADER_DATA | SWITCH_GM20B_SHADER_CONSTANTS));
            probe_forms_detail::barrier(p, switch_gm20b_invalidate_texture_data);
        }
        return;
    }
    probe_forms_detail::barrier(p, switch_gm20b_pass_barrier, false);
}

// A blit encoder's opening wait (today WAIT_FOR_IDLE after any work)
inline void probeBlitOpening(Push &p, const ProbeForms &f, ProbeEngine before, ProbeTaken &taken)
{
    if (before == ProbeEngine::None && f.entry != EntryForm::Today) {
        taken.firsts++;
        return;
    }
    if (before != ProbeEngine::None && before != ProbeEngine::Copy && f.switchForm != SwitchForm::Today) {
        taken.switches++;
        return;
    }
    p.m(NVB197_WAIT_FOR_IDLE, 0);
}

// A compute encoder's opening (raw_compute.cpp's ComputeEncoder): the scheduler's and the compute class's header and
// sampler caches always; the flush and the data invalidates by the form (today's is raw_compute_opening.h's, as shipped)
inline void probeComputeOpening(Push &p, const ProbeForms &f, ProbeEngine before, ProbeTaken &taken)
{
    constexpr uint32_t dataNoWfi = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE),
                       constantNoWfi = NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE);
    const bool first = before == ProbeEngine::None && f.entry != EntryForm::Today;
    const bool switched =
        before != ProbeEngine::None && before != ProbeEngine::Compute && f.switchForm != SwitchForm::Today;
    if (!first && !switched) {
        computeOpening(p);
        return;
    }
    if (first) {
        taken.firsts++;
    } else {
        taken.switches++;
        if (f.switchForm != SwitchForm::None) {
            p.m(NVB1C0_INVALIDATE_SHADER_CACHES_NO_WFI, dataNoWfi | constantNoWfi, SUBC_COMPUTE);
            p.m(NVB1C0_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
        }
    }
    computeOpeningHeaders(p);
}

// A submission's prologue after its stamp, for an entry form other than today's; answers the
// INVALIDATE_SHADER_CACHES_NO_WFI data word commit gives the INSTRUCTION bit where the heap grew
inline uint32_t *probeEntry(Push &p, EntryForm f, ProbeTaken &taken)
{
    taken.entries++;
    p.m(NVB197_INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
    p.m(NVB197_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
    constexpr uint32_t data = NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, GLOBAL_DATA, TRUE),
                       constant = NVDEF(NVB197, INVALIDATE_SHADER_CACHES_NO_WFI, CONSTANT, TRUE);
    p.inc(NVB197_INVALIDATE_SHADER_CACHES_NO_WFI, {f == EntryForm::Planner ? data | constant : 0u});
    uint32_t *word = p.at - 1;
    if (f == EntryForm::Planner)
        probe_forms_detail::barrier(p, switch_gm20b_invalidate_texture_data);
    return word;
}

}  // namespace raw
#endif
