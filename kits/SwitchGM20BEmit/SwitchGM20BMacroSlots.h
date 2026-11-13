// SPDX-License-Identifier: MIT
// The 3D engine's macro slots (CALL_MME_MACRO(j)) and MME shadow scratch words as the GM20B's two drivers use them: the
// one table of the raw path's macro ABI v6. NVK's context loads its macros at slots 0..NVK_MME_COUNT-1 in nvk_mme.h's
// order, and the raw path's context replays that load before it loads its own, so the kernel's push profile reads NVK's
// clear and anti-alias macros by NVK's numbers. The raw path's draw macros take NVK's draw macros' slots, so the
// profile counts both drivers' draws as one, and every other raw macro takes a slot past them, where NVK's streams call
// macros that draw nothing: the profile reads those slots as the raw path's only in a raw job's stream, its indirect
// command buffer, patch and amplified draws as draws and its layered clear as a clear.
// GM20B's 3D class runs the Fermi MME (mme_fermi.h, kits/SwitchGM20B/caps/limits.tsv): 128 macros, 2048 words of code
// and 128 shadow scratch words. SET_MME_SHADOW_SCRATCH(128) and up are illegal methods, which kill the channel.
// The raw path loads its programs from code word 0, over all of NVK's, so no NVK macro runs in a raw channel after its
// context init, and the raw path's scratch words take NVK's. Two of NVK's keep their meaning: FECS's mailbox, which NVK's
// SET_PRIV_REG shares with the firmware, and the root table's address, which the context init writes for NVK's
// SELECT_CB0 and raw_mme.h's install() writes again for the patch macro. kits/SwitchGM20BEmit/test/macro_slots_test.py
// checks the NVK numbers against nvk_mme.h and context.tsv.
#ifndef SWITCH_GM20B_MACRO_SLOTS_H
#define SWITCH_GM20B_MACRO_SLOTS_H

enum {
    SWITCH_GM20B_MACRO_NVK_CLEAR = 2,            // NVK_MME_CLEAR
    SWITCH_GM20B_MACRO_NVK_SET_ANTI_ALIAS = 10,  // NVK_MME_SET_ANTI_ALIAS: its argument a pass's sample count
    SWITCH_GM20B_MACRO_DRAWS_FIRST = 11,         // NVK_MME_DRAW
    SWITCH_GM20B_MACRO_DRAWS_LAST = 16,          // NVK_MME_DRAW_INDEXED_INDIRECT_COUNT
    SWITCH_GM20B_MACRO_SLOTS = 128,              // CALL_MME_MACRO(j), LOAD_MME_START_ADDRESS_RAM's entries
    SWITCH_GM20B_MACRO_CODE_WORDS = 2048,        // LOAD_MME_INSTRUCTION_RAM's words

    SWITCH_GM20B_MACRO_DRAW = SWITCH_GM20B_MACRO_DRAWS_FIRST,
    SWITCH_GM20B_MACRO_DRAW_INDEXED,
    SWITCH_GM20B_MACRO_INDIRECT,
    SWITCH_GM20B_MACRO_INDEXED_INDIRECT,
    SWITCH_GM20B_MACRO_DRAW_PARAMS,
    SWITCH_GM20B_MACRO_DRAW_INDEXED_PARAMS,
    SWITCH_GM20B_MACRO_VCM_CONSTANTS,
    // indirect command buffers': an execute's range, a gated draw of each kind, a dispatch command's gate, a command's
    // bound buffers (x1-calls3's kIcbBind), a patch command's control-stage words and draw
    SWITCH_GM20B_MACRO_ICB_RANGE,
    SWITCH_GM20B_MACRO_ICB_DRAW,
    SWITCH_GM20B_MACRO_ICB_DRAW_INDEXED,
    SWITCH_GM20B_MACRO_ICB_GATE,
    SWITCH_GM20B_MACRO_ICB_BIND,
    SWITCH_GM20B_MACRO_ICB_PATCHES,
    // a draw under vertex amplification, of vertices and of indices
    SWITCH_GM20B_MACRO_AMPLIFY,
    SWITCH_GM20B_MACRO_AMPLIFY_INDEXED,
    // a layered pass's clear of its layers (NVK's nvk_mme_clear without a view mask)
    SWITCH_GM20B_MACRO_CLEAR_LAYERS,
    SWITCH_GM20B_MACRO_RAW_END,  // one past the raw path's last slot
};

enum {
    SWITCH_GM20B_SCRATCH_WORDS = 128,   // SET_MME_SHADOW_SCRATCH(i): MME_FERMI_SCRATCH_COUNT
    SWITCH_GM20B_SCRATCH_FECS_END = 3,  // NVK_MME_SCRATCH_FALCON_2 + 1
    SWITCH_GM20B_SCRATCH_ROOT_HI = 26,  // NVK_MME_SCRATCH_CB0_ADDR_HI: the root table's address, high then low

    // an indirect command buffer execute's range: location, length
    SWITCH_GM20B_SCRATCH_ICB_LOCATION = SWITCH_GM20B_SCRATCH_FECS_END,
    SWITCH_GM20B_SCRATCH_ICB_LENGTH,
    // a patch execute's layout of its inherited state: set 0's address (high, low), each control-stage buffer's set-0
    // byte offset, the root byte offset of the control stage's first push word
    SWITCH_GM20B_SCRATCH_ICB_SET_HI,
    SWITCH_GM20B_SCRATCH_ICB_ENTRY = SWITCH_GM20B_SCRATCH_ICB_SET_HI + 2,
    SWITCH_GM20B_SCRATCH_ICB_CONTROL = SWITCH_GM20B_SCRATCH_ICB_ENTRY + 3,
    // vertex amplification's, which the amplify macros read (the raw path's RenderEncoder writes them where the bound
    // state, the count or the mapping changes): the state's root byte offsets of its AmplificationCount, AmplificationId
    // and AmplificationViewportOffset words (0: none), its layer (SWITCH_GM20B_AMP_LAYER_*, else the root byte offset of
    // its AmplificationLayerOffset word), whether its fetches need a pixel barrier between two amplifications (its
    // barrier form's), then the count and each amplification's mapping word (SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT)
    SWITCH_GM20B_SCRATCH_AMP_COUNT_WORD,
    SWITCH_GM20B_SCRATCH_AMP_ID_WORD,
    SWITCH_GM20B_SCRATCH_AMP_VIEWPORT_WORD,
    SWITCH_GM20B_SCRATCH_AMP_LAYER,
    SWITCH_GM20B_SCRATCH_AMP_BARRIER,
    SWITCH_GM20B_SCRATCH_AMP_COUNT,
    SWITCH_GM20B_SCRATCH_AMP_MAPS,
    SWITCH_GM20B_SCRATCH_AMP_END = SWITCH_GM20B_SCRATCH_AMP_MAPS + 8,
    // a command's bound buffers: ICB_BUFFERS + 3 x index (address low, address high, size), index < 31; the address-high
    // word holds the buffer's bind stride above its address's bits (SWITCH_GM20B_ICB_STRIDE_SHIFT). They take the top of
    // the scratch, where the context init writes only zeros, so a slot no execute has written yet reads as no buffer.
    SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS = 31,
    SWITCH_GM20B_SCRATCH_ICB_BUFFERS = SWITCH_GM20B_SCRATCH_WORDS - 3 * SWITCH_GM20B_SCRATCH_ICB_BUFFER_SLOTS,
    SWITCH_GM20B_SCRATCH_ICB_BUFFERS_END = SWITCH_GM20B_SCRATCH_WORDS,
};

// What the patch macro's words name: a layout entry of no buffer, the constant buffer sizes it selects (NVK's
// descriptor set slot and root descriptor table), its control-stage buffers and push words
enum {
    SWITCH_GM20B_ICB_NO_ENTRY = 0x3fff,
    SWITCH_GM20B_ICB_SET_BYTES = 0x100,
    SWITCH_GM20B_ICB_ROOT_BYTES = 0x800,
    SWITCH_GM20B_ICB_PATCH_BUFFERS = 3,
    SWITCH_GM20B_ICB_PATCH_CONTROL_WORDS = 6,
    // X1's GPU addresses lie below 2^40 (TegraGM20BGpu.h TGM_VA_LIMIT): an address-high word's bits from here are free
    SWITCH_GM20B_ICB_STRIDE_SHIFT = 8,
};

// SWITCH_GM20B_MACRO_ICB_BIND's call data: the descriptors' count, then each descriptor's address (high, low), its
// buffer's first scratch word, the kernel's window of it and its residual root word's address (high, low); then the
// strides' count, then each StageInStride root word's address (high, low) and its buffer's first scratch word. The
// macro reads exactly these words: a call that sends fewer leaves the MME waiting on the GPU for the rest.
enum {
    SWITCH_GM20B_ICB_BIND_DESCRIPTOR_WORDS = 6,
    SWITCH_GM20B_ICB_BIND_STRIDE_WORDS = 3,
};

#define SWITCH_GM20B_ICB_BIND_WORDS(descriptors, strides) \
    (2 + SWITCH_GM20B_ICB_BIND_DESCRIPTOR_WORDS * (descriptors) + SWITCH_GM20B_ICB_BIND_STRIDE_WORDS * (strides))

// SWITCH_GM20B_SCRATCH_AMP_LAYER's modes besides a word: the draws render the pass's first layer, or SET_RT_LAYER takes
// each amplification's render target offset (V_SELECTS_LAYER). No root word lies at either byte offset. A mapping word
// holds an amplification's render target offset in its low half and its viewport offset in its high half: Metal bounds
// them by the render target's 2048 layers and the 16 viewports.
enum {
    SWITCH_GM20B_AMP_LAYER_FIRST = 0,
    SWITCH_GM20B_AMP_LAYER_RT = 1,
    SWITCH_GM20B_AMP_MAX = 8,  // Metal's largest amplification count
    SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT = 16,
};

#ifdef __cplusplus
#define SWITCH_GM20B_SLOTS_ASSERT(c, m) static_assert(c, m)
#else
#define SWITCH_GM20B_SLOTS_ASSERT(c, m) _Static_assert(c, m)
#endif

SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_MACRO_VCM_CONSTANTS > SWITCH_GM20B_MACRO_DRAWS_LAST,
                          "every raw macro but NVK's draw slots' takes a slot past them");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_MACRO_RAW_END <= SWITCH_GM20B_MACRO_SLOTS, "every raw slot is a CALL_MME_MACRO");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_SCRATCH_ICB_LOCATION >= SWITCH_GM20B_SCRATCH_FECS_END &&
                              SWITCH_GM20B_SCRATCH_AMP_END <= SWITCH_GM20B_SCRATCH_ROOT_HI,
                          "the raw path's words below the root table's address leave FECS's mailbox alone");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_SCRATCH_ICB_BUFFERS >= SWITCH_GM20B_SCRATCH_ROOT_HI + 2,
                          "the inherited buffers lie past the root table's address");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_ICB_SET_BYTES % 256 == 0 && SWITCH_GM20B_ICB_ROOT_BYTES % 256 == 0,
                          "the patch macro binds whole 256-byte constant buffers, as NVK does before Turing");
SWITCH_GM20B_SLOTS_ASSERT(SWITCH_GM20B_SCRATCH_AMP_END - SWITCH_GM20B_SCRATCH_AMP_MAPS == SWITCH_GM20B_AMP_MAX,
                          "a mapping word for each amplification");

#undef SWITCH_GM20B_SLOTS_ASSERT

#endif
