// SPDX-License-Identifier: MIT
// The fetch ladder's fixed-function rungs (raw_pack.h PackFetchRung): where a state that blends by fetching its own
// colour may instead draw through the ROP, with its fetch-free form (raw_fetch_free.h) where the pack holds one. Each
// rung is taken only once its unit is proven on the chip (raw_chip_facts.h kProvenUnits, kIteratedProven).
#pragma once
#include <cstdint>
#include "raw_chip_facts.h"

namespace raw {

constexpr bool fixedBlendProven()
{
    return kProvenUnits & ProvenFixedBlend;
}

// A debug build's probe values (Device::probeFetchRung) past the rungs: a fetching state drawn by its barrier twin instead
// of its storage form (a blended storage form's blendTwin, else its barrierTwin), which reads the colour through the
// texture unit behind the fetch barrier and leaves any blend to the ROP: one barrier a triangle (each ordered after the
// one before) or one for the whole draw (its triangles not ordered among themselves). No rule takes the twin until the
// device has measured both against the storage form (HW Test gpu.raw.blend_twin).
constexpr uint8_t kProbeBlendTwinTriangles = 5, kProbeBlendTwinWhole = 6;

// Rung 2, rule fetch.blend: the ROP's blend. GM20B's blender is exact integer arithmetic on the source and source alpha
// quantised to 8-bit codes by the store rule, divided by 255 and rounded once (x1-units UNITS.md, fitted to every device
// count of gpu.x1.blend_exact; bench blendff 81,920 of 81,920): within one code of Metal for 8-bit content at any depth,
// up to 2 codes for shader-computed colour stacked twice, 4 codes deeper. It is taken only where that error cannot be seen
// as data: an 8-bit unorm target the client never reads back, not re-blended across frames, of standard factors.
struct FixedBlendAsk {
    uint16_t format = 0;           // MTLPixelFormat
    bool readable = false;         // CPU-visible storage, a blit or compute pass reads it as data, or a HW Test golden
    bool accumulates = false;      // a feedback target re-blended across frames
    bool standardFactors = false;  // Metal's blend factor pairs, not the iterated unit's programs
    bool eightBitSources = false;  // what blends into it is 8-bit content (textures of unorm8 images, 8-bit colours)
    uint8_t computedLayers = 0;    // shader-computed colours stacked over one pixel, where the sources are not 8-bit
};

constexpr bool fixedBlendAcceptable(const FixedBlendAsk &a)
{
    const bool unorm8 = a.format == 70 || a.format == 80;  // RGBA8Unorm, BGRA8Unorm
    return fixedBlendProven() && unorm8 && !a.readable && !a.accumulates && a.standardFactors &&
           (a.eightBitSources || a.computedLayers <= 2);
}

// Rung 3, rule fetch.iterated: the ROP's iterated blend (kits/SwitchGM20BEmit/SwitchGM20BEmitIterated.h) for blends with
// no fixed-function equation (Core Animation's separable, non-separable and luminosity modes). Its setup is the one
// x1-units derived from the class errors (on T-v2 the chip ran 5 of 5 of NVN's programs under it, 4 within one code).
struct IteratedAsk {
    bool candidate = false;  // the colour's fetch-free form is on rung 3
    uint32_t colourTargets =
        0;  // targets its draw binds (the unit refuses more than one: 0x59; the draw narrows to one)
    uint32_t programWords = 0;  // its microcode, which ends in a write of TEMP0: one pass a word
    uint32_t constants = 0;     // half-float constants it reads
    uint64_t program = 0;       // its name's FNV-1a 64 (PackIteratedBlend::name)
};

constexpr uint64_t iteratedProgramName(std::string_view name)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : name)
        h = (h ^ uint8_t(c)) * 0x100000001b3ull;
    return h;
}

// Whether the device proved the program worth its passes (raw_chip_facts.h kIteratedProven)
constexpr bool iteratedProgramProven(uint64_t program)
{
    for (const std::string_view name : kIteratedProven)
        if (iteratedProgramName(name) == program)
            return true;
    return false;
}

constexpr bool iteratedBlendUsable(const IteratedAsk &a)
{
    return a.candidate && a.colourTargets == 1 && a.programWords >= 1 && a.programWords <= 128 && a.constants <= 24 &&
           iteratedProgramProven(a.program);
}

}  // namespace raw
