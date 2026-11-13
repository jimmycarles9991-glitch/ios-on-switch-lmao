// SPDX-License-Identifier: MIT
// The chip facts (raw_pack.h kChipAssumptions) a device probe has proved on GM20B. The pack builder compiles the variants
// that assume them and stamps them into the pack header (PackHeader::chipFacts, pack/packfile.py chip_facts), whose draws
// then hold them. A bit is set here by hand, from the probe's log line, never at run time.
//   AssumeTexsF16: SwitchHWTest gpu.raw.texs_f16: every unorm8_* and rgba16f_* "_f16_wrong" 0, its "_proof" 0x5346....
//   AssumeRopRounding: no probe yet (COMPILER.md E3)
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace raw {

constexpr uint32_t kProvenChipFacts = 0;

// The ROP units a device probe proved the raw path drives right, set by hand from the probe's line (never at run time):
//   ProvenFixedBlend: SwitchHWTest gpu.x1.blend_exact "model=chip chip_misses=0" (T-v2, 11 Nov: the 8-bit integer blender
//     reproduces every blended channel of 8.4 M per factor pair on raw)
enum ProvenUnit : uint32_t {
    ProvenFixedBlend = 1u << 0,
};

constexpr uint32_t kProvenUnits = ProvenFixedBlend;

// The iterated blend programs (pack/iterated/table.py's names; vcm, the colour matrix macro's, and vcm.rows, its route
// for a QuartzCore whose matrix has no clamp: pack/fetchfree_link.py) the device showed within their bound of the
// storage form and faster than it, each by its own measured cost, not by a pass count: SwitchHWTest
// gpu.raw.fetch_rung3 "proven" (every state of the program within the bound and faster), with gpu.x1.iterated_blend
// "nvn_within_one" equal to "nvn_run". None has been, so rung 3 takes no draw.
inline constexpr std::array<std::string_view, 0> kIteratedProven = {};

// The colour ROP writes a zeta's memory (PTE kinds ZF32_X24S8 and S8) as a colour target of its texel size, and the zeta's
// depth and stencil tests and the texture unit then read what it wrote: the one-draw stencil route (raw_zeta_colour.h).
// Set by hand from SwitchHWTest gpu.raw.zeta_colour: claim_verdict=PASS (its colour arm's counts 0, colour_bit_draws=0,
// colour_rop_draws and colour_turn_cells above 0), reopens=0, gpu_after=1, and no "fault", "ROP", "LTC" or "TRAP"
// kernel line in its window. Until then stencil planes take the eight-draw route.
constexpr bool kColourRopWritesZeta = false;

}  // namespace raw
