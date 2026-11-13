/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

/*
 * The raw path's macros (ABI v6, at the slots and shadow scratch words kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h
 * names: R1 the first argument, constant buffer 0 left selected), built with Mesa's MME builder. The draw macros take
 * the slots NVK's draw macros take, so a push profile that counts NVK's draw calls counts the raw path's too. The
 * instance loops are NVK's pre-Pascal nvk_mme_build_draw_loop and
 * nvk_mme_build_draw_indexed_loop (src/nouveau/vulkan/nvk_cmd_draw.c, commit 77452b9092cc), with their Turing and
 * Pascal branches dropped; the calling convention around them is raw's (drivers/TegraGM20BMetal/raw/runtime/raw_mme.h),
 * and a zero instance count skips the loop, which on Fermi's MME tests its condition at the end.
 *
 * Writes the macros' words as a C header: gen_macros <out.h>.
 */

#include <stdio.h>
#include <stdlib.h>

#include "drf.h"
#include "mme_builder.h"
#include "nv_push_cl9097.h"
#include "nv_push_clb197.h"
#include "nv_push_clc597.h"

#include "../../SwitchGM20B/SwitchGM20BTegra.h"
#include "../SwitchGM20BMacroSlots.h"

/* LAUNCH_DMA of an inline-to-memory write into pitch memory, as NVK's vkCmdUpdateBuffer sends it */
#define SWITCH_GM20B_I2M_LAUNCH \
    (NVB197_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH | NVB197_LAUNCH_DMA_SYSMEMBAR_DISABLE_TRUE << 6)

/* mme_tu104.c's disassembler names Turing's methods; this tool builds Fermi macros and prints none */
const char *P_PARSE_NVC597_MTHD(uint16_t idx)
{
    (void)idx;
    return "?";
}

static void nvk_mme_build_draw_loop(struct mme_builder *b, struct mme_value instance_count, struct mme_value first,
                                    struct mme_value count, uint16_t first_mthd)
{
    uint32_t begin_initial_value;
    V_NVB197_BEGIN(begin_initial_value, {
                                            .primitive_id = PRIMITIVE_ID_FIRST,
                                            .instance_id = INSTANCE_ID_FIRST,
                                            .split_mode = SPLIT_MODE_NORMAL_BEGIN_NORMAL_END,
                                        });
    struct mme_value begin = mme_mov(b, mme_imm(begin_initial_value));

    mme_start_loop(b, instance_count);

    mme_mthd(b, NV9097_BEGIN);
    mme_emit(b, begin);

    mme_mthd(b, first_mthd);
    mme_emit(b, first);
    mme_emit(b, count);

    mme_mthd(b, NV9097_END);
    mme_emit(b, mme_zero());

    mme_set_field_enum(b, begin, NV9097_BEGIN_INSTANCE_ID, SUBSEQUENT);
    mme_end_loop(b);

    mme_free_reg(b, begin);
}

static void build_direct(struct mme_builder *b, uint16_t first_mthd)
{
    struct mme_value instances = mme_load(b);
    struct mme_value first = mme_load(b);
    struct mme_value count = mme_load(b);
    mme_if(b, ine, instances, mme_zero())
    {
        nvk_mme_build_draw_loop(b, instances, first, count, first_mthd);
    }
}

/* A draw that writes its draw parameters first: the root table's base vertex and instance and the base registers, as
 * nvk_mme_build_set_draw_params writes them before Turing. Arguments: instances, first vertex or index, count, then the
 * base instance (a draw of vertices: its first vertex is the root's base vertex, the base registers 0), or the base
 * vertex and the base instance (an indexed draw). */
static void build_direct_params(struct mme_builder *b, bool indexed)
{
    struct mme_value instances = mme_load(b);
    struct mme_value first = mme_load(b);
    struct mme_value count = mme_load(b);
    struct mme_value base = mme_load(b);
    struct mme_value base_instance = indexed ? mme_load(b) : base;
    struct mme_value base_vertex = indexed ? base : mme_zero();

    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
    mme_emit(b, mme_zero());
    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
    mme_emit(b, indexed ? base_vertex : first);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_VERTEX_INDEX);
    mme_emit(b, base_vertex);
    mme_mthd(b, NVB197_SET_VERTEX_ID_BASE);
    mme_emit(b, base_vertex);

    mme_free_reg(b, base);
    if (indexed)
        mme_free_reg(b, base_instance);

    mme_if(b, ine, instances, mme_zero())
    {
        nvk_mme_build_draw_loop(b, instances, first, count,
                                indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START);
    }
}

/* Metal's indirect arguments after their count and instances, in Vulkan's order: first vertex or index, (vertex offset,)
 * first instance. The root table's base vertex and instance, then the base registers, then the draws. */
static void build_indirect_body(struct mme_builder *b, bool indexed, struct mme_value count, struct mme_value instances)
{
    struct mme_value first = mme_load(b);
    struct mme_value base = mme_load(b);
    struct mme_value base_instance = indexed ? mme_load(b) : base;
    struct mme_value base_vertex = indexed ? base : mme_zero();

    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
    mme_emit(b, mme_zero());
    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
    mme_emit(b, indexed ? base_vertex : first);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_VERTEX_INDEX);
    mme_emit(b, base_vertex);
    mme_mthd(b, NVB197_SET_VERTEX_ID_BASE);
    mme_emit(b, base_vertex);

    /* Fermi's MME has seven registers: the loop needs three more than the arguments it keeps */
    mme_free_reg(b, base);
    if (indexed)
        mme_free_reg(b, base_instance);

    mme_if(b, ine, count, mme_zero())
    {
        mme_if(b, ine, instances, mme_zero())
        {
            nvk_mme_build_draw_loop(b, instances, first, count,
                                    indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START);
        }
    }
}

#define AMP_SCRATCH(word) NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_AMP_##word)

/* The root word at the byte offset a scratch word names, where it names one: `value` */
static void amplification_word(struct mme_builder *b, struct mme_value offset, struct mme_value value)
{
    mme_if(b, ine, offset, mme_zero())
    {
        mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
        mme_emit(b, offset);
        mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
        mme_emit(b, value);
    }
}

/* Amplification `a`'s render target offset (shift 0) or viewport offset (SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT), from
 * its mapping word */
static struct mme_value amplification_map(struct mme_builder *b, struct mme_value a, uint16_t shift)
{
    struct mme_value map = mme_state_arr(b, AMP_SCRATCH(MAPS), a);
    mme_merge_to(b, map, mme_zero(), map, 0, 16, shift);
    return map;
}

/* Amplification `a`'s words: a pixel barrier after the one before where the state's fetches need it, its id, its
 * viewport offset, and its render target offset as the state's layer word or as SET_RT_LAYER's layer */
static void amplification_words(struct mme_builder *b, struct mme_value a)
{
    struct mme_value word = mme_state(b, AMP_SCRATCH(BARRIER));
    mme_if(b, ine, word, mme_zero())
    {
        mme_if(b, ine, a, mme_zero())
        {
            mme_mthd(b, NVB197_PIXEL_SHADER_BARRIER);
            mme_emit(b, mme_imm(NVB197_PIXEL_SHADER_BARRIER_SYSMEMBAR_ENABLE_FALSE));
            mme_mthd(b, SWITCH_NVB197_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED);
            mme_emit(b, mme_zero());
        }
    }
    mme_state_to(b, word, AMP_SCRATCH(ID_WORD));
    amplification_word(b, word, a);
    mme_state_to(b, word, AMP_SCRATCH(VIEWPORT_WORD));
    mme_if(b, ine, word, mme_zero())
    {
        struct mme_value offset = amplification_map(b, a, SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT);
        mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
        mme_emit(b, word);
        mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
        mme_emit(b, offset);
        mme_free_reg(b, offset);
    }
    mme_state_to(b, word, AMP_SCRATCH(LAYER));
    mme_if(b, ieq, word, mme_imm(SWITCH_GM20B_AMP_LAYER_RT))
    {
        struct mme_value layer = amplification_map(b, a, 0);
        mme_mthd(b, NVB197_SET_RT_LAYER);
        mme_emit(b, layer);
        mme_free_reg(b, layer);
    }
    mme_if(b, ine, word, mme_imm(SWITCH_GM20B_AMP_LAYER_RT))
    {
        mme_if(b, ine, word, mme_zero())
        {
            struct mme_value offset = amplification_map(b, a, 0);
            mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
            mme_emit(b, word);
            mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
            mme_emit(b, offset);
            mme_free_reg(b, offset);
        }
    }
    mme_free_reg(b, word);
}

/* A draw under vertex amplification as one call, as NVK's pre-Turing multiview draws one (nvk_mme_build_draw's view
 * loop): its words in Vulkan's indirect order (count, instances, first, an indexed draw's vertex offset, the first
 * instance), the call's data or a GPFIFO continuation over Metal's indirect arguments. The draw parameters and the
 * state's AmplificationCount word once, then each amplification in order, its words and then the draw, so that every
 * primitive of one amplification is drawn before the next's, as Metal draws them. The count, the state's word offsets and
 * the mappings are the scratch words; a count of 0 draws nothing. SET_RT_LAYER is left at the first layer. */
static void build_amplify(struct mme_builder *b, bool indexed)
{
    struct mme_value count = mme_load(b);
    struct mme_value instances = mme_load(b);
    struct mme_value first = mme_load(b);
    struct mme_value base = mme_load(b);
    struct mme_value base_instance = indexed ? mme_load(b) : base;
    struct mme_value base_vertex = indexed ? base : mme_zero();

    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
    mme_emit(b, mme_zero());
    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
    mme_emit(b, indexed ? base_vertex : first);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX);
    mme_emit(b, base_instance);
    mme_mthd(b, NVB197_SET_GLOBAL_BASE_VERTEX_INDEX);
    mme_emit(b, base_vertex);
    mme_mthd(b, NVB197_SET_VERTEX_ID_BASE);
    mme_emit(b, base_vertex);
    mme_free_reg(b, base);
    if (indexed)
        mme_free_reg(b, base_instance);

    struct mme_value word = mme_state(b, AMP_SCRATCH(COUNT_WORD));
    struct mme_value amplifications = mme_state(b, AMP_SCRATCH(COUNT));
    amplification_word(b, word, amplifications);
    mme_free_reg(b, word);

    /* Fermi's MME tests a loop's condition at its end: each loop runs only where its count is not 0 */
    mme_if(b, ine, count, mme_zero())
    {
        mme_if(b, ine, instances, mme_zero())
        {
            mme_if(b, ine, amplifications, mme_zero())
            {
                mme_while(b, ine, amplifications, mme_zero())
                {
                    struct mme_value a = mme_state(b, AMP_SCRATCH(COUNT));
                    mme_sub_to(b, a, a, amplifications);
                    amplification_words(b, a);
                    mme_free_reg(b, a);
                    nvk_mme_build_draw_loop(b, instances, first, count,
                                            indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START);
                    mme_sub_to(b, amplifications, amplifications, mme_imm(1));
                }
                word = mme_state(b, AMP_SCRATCH(LAYER));
                mme_if(b, ieq, word, mme_imm(SWITCH_GM20B_AMP_LAYER_RT))
                {
                    mme_mthd(b, NVB197_SET_RT_LAYER);
                    mme_emit(b, mme_zero());
                }
                mme_free_reg(b, word);
            }
        }
    }
}

static void build_indirect(struct mme_builder *b, bool indexed)
{
    struct mme_value count = mme_load(b);
    struct mme_value instances = mme_load(b);
    build_indirect_body(b, indexed, count, instances);
}

/* Core Animation's colour matrix blend (vcm) on the iterated blend unit, fetch rung 3
 * (drivers/TegraGM20BMetal/raw/pack/iterated/programs_long.py vcm): the program's words, whose 21-word head and 5-word
 * tail its three forms share. The form is the matrix uniform's clamp pair's: none (p3 <= 0), clip (p4 <= 0), scale. */
static const uint32_t vcm_head[] = {
    0x3200d799, 0x30000e21, 0x0000c201, 0x31009029, 0x1000de01, 0x31809029, 0x100dde41,
    0x32009029, 0x100dde81, 0x100dd6c1, 0x1068a129, 0x3100d941, 0x318fd941, 0x320fd941,
    0x310fd781, 0x31e8c1a9, 0x30000e19, 0x20001e11, 0x1000ba01, 0x10003a01, 0x20003c01,
};
static const uint32_t vcm_clamp_head[] = {0x3200dd99, 0x30000e21, 0x1000ca01};
static const uint32_t vcm_clip[] = {0x3180d1e9, 0x1000ca19, 0x3100d1e9, 0x1000ca11, 0x1000ba01};
static const uint32_t vcm_scale[] = {
    0x3080a029, 0x3080ae19, 0x0080ae19, 0xb16891f1, 0x00000826, 0x3100d1ee, 0x0000c806, 0x10009a06, 0x1000ba01,
};
static const uint32_t vcm_tail[] = {0x30581031, 0x000d5e01, 0x30581031, 0x128e6e01, 0x00009029};

#define VCM_GUARD       0x068e /* half 1.0e-4: QuartzCore's max(Da, e) */
#define HALF_MINUS_HALF 0xb800

static void emit_words(struct mme_builder *b, const uint32_t *words, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        mme_mthd(b, NVB197_LOAD_ITERATED_BLEND_INSTRUCTION);
        mme_emit(b, mme_imm(words[i]));
    }
}

static void emit_constant(struct mme_builder *b, uint32_t j, uint32_t lane, struct mme_value half)
{
    mme_mthd(b, NVB197_SET_ITERATED_BLEND_CONSTANT_RED(j) + 4 * lane);
    mme_emit(b, half);
}

/* 1 where a half's bits are above zero (an infinity too, not a NaN), else 0: Fermi's MME compares for equality only */
static struct mme_value positive_half(struct mme_builder *b, struct mme_value h)
{
    struct mme_value out = mme_mov(b, mme_zero());
    struct mme_value t = mme_and(b, h, mme_imm(0x8000));
    mme_if(b, ieq, t, mme_zero())
    {
        mme_if(b, ine, h, mme_zero())
        {
            mme_and_to(b, t, h, mme_imm(0x7c00));
            mme_if(b, ine, t, mme_imm(0x7c00))
            {
                mme_mov_to(b, out, mme_imm(1));
            }
            mme_if(b, ieq, t, mme_imm(0x7c00))
            {
                mme_and_to(b, t, h, mme_imm(0x3ff));
                mme_if(b, ieq, t, mme_zero())
                {
                    mme_mov_to(b, out, mme_imm(1));
                }
            }
        }
    }
    mme_free_reg(b, t);
    return out;
}

static struct mme_value high_half(struct mme_builder *b, struct mme_value word)
{
    return mme_merge(b, mme_zero(), word, 0, 16, 16);
}

/* Its 11 arguments are the colour matrix uniform's words, QuartzCore's five half4 rows M0..M4 then the clamp pair
 * (p3, p4), which a non-prefetched GPFIFO entry over the uniform buffer feeds it: the constants become
 * K0..K4 = the rows' rgb, K5 = (M0.a, M1.a, M2.a), K6 = (M3.a, M4.a, e), K7 = (p3, -0.5, 0), then the program the
 * clamp pair picks is loaded and enabled with its alpha. No CPU reads or writes a uniform for it. */
static void build_vcm_constants(struct mme_builder *b, bool unused)
{
    (void)unused;
    static const uint8_t alpha_slot[5][2] = {{5, 0}, {5, 1}, {5, 2}, {6, 0}, {6, 1}};
    for (uint32_t row = 0; row < 5; row++) {
        struct mme_value rg = mme_load(b);
        struct mme_value half = mme_and(b, rg, mme_imm(0xffff));
        emit_constant(b, row, 0, half);
        mme_free_reg(b, half);
        half = high_half(b, rg);
        emit_constant(b, row, 1, half);
        mme_free_reg(b, half);
        mme_free_reg(b, rg);
        struct mme_value ba = mme_load(b);
        half = mme_and(b, ba, mme_imm(0xffff));
        emit_constant(b, row, 2, half);
        mme_free_reg(b, half);
        half = high_half(b, ba);
        emit_constant(b, alpha_slot[row][0], alpha_slot[row][1], half);
        mme_free_reg(b, half);
        mme_free_reg(b, ba);
    }
    emit_constant(b, 6, 2, mme_imm(VCM_GUARD));
    struct mme_value clamp = mme_load(b);
    struct mme_value p3 = mme_and(b, clamp, mme_imm(0xffff));
    emit_constant(b, 7, 0, p3);
    emit_constant(b, 7, 1, mme_imm(HALF_MINUS_HALF));
    emit_constant(b, 7, 2, mme_zero());
    struct mme_value clips = positive_half(b, p3);
    mme_free_reg(b, p3);
    struct mme_value p4 = high_half(b, clamp);
    mme_free_reg(b, clamp);
    struct mme_value scales = positive_half(b, p4);
    mme_free_reg(b, p4);

    mme_mthd(b, NVB197_LOAD_ITERATED_BLEND_INSTRUCTION_POINTER);
    mme_emit(b, mme_zero());
    emit_words(b, vcm_head, ARRAY_SIZE(vcm_head));
    struct mme_value passes = mme_mov(b, mme_imm(ARRAY_SIZE(vcm_head) + ARRAY_SIZE(vcm_tail)));
    mme_if(b, ine, clips, mme_zero())
    {
        emit_words(b, vcm_clamp_head, ARRAY_SIZE(vcm_clamp_head));
        mme_if(b, ieq, scales, mme_zero())
        {
            emit_words(b, vcm_clip, ARRAY_SIZE(vcm_clip));
            mme_add_to(b, passes, passes, mme_imm(ARRAY_SIZE(vcm_clamp_head) + ARRAY_SIZE(vcm_clip)));
        }
        mme_if(b, ine, scales, mme_zero())
        {
            emit_words(b, vcm_scale, ARRAY_SIZE(vcm_scale));
            mme_add_to(b, passes, passes, mme_imm(ARRAY_SIZE(vcm_clamp_head) + ARRAY_SIZE(vcm_scale)));
        }
    }
    emit_words(b, vcm_tail, ARRAY_SIZE(vcm_tail));
    mme_mthd(b, NVB197_SET_ITERATED_BLEND_PASS);
    mme_emit(b, passes);
    mme_mthd(b, NVB197_SET_ITERATED_BLEND);
    mme_emit(b, mme_imm(3));
}

/* An indirect command buffer's range, as an MTLIndirectCommandBufferExecutionRange (location, length), into the shadow
 * scratch words the gated draws read */
static void build_icb_range(struct mme_builder *b, bool unused)
{
    (void)unused;
    struct mme_value location = mme_load(b);
    struct mme_value length = mme_load(b);
    mme_mthd(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LOCATION));
    mme_emit(b, location);
    mme_emit(b, length);
}

/* All ones where the command index loaded first is inside the range in scratch (index - location unsigned below length),
 * else zero: the borrow of (index - location) - length. Fermi's MME compares for equality alone. */
static struct mme_value icb_inside(struct mme_builder *b)
{
    struct mme_value index = mme_load(b);
    struct mme_value location = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LOCATION));
    mme_sub_to(b, index, index, location);
    mme_free_reg(b, location);
    struct mme_value length = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_LENGTH));
    struct mme_value64 below = mme_sub64(b, mme_value64(index, mme_zero()), mme_value64(length, mme_zero()));
    mme_free_reg(b, index);
    mme_free_reg(b, length);
    mme_free_reg(b, below.lo);
    return below.hi;
}

/* An indirect command: its index in its buffer, then an indirect draw's arguments. Outside the range its instance count is
 * zero, so it loads its arguments and draws nothing. */
static void build_icb_draw(struct mme_builder *b, bool indexed)
{
    struct mme_value inside = icb_inside(b);
    struct mme_value count = mme_load(b);
    struct mme_value instances = mme_load(b);
    mme_and_to(b, instances, instances, inside);
    mme_free_reg(b, inside);
    build_indirect_body(b, indexed, count, instances);
}

/* The constant buffer at the two scratch words' address, `bytes` long, selected for the loads after it */
static void select_scratch_cbuf(struct mme_builder *b, uint32_t hi, uint32_t bytes)
{
    mme_mthd(b, NVB197_SET_CONSTANT_BUFFER_SELECTOR_A);
    mme_emit(b, mme_imm(bytes));
    struct mme_value word = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(hi));
    mme_emit(b, word);
    mme_free_reg(b, word);
    word = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(hi + 1));
    mme_emit(b, word);
    mme_free_reg(b, word);
}

/* An indirect command of patches: its index, patch count and instances, then each control-stage buffer's set-0 descriptor
 * (base low, base high, range), the control stage's six push words, the first patch and the base instance. The descriptors
 * go into the inherited state's set 0 and the push words into the root table as constant buffer loads, which the GPU
 * orders with the draws around them (each draw reads the words loaded before it), then the root table is selected again
 * and the patches drawn as an indirect draw of vertices, nothing outside the range. Every command loads its words, inside
 * the range or not: the next command inside it loads its own before it draws. */
static void build_icb_patches(struct mme_builder *b, bool unused)
{
    (void)unused;
    struct mme_value inside = icb_inside(b);
    struct mme_value count = mme_load(b);
    struct mme_value instances = mme_load(b);
    mme_and_to(b, instances, instances, inside);
    mme_free_reg(b, inside);
    select_scratch_cbuf(b, SWITCH_GM20B_SCRATCH_ICB_SET_HI, SWITCH_GM20B_ICB_SET_BYTES);
    for (uint32_t k = 0; k < SWITCH_GM20B_ICB_PATCH_BUFFERS; k++) {
        struct mme_value entry = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_ENTRY + k));
        struct mme_value lo = mme_load(b);
        struct mme_value hi = mme_load(b);
        struct mme_value range = mme_load(b);
        mme_if(b, ine, entry, mme_imm(SWITCH_GM20B_ICB_NO_ENTRY))
        {
            mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
            mme_emit(b, entry);
            mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
            mme_emit(b, lo);
            mme_emit(b, hi);
            mme_emit(b, range);
            mme_emit(b, mme_zero());
        }
        mme_free_reg(b, entry);
        mme_free_reg(b, lo);
        mme_free_reg(b, hi);
        mme_free_reg(b, range);
    }
    select_scratch_cbuf(b, SWITCH_GM20B_SCRATCH_ROOT_HI, SWITCH_GM20B_ICB_ROOT_BYTES);
    struct mme_value control = mme_state(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_CONTROL));
    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER_OFFSET);
    mme_emit(b, control);
    mme_free_reg(b, control);
    mme_mthd(b, NVB197_LOAD_CONSTANT_BUFFER(0));
    for (uint32_t k = 0; k < SWITCH_GM20B_ICB_PATCH_CONTROL_WORDS; k++) {
        struct mme_value word = mme_load(b);
        mme_emit(b, word);
        mme_free_reg(b, word);
    }
    build_indirect_body(b, false, count, instances);
}

/* An indirect command buffer's dispatch: its index. The global render enable, which the compute class's launches obey
 * (NVK's conditional rendering of dispatches), is TRUE inside the range in scratch and FALSE outside it, from the same
 * borrow as build_icb_draw's; the execute sets it TRUE again after its commands. */
static void build_icb_gate(struct mme_builder *b, bool unused)
{
    (void)unused;
    struct mme_value inside = icb_inside(b);
    mme_and_to(b, inside, inside, mme_imm(NVB197_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE));
    mme_mthd(b, NVB197_SET_GLOBAL_RENDER_ENABLE_C);
    mme_emit(b, inside);
    mme_free_reg(b, inside);
}

/* NVK's nvk_mme_clear without a view mask: CLEAR_SURFACE's payload once a layer for `count` layers, its RT_ARRAY_INDEX one
 * higher each time (a layered pass's clear; the count is at least one) */
static void build_clear_layers(struct mme_builder *b, bool unused)
{
    (void)unused;
    struct mme_value payload = mme_load(b);
    struct mme_value count = mme_load(b);
    mme_loop(b, count)
    {
        mme_mthd(b, NVB197_CLEAR_SURFACE);
        mme_emit(b, payload);
        mme_add_to(b, payload, payload, mme_imm(1u << DRF_LO(NVB197_CLEAR_SURFACE_RT_ARRAY_INDEX)));
    }
    mme_free_reg(b, count);
    mme_free_reg(b, payload);
}

static void build_i2m_head(struct mme_builder *b, struct mme_value hi, struct mme_value lo, uint32_t bytes)
{
    mme_mthd(b, NVB197_OFFSET_OUT_UPPER);
    mme_emit(b, hi);
    mme_emit(b, lo);
    mme_mthd(b, NVB197_LINE_LENGTH_IN);
    mme_emit(b, mme_imm(bytes));
    mme_emit(b, mme_imm(1));
    mme_mthd(b, NVB197_LAUNCH_DMA);
    mme_emit(b, mme_imm(SWITCH_GM20B_I2M_LAUNCH));
}

static void build_inline_word(struct mme_builder *b, struct mme_value word)
{
    mme_mthd(b, NVB197_LOAD_INLINE_DATA);
    mme_emit(b, word);
}

/* An execute's inherited buffers into the set-0 descriptors of an indirect command buffer's dispatches, on the GPU: the
 * count of descriptors, then for each the descriptor's address (high, low word), its buffer's first scratch word, the
 * kernel's window of it in bytes (0 the whole buffer) and the address of its root push word for the base's residual (high
 * word 0: none). The descriptor is raw_residual.h's storage buffer descriptor of the scratch's binding: the base aligned
 * down to 16 bytes, the range from it to the end of the buffer or of the window rounded up to 16 (an X1 buffer is far
 * below the 0xfffffff0 the CPU's clamps it to), 0 for no buffer. Then the count of a stage-in kernel's bind strides, and
 * for each the address of its StageInStride root word (high, low) and its buffer's first scratch word: the stride the
 * address-high scratch word holds above its address. Written with the 3D class's inline-to-memory methods. */
static void build_icb_bind(struct mme_builder *b, bool unused)
{
    (void)unused;
    struct mme_value count = mme_load(b);
    mme_if(b, ine, count, mme_zero())
    {
        /* the loop counts in a register of its own: Fermi's seven are all this body's */
        mme_start_loop(b, count);
        mme_free_reg(b, count);
        {
            struct mme_value hi = mme_load(b);
            struct mme_value lo = mme_load(b);
            build_i2m_head(b, hi, lo, 12);
            mme_free_reg(b, hi);
            mme_free_reg(b, lo);

            struct mme_value at = mme_load(b);
            struct mme_value base =
                mme_state_arr(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_BUFFERS), at);
            struct mme_value residual = mme_and(b, base, mme_imm(15));
            mme_and_not_to(b, base, base, mme_imm(15));
            build_inline_word(b, base);
            struct mme_value upper =
                mme_state_arr(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 1), at);
            mme_and_to(b, upper, upper, mme_imm((1u << SWITCH_GM20B_ICB_STRIDE_SHIFT) - 1));
            build_inline_word(b, upper);
            mme_or_to(b, base, base, upper);
            mme_free_reg(b, upper);

            struct mme_value end =
                mme_state_arr(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 2), at);
            mme_free_reg(b, at);
            mme_add_to(b, end, end, residual);
            mme_add_to(b, end, end, mme_imm(15));
            mme_and_not_to(b, end, end, mme_imm(15));
            struct mme_value window = mme_load(b);
            mme_if(b, ine, window, mme_zero())
            {
                mme_add_to(b, window, window, residual);
                mme_add_to(b, window, window, mme_imm(15));
                mme_and_not_to(b, window, window, mme_imm(15));
                /* the smaller of the two: the borrow of end - window is all ones where end is below */
                struct mme_value64 below = mme_sub64(b, mme_value64(end, mme_zero()), mme_value64(window, mme_zero()));
                mme_xor_to(b, below.lo, end, window);
                mme_and_to(b, below.lo, below.lo, below.hi);
                mme_xor_to(b, end, window, below.lo);
                mme_free_reg64(b, below);
            }
            mme_free_reg(b, window);
            mme_if(b, ieq, base, mme_zero())
            {
                mme_mov_to(b, end, mme_zero());
            }
            mme_free_reg(b, base);
            build_inline_word(b, end);
            mme_free_reg(b, end);

            hi = mme_load(b);
            lo = mme_load(b);
            mme_if(b, ine, hi, mme_zero())
            {
                build_i2m_head(b, hi, lo, 4);
                build_inline_word(b, residual);
            }
            mme_free_reg(b, hi);
            mme_free_reg(b, lo);
            mme_free_reg(b, residual);
        }
        mme_end_loop(b);
    }
    struct mme_value strides = mme_load(b);
    mme_if(b, ine, strides, mme_zero())
    {
        mme_start_loop(b, strides);
        mme_free_reg(b, strides);
        {
            struct mme_value hi = mme_load(b);
            struct mme_value lo = mme_load(b);
            build_i2m_head(b, hi, lo, 4);
            mme_free_reg(b, hi);
            mme_free_reg(b, lo);
            struct mme_value at = mme_load(b);
            struct mme_value stride =
                mme_state_arr(b, NVB197_SET_MME_SHADOW_SCRATCH(SWITCH_GM20B_SCRATCH_ICB_BUFFERS + 1), at);
            mme_free_reg(b, at);
            mme_srl_to(b, stride, stride, mme_imm(SWITCH_GM20B_ICB_STRIDE_SHIFT));
            build_inline_word(b, stride);
            mme_free_reg(b, stride);
        }
        mme_end_loop(b);
    }
}

static void write_macro(FILE *out, const char *name, void (*build)(struct mme_builder *, bool), bool indexed,
                        const struct nv_device_info *dev)
{
    struct mme_builder b;
    mme_builder_init(&b, dev);
    build(&b, indexed);
    size_t size;
    uint32_t *words = mme_builder_finish(&b, &size);
    fprintf(out, "\nstatic const uint32_t switch_gm20b_macro_%s[] = {", name);
    for (size_t i = 0; i < size / 4; i++)
        fprintf(out, "%s0x%08x,", i % 6 ? " " : "\n    ", words[i]);
    fprintf(out, "\n};\n");
    free(words);
}

static void draw(struct mme_builder *b, bool indexed)
{
    build_direct(b, indexed ? NVB197_SET_INDEX_BUFFER_F : NVB197_SET_VERTEX_ARRAY_START);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *out = fopen(argv[1], "w");
    if (!out)
        return 1;
    const struct nv_device_info dev = {
        .type = NV_DEVICE_TYPE_SOC,
        .chipset = 0x12b,
        .cls_eng3d = MAXWELL_B,
    };
    fprintf(out,
            "// Generated by kits/SwitchGM20BEmit/mme/switch_gm20b_macros.c with Mesa's MME builder: do not edit.\n"
            "// The slots and shadow scratch words they take: kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h\n"
            "#ifndef SWITCH_GM20B_MACROS_H\n#define SWITCH_GM20B_MACROS_H\n\n#include <stdint.h>\n"
            "\n// SWITCH_GM20B_MACRO_VCM_CONSTANTS's arguments: the colour matrix uniform's words\n"
            "#define SWITCH_GM20B_MACRO_VCM_ARGUMENTS 11u\n");
    write_macro(out, "draw", draw, false, &dev);
    write_macro(out, "draw_indexed", draw, true, &dev);
    write_macro(out, "indirect", build_indirect, false, &dev);
    write_macro(out, "indexed_indirect", build_indirect, true, &dev);
    write_macro(out, "draw_params", build_direct_params, false, &dev);
    write_macro(out, "draw_indexed_params", build_direct_params, true, &dev);
    write_macro(out, "vcm_constants", build_vcm_constants, false, &dev);
    write_macro(out, "icb_range", build_icb_range, false, &dev);
    write_macro(out, "icb_draw", build_icb_draw, false, &dev);
    write_macro(out, "icb_draw_indexed", build_icb_draw, true, &dev);
    write_macro(out, "icb_gate", build_icb_gate, false, &dev);
    write_macro(out, "icb_bind", build_icb_bind, false, &dev);
    write_macro(out, "icb_patches", build_icb_patches, false, &dev);
    write_macro(out, "amplify", build_amplify, false, &dev);
    write_macro(out, "amplify_indexed", build_amplify, true, &dev);
    write_macro(out, "clear_layers", build_clear_layers, false, &dev);
    fprintf(out, "\n#endif\n");
    return fclose(out) ? 1 : 0;
}
