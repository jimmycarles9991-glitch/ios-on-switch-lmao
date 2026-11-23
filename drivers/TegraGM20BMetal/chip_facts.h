// SPDX-License-Identifier: MIT
// The chip facts (pack.h kChipAssumptions) a device probe has proved on GM20B. Every program a record holds is compiled
// under them, its generic one too (direct.cpp), and the pack builder stamps them into the pack header
// (PackHeader::chipFacts, pack/packfile.py chip_facts), whose draws then hold them. A bit is set here by hand, from the
// probe's log line, never at run time; pack/packfile.py reads the literal, so it stays a number.
//   AssumeTexsF16 (1 << 18): SwitchHWTest gpu.gm20b.texs_f16 on boots 14, 15, 17, 18, 19 and 21: every unorm8_* and
//     rgba16f_* "_f16_wrong" 0, its "_proof" 0x53460001
//   AssumeRopRounding: no probe yet (COMPILER.md E3)
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace gm20b {

constexpr uint32_t kProvenChipFacts = 0x40000;

// The ROP units a device probe proved the driver drives right, set by hand from the probe's line (never at run time):
//   ProvenFixedBlend: SwitchHWTest gpu.x1.blend_exact "model=chip chip_misses=0" (T-v2, 11 Nov: the 8-bit integer blender
//     reproduces every blended channel of 8.4 M per factor pair on raw)
enum ProvenUnit : uint32_t {
    ProvenFixedBlend = 1u << 0,
};

constexpr uint32_t kProvenUnits = ProvenFixedBlend;

// The iterated blend programs rung 3 draws (pack/iterated/table.py's names; vcm, the colour matrix macro's, and vcm.rows,
// its route for a QuartzCore whose matrix has no clamp: pack/fetchfree_link.py): those whose passes cost less than the
// interlocked storage form they stand in for.
// Accuracy: on T-v3i and T-v4c (x1-bench3/BENCH3.md) every Core Animation program ran within 1 code of the exact
// interpreter of its words; lum only for premultiplied colour.
// Cost at 720p (BENCH3 section 5, T-v3i/T-v4c): the first pass is free, each further one 150.3 us, on BGRA8 and RGBA16F,
// pitch and block-linear alike. Boot 21's interlocked storage forms: 1,501.5 us for Core Animation's add (gpu.gm20b.fetch_rung2
// us_rung4_add), the shortest in-shader blend; 1,542.4 us for a bare fetch (gpu.gm20b.fetch_blend_cost); 2,295.4 us for vcm
// (gpu.gm20b.blend_twin). A longer in-shader blend costs the storage form more: about 2.6 us a dependent instruction inside
// the interlock (BENCH3 T-v4c interlock section, 64 to 256 FFMAs).
//   up to 9 passes (1,353 us): below every storage form
//   10 passes (1,503 us: hlt, sbt.x, llt.x, dlc.u): level with add's, whose blend is the shortest; their own storage forms
//     carry longer blends
//   11 passes and more (1,653 us and up): slt, div, cbn.u, div.x, cdg, dso, cdg.x, lso, cbn, cbn.x, dlc 28, col and lum 42,
//     cli 54, hue and sat 64, vcm 26-38 (3,908 us and more against its 2,295) lose to the interlock and stay on it
// SwitchHWTest gpu.gm20b.fetch_rung3 judges each on the device: worst_<mode> (codes off the storage form, bound 2),
// us_rung3_<mode> against us_rung4_<mode>, "slower" and "proven".
inline constexpr std::array<std::string_view, 20> kIteratedProven = {
    "mpy", "pdi", "lbn", "drk",   "ltn", "sbt.u", "llt.u", "pld",   "sds",   "div.u",
    "dif", "ovl", "pin", "cdg.u", "sbt", "llt",   "hlt",   "sbt.x", "llt.x", "dlc.u",
};

}  // namespace gm20b
