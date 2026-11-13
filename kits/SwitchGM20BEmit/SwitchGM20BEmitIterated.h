// SPDX-License-Identifier: MIT
// The ROP's iterated blend (fetch rung 3): a microcode program the ROP runs per sample over the source and the
// destination, as NVN drives it for advanced blending. The class rejects a draw under it with per-target blend state on
// (ITERATED_BLEND_WITH_PER_TARGET_BLEND, 0x5d) or more than one colour target bound (ITERATED_BLEND_TARGET_COUNT, 0x59),
// and a pass count of 0 at the method (INVALID_VALUE): the setup below is the one x1-units' probe derived from those
// errors and NVN's programs (Ryujinx's AdvancedBlendFunctions table), each program ending in a write of TEMP0.
#pragma once

#include "../SwitchGM20B/SwitchGM20B3D.h"
#include "SwitchGM20BEmit3D.h"
#include "nv_push.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_GM20B_ITERATED_BLEND_MAX_PROGRAM   128u
#define SWITCH_GM20B_ITERATED_BLEND_MAX_CONSTANTS 24u  // halves: R, G, B of SET_ITERATED_BLEND_CONSTANT_*(0..7)

struct switch_gm20b_iterated_program {
    const uint32_t *program;  // LOAD_ITERATED_BLEND_INSTRUCTION words
    uint32_t count;           // 1 .. SWITCH_GM20B_ITERATED_BLEND_MAX_PROGRAM
    bool alpha;               // the program writes alpha too (ALPHA_ENABLE); else the common block's alpha blend does
    struct switch_gm20b_blend_state common;  // the blend NVN pairs with the program, in the common (global) block
    const uint16_t *constants;               // half-float bits, R G B of constant 0 then 1 ...
    uint32_t constant_count;
};

#define SWITCH_GM20B_ITERATED_BLEND_WORDS(count, constants) (16u + 4u * (((constants) + 2u) / 3u) + (count))
#define SWITCH_GM20B_ITERATED_BLEND_OFF_WORDS               2u

// Per-target blend off, the common block programmed, the zero-source drop off, the program loaded from instruction 0,
// its pass count and the enable. The caller binds colour target 0 alone for the draws that follow. The drop
// (SET_ITERATED_BLEND_OPTIMIZATION NOOP_SOURCE_RGBA_0000, which raw's source-over states set: raw_blend_kill.h) keeps
// the destination under a zero source, which a program that ignores the destination does not: T-v2's gpu.x1.iterated_blend
// pass arm kept it on its zero-alpha row, 510 fields up to 255 codes off. The next state bind writes the state's own value.
void switch_gm20b_iterated_blend(struct nv_push *p, const struct switch_gm20b_iterated_program *b);

// Its first part: per-target blend off and the common block programmed, where the program, its constants, its pass count
// and the enable come from elsewhere (SWITCH_GM20B_MACRO_VCM_CONSTANTS, each draw)
#define SWITCH_GM20B_ITERATED_BLEND_COMMON_WORDS 10u
void switch_gm20b_iterated_blend_common(struct nv_push *p, const struct switch_gm20b_blend_state *common);

// Iterated blend off and per-target blend back on, the state NVK's context init leaves (the pass count stays: 0 is not a
// legal value)
void switch_gm20b_iterated_blend_off(struct nv_push *p);

#ifdef __cplusplus
}
#endif
