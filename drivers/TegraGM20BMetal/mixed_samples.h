// SPDX-License-Identifier: MIT
// A pass of one-sample colour beside a four-sample depth/stencil, which Metal's validation layer stops and Apple's GPU renders
// (an M4 Max, validation off, c38-audit/m2v-pass-forms/probe/mixprobe.mm): it rasterises at the depth's four samples, tests
// and writes depth at each sample a triangle covers, and writes a colour pixel where any covered sample passes under a state
// of one sample, where sample 0 does under a state of four, the colour never scaled by coverage. (The first such pass of a
// process wrote no colour where only sample 0 passed: one pass, not mirrored.)
// The GM20B's form is target-independent rasterisation (NVB197 SET_TIR RASTER_N_TARGET_M): SET_ANTI_ALIAS the colour's one
// sample, SET_ANTI_ALIAS_RASTER the zeta's four, no TIR modulation, SET_TIR_CONTROL REDUCE_COVERAGE on (a colour pixel
// where any covered raster sample passes). The zeta is cleared under its own layout before the mode is on.
#pragma once
#include <cstdint>
#include "../../kits/SwitchGM20B/SwitchGM20B.h"
#include "pass_desc.h"
#include "pipeline.h"
#include "push.h"
#include "samples.h"
#include "stage_switches.h"
#include "zeta.h"

namespace gm20b {

inline bool mixedSamples(uint32_t colour, uint32_t zeta)
{
    return colour == 1 && zeta == 4;
}

constexpr uint32_t kTirWords = 10;

// GM20B b197 faults INVALID_TIR_COMBINATION05 (0x127) if REDUCE_COVERAGE is disabled under RASTER_N_TARGET_M, so a
// four-sample state's sample-0 colour rule is drawn by sample masks (MixedDraw) rather than by this bit.
inline uint32_t tirControl(uint32_t)
{
    return NVB197_SET_TIR_CONTROL_REDUCE_COVERAGE_ENABLE << 1;
}

constexpr uint32_t kFirstSampleMask = 0x1111, kLaterSamplesMask = 0xeeee;

// How a draw keeps Apple's rule for a four-sample state in a mixed pass (colour where sample 0 passes, depth and stencil at
// every covered sample) under REDUCE_COVERAGE. Sample 0's colour and samples 1-3's depth come apart because no sample's
// test reads another's.
enum class MixedDraw : uint8_t {
    Whole,        // every sample at once: not such a draw, or one writing no colour
    FirstSample,  // sample 0 alone: the state writes no depth or stencil, which samples 1-3 then keep
    // sample 0, then samples 1-3 with colour writes and the pixel stage off: its program shapes no coverage, so what
    // samples 1-3 test and write needs no program run, and its side effects run once
    Split,
    SplitShaded,  // sample 0, then samples 1-3 with colour writes off and the fragment program run again
    // every sample at once: its program shapes coverage and writes memory, which a second run would repeat. Its colour
    // lands where any covered sample passes (REDUCE_COVERAGE), not only where sample 0 does
    Covering,
};

// `alphaToCoverage`: the bound Metal state turns it on (the encoder's, alpha_control.h)
inline MixedDraw mixedDraw(const Pipeline &pl, const DepthStencilDesc &ds, const DepthStencilAttachment &zs,
                           uint32_t passSamples, uint32_t rasterSamples, bool alphaToCoverage)
{
    if (rasterSamples <= passSamples || pl.samples <= passSamples || !zs.texture || pl.stagesOff ||
        !(pl.writeBits | pl.storageWriteBits))
        return MixedDraw::Whole;
    const uint32_t f = zs.texture->format;
    if (!ds.writes(zetaHasDepth(f) && zs.depthEnabled, zetaHasStencil(f) && zs.stencilEnabled))
        return MixedDraw::FirstSample;
    if (!pl.shapesCoverage && !alphaToCoverage)
        return MixedDraw::Split;
    return pl.writesMemory[1] || pl.storageWriteBits ? MixedDraw::Covering : MixedDraw::SplitShaded;
}

inline bool splitsSamples(MixedDraw d)
{
    return d == MixedDraw::Split || d == MixedDraw::SplitShaded;
}

// SET_SAMPLE_MASK_X0_Y0..X1_Y1, a CT_WRITE of every target and the pixel stage's switch, each way round a split's second
// draw
constexpr uint32_t kLaterSamplesWords = 5 + 9 + 2;

// `stageOff`: the pixel stage off for the second draw (MixedDraw::Split), as a state of no fragment program draws
// (stage_switches.h): depth and stencil through the ROP, no colour
inline void laterSamplesOn(Push &p, uint32_t rasterMask, uint32_t targets, bool stageOff)
{
    const uint32_t m = rasterMask & kLaterSamplesMask;
    const uint32_t off[8] = {};
    p.inc(NVB197_SET_SAMPLE_MASK_X0_Y0, {m, m, m, m});
    if (targets)
        p.incArray(NVB197_SET_CT_WRITE(0), off, targets);
    if (stageOff)
        p.m(kPixelStageSwitch, kPixelStageOff);
}

inline void laterSamplesOff(Push &p, const uint32_t (&ctWrite)[8], uint32_t targets, bool stageOff)
{
    if (stageOff)
        p.m(kPixelStageSwitch, kPixelStageOn);
    if (targets)
        p.incArray(NVB197_SET_CT_WRITE(0), ctWrite, targets);
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

// GM20B b197 faults INVALID_VALUE (0x004) on SET_ANTI_ALIAS_RASTER = 1X1 (0), even with SET_TIR disabled.
inline void tirOff(Push &p)
{
    p.m(NVB197_SET_TIR, NVB197_SET_TIR_MODE_DISABLED);
}

// HW Test's rows of a mixed pass's methods (Device::probeTir, debug builds; gpu.gm20b.zc_mixed, gpu.gm20b.mixed_end_rows),
// which ask GM20B which combinations its class takes where the header does not say: the class killed boot 2's mixed
// passes with INVALID_TIR_COMBINATION05 at the first draw of one given a zcull region, and INVALID_VALUE at the pass end's
// SET_ANTI_ALIAS_RASTER 1X1. A row's word: bit 0 armed; bit 1 a zcull region; bit 2 the region over the zeta's sample
// grid (2x2 a pixel), not its pixels; bit 3 the region's words after the raster state, not before the clears; bit 4
// SET_TIR left DISABLED; bit 5 a four-sample state's draw whole, never split (gpu.gm20b.mixed_merge_probe: whether a
// sample mask under REDUCE_COVERAGE gives Apple's sample-0 rule in one draw); bits 8-11 the pass end's order (TirEnd);
// bits 12-15 the raster value TirEnd::Value writes.
constexpr uint32_t kTirProbeZcull = 1u << 1, kTirProbeSampleGrid = 1u << 2, kTirProbeZcullAfter = 1u << 3,
                   kTirProbeModeOff = 1u << 4, kTirProbeWhole = 1u << 5;

enum class TirEnd : uint32_t {
    Mode,            // tirOff: SET_TIR DISABLED, then SET_ANTI_ALIAS_RASTER 1X1
    RasterFirst,     // SET_ANTI_ALIAS_RASTER 1X1, then SET_TIR DISABLED
    ZetaOff,         // SET_ZT_SELECT 0 first (the next pass's targets select their own), then tirOff
    ModeOnly,        // SET_TIR DISABLED alone, the raster count left
    RasterDisabled,  // SET_RASTER_ENABLE FALSE around tirOff
    Value,           // SET_TIR DISABLED, SET_ANTI_ALIAS_RASTER of the row's value, then ZetaOff's reset
};

inline TirEnd tirProbeEnd(uint32_t probe)
{
    return TirEnd(probe >> 8 & 15);
}

inline uint32_t tirProbeRaster(uint32_t probe)
{
    return probe >> 12 & 15;
}

// A mixed pass's end under probe row `probe`
inline void tirOffProbed(Push &p, uint32_t probe)
{
    switch (tirProbeEnd(probe)) {
    case TirEnd::RasterFirst:
        p.m(NVB197_SET_ANTI_ALIAS_RASTER, NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_1X1);
        p.m(NVB197_SET_TIR, NVB197_SET_TIR_MODE_DISABLED);
        return;
    case TirEnd::ZetaOff:
        p.m(NVB197_SET_ZT_SELECT, 0);
        tirOff(p);
        return;
    case TirEnd::ModeOnly:
        p.m(NVB197_SET_TIR, NVB197_SET_TIR_MODE_DISABLED);
        return;
    case TirEnd::RasterDisabled:
        p.m(NVB197_SET_RASTER_ENABLE, NVB197_SET_RASTER_ENABLE_V_FALSE);
        tirOff(p);
        p.m(NVB197_SET_RASTER_ENABLE, NVB197_SET_RASTER_ENABLE_V_TRUE);
        return;
    case TirEnd::Value:
        p.m(NVB197_SET_TIR, NVB197_SET_TIR_MODE_DISABLED);
        p.m(NVB197_SET_ANTI_ALIAS_RASTER, tirProbeRaster(probe));
        p.m(NVB197_SET_ZT_SELECT, 0);
        p.m(NVB197_SET_ANTI_ALIAS_RASTER, NVB197_SET_ANTI_ALIAS_RASTER_SAMPLES_MODE_1X1);
        return;
    case TirEnd::Mode:
        break;
    }
    tirOff(p);
}

}  // namespace gm20b
