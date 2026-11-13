// SPDX-License-Identifier: MIT
// What the planner may choose: kits/SwitchGM20B:caps rows, the probes passed on the chip, and the pack's own states.
// A rule rests on one caps feature row at a grade, and a rule that needs a probe is not selectable until it passed.
#pragma once
#include "kits/SwitchGM20B/SwitchGM20BCaps.h"
#include "plan_graph.h"

namespace raw::plan {

enum class Grade : uint8_t {
    Source = SWITCH_GM20B_GRADE_SOURCE,
    Host = SWITCH_GM20B_GRADE_HOST,
    Chip = SWITCH_GM20B_GRADE_CHIP
};

enum class Probe : uint8_t {
    None,
    CeCompressed,
    TwodScale,
    TwodResolve,
    TwodFill,
    MipFilter,
    BlendExact,
    IteratedBlend,
    PassPixelBarrier,
    IdleJobBoundary,
    SwitchDrain,
    Count
};

constexpr const char *kProbeIds[] = {"-",
                                     "gpu.x1.ce_compressed",
                                     "gpu.x1.twod_scale",
                                     "gpu.x1.twod_resolve",
                                     "gpu.x1.twod_clear",
                                     "gpu.x1.mip_filter",
                                     "gpu.x1.blend_exact",
                                     "gpu.x1.iterated_blend",
                                     "gpu.x1.pass_pixel_barrier",
                                     "gpu.x1.job_boundary_idle",
                                     "gpu.x1.subchannel_switch_drain"};
static_assert(sizeof kProbeIds / sizeof *kProbeIds == size_t(Probe::Count), "an id for each probe");

// The probes that passed on the chip (stack-F-v1, 11 Nov): ce_compressed for one-sample C32 and C64 colour kinds
// only (BGRA8, RGBA16F: C128, MS2/MS4 and zeta kinds unproven, so copy.ce_compressed lowers only those), mip_filter,
// and pass_pixel_barrier's planner form (26 cells held, none failed; the strided reads at 320x180 and 1280x720 were
// blind and are re-probed; the 0x1288 reading failed)
constexpr uint32_t kProvenOnChip =
    1u << uint32_t(Probe::CeCompressed) | 1u << uint32_t(Probe::MipFilter) | 1u << uint32_t(Probe::PassPixelBarrier);

struct RuleEvidence {
    Rule rule;
    switch_gm20b_feature feature;
    Grade grade;
    Probe probe;
};

const RuleEvidence &evidence(Rule rule);

// The pack's own states (L4): which of raw::Device's standIn/resolve/ycbcr/xr/mip pipelines exist for a format
struct OwnStates {
    const void *context = nullptr;
    bool (*has)(const void *context, OwnState state, uint16_t format) = nullptr;

    bool available(OwnState state, uint16_t format) const
    {
        return has && has(context, state, format);
    }
};

struct Caps {
    uint32_t proven = 0;     // bit per Probe that passed on the chip
    bool tiledCache = true;  // the kernel put the tile coalescer's table in the context (TegraGM20BAbiInfo)
    OwnStates own;

    static const switch_gm20b_format *format(uint16_t mtl);
    static bool hasFeature(switch_gm20b_feature f, Grade at);
    static bool formatHas(uint16_t mtl, uint32_t flag);

    bool passed(Probe p) const
    {
        return p == Probe::None || (proven >> uint32_t(p) & 1);
    }

    bool selectable(Rule rule) const;
};

}  // namespace raw::plan
