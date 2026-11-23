// SPDX-License-Identifier: MIT
// A pass of one-sample colour beside a four-sample depth/stencil, which Metal's validation layer stops and Apple's GPU renders
// (an M4 Max, validation off, c38-audit/m2v-pass-forms/probe/mixprobe.mm): it rasterises at the depth's four samples, tests
// and writes depth at each sample a triangle covers, and writes a colour pixel where any covered sample passes under a state
// of one sample, where sample 0 does under a state of four, the colour never scaled by coverage. (The first such pass of a
// process wrote no colour where only sample 0 passed: one pass, not mirrored.)
// The GM20B's form is target-independent rasterisation (NVB197 SET_TIR RASTER_N_TARGET_M): SET_ANTI_ALIAS the colour's one
// sample, SET_ANTI_ALIAS_RASTER the zeta's four, no TIR modulation. The zeta is cleared under its own layout before the mode
// is on. The class header does not say which SET_TIR_CONTROL REDUCE_COVERAGE value merges a pixel's raster samples (any)
// and which keeps the first (sample 0): hw_test_app gpu.gm20b.mixed_samples decides.
#pragma once
#include <cstdint>
#include "../../kits/SwitchGM20B/SwitchGM20B.h"
#include "push.h"
#include "samples.h"

namespace gm20b {

inline bool mixedSamples(uint32_t colour, uint32_t zeta)
{
    return colour == 1 && zeta == 4;
}

constexpr uint32_t kTirWords = 10;

// SET_TIR_CONTROL for a state of `stateSamples` in a mixed pass (REDUCE_COVERAGE is bit 1)
inline uint32_t tirControl(uint32_t stateSamples)
{
    return (stateSamples == 1 ? NVB197_SET_TIR_CONTROL_REDUCE_COVERAGE_ENABLE
                              : NVB197_SET_TIR_CONTROL_REDUCE_COVERAGE_DISABLE)
           << 1;
}

inline void tirOn(Push &p, uint32_t rasterSamples)
{
    SampleLayout raster;
    sampleLayout(rasterSamples, raster);
    p.m(NVB197_SET_ANTI_ALIAS, NVB197_SET_ANTI_ALIAS_SAMPLES_MODE_1X1);
    p.m(NVB197_SET_ANTI_ALIAS_RASTER, raster.antiAlias);
    p.m(NVB197_SET_TIR_MODULATION, NVB197_SET_TIR_MODULATION_COMPONENT_SELECT_NO_MODULATION);
    p.m(NVB197_SET_TIR_CONTROL, tirControl(1));
    p.m(NVB197_SET_TIR, NVB197_SET_TIR_MODE_RASTER_N_TARGET_M);
}

inline void tirOff(Push &p)
{
    p.m(NVB197_SET_TIR, NVB197_SET_TIR_MODE_DISABLED);
    p.m(NVB197_SET_ANTI_ALIAS_RASTER, NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_1X1);
}

}  // namespace gm20b
