// SPDX-License-Identifier: MIT
// The render enable and inline write follow Mesa NVK's conditional rendering and vkCmdUpdateBuffer (nvk_cmd_draw.c,
// nvk_cmd_copy.c), MIT.
#include "SwitchGM20BEmitPredicate.h"

#include "nv_push_clb06f.h"
#include "nv_push_clb0b5.h"
#include "nv_push_clb1c0.h"

void switch_gm20b_render_enable(struct nv_push *p, uint64_t va, uint32_t mode)
{
    assert(!(va & 15));
    P_MTHD(p, NVB1C0, SET_GLOBAL_RENDER_ENABLE_A);
    P_NVB1C0_SET_GLOBAL_RENDER_ENABLE_A(p, va >> 32);
    P_NVB1C0_SET_GLOBAL_RENDER_ENABLE_B(p, va & 0xffffffff);
    P_NVB1C0_SET_GLOBAL_RENDER_ENABLE_C(p, mode);
}

void switch_gm20b_render_enable_always(struct nv_push *p)
{
    P_MTHD(p, NVB1C0, SET_GLOBAL_RENDER_ENABLE_A);
    P_NVB1C0_SET_GLOBAL_RENDER_ENABLE_A(p, 0);
    P_NVB1C0_SET_GLOBAL_RENDER_ENABLE_B(p, 0);
    P_NVB1C0_SET_GLOBAL_RENDER_ENABLE_C(p, MODE_TRUE);
}

void switch_gm20b_compute_inline_word(struct nv_push *p, uint64_t va, uint32_t value)
{
    assert(!(va & 3));
    P_MTHD(p, NVB1C0, LINE_LENGTH_IN);
    P_NVB1C0_LINE_LENGTH_IN(p, 4);
    P_NVB1C0_LINE_COUNT(p, 1);
    P_NVB1C0_OFFSET_OUT_UPPER(p, va >> 32);
    P_NVB1C0_OFFSET_OUT(p, va & 0xffffffff);
    P_1INC(p, NVB1C0, LAUNCH_DMA);
    P_NVB1C0_LAUNCH_DMA(p, {
                               .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
                               .completion_type = COMPLETION_TYPE_FLUSH_ONLY,
                           });
    P_INLINE_DATA(p, value);
}

void switch_gm20b_compute_inline_words(struct nv_push *p, uint64_t va, const uint32_t *words, uint32_t count)
{
    assert(!(va & 3) && count && count < NV_PUSH_MAX_COUNT);
    P_MTHD(p, NVB1C0, LINE_LENGTH_IN);
    P_NVB1C0_LINE_LENGTH_IN(p, 4 * count);
    P_NVB1C0_LINE_COUNT(p, 1);
    P_NVB1C0_OFFSET_OUT_UPPER(p, va >> 32);
    P_NVB1C0_OFFSET_OUT(p, va & 0xffffffff);
    P_1INC(p, NVB1C0, LAUNCH_DMA);
    P_NVB1C0_LAUNCH_DMA(p, {
                               .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
                               .completion_type = COMPLETION_TYPE_FLUSH_ONLY,
                           });
    P_INLINE_ARRAY(p, words, (int)count);
}

void switch_gm20b_fetch_after_idle(struct nv_push *p)
{
    __push_immd(p, SUBC_NVB1C0, NVB06F_SET_REFERENCE, 0);
}

void switch_gm20b_compute_idle_flushed(struct nv_push *p)
{
    P_IMMD(p, NVB1C0, WAIT_FOR_IDLE, 0);
    P_IMMD(p, NVB1C0, INVALIDATE_SHADER_CACHES,
           {
               .data = DATA_TRUE,
               .flush_data = FLUSH_DATA_TRUE,
           });
}

void switch_gm20b_copy_word_reduce(struct nv_push *p, uint64_t src, uint64_t dst, uint32_t payload, uint32_t reduction,
                                   bool is_signed)
{
    assert(!(src & 3) && !(dst & 3));
    P_MTHD(p, NVB0B5, OFFSET_IN_UPPER);
    P_NVB0B5_OFFSET_IN_UPPER(p, src >> 32);
    P_NVB0B5_OFFSET_IN_LOWER(p, src & 0xffffffff);
    P_NVB0B5_OFFSET_OUT_UPPER(p, dst >> 32);
    P_NVB0B5_OFFSET_OUT_LOWER(p, dst & 0xffffffff);
    P_NVB0B5_PITCH_IN(p, 4);
    P_NVB0B5_PITCH_OUT(p, 4);
    P_NVB0B5_LINE_LENGTH_IN(p, 4);
    P_NVB0B5_LINE_COUNT(p, 1);
    P_MTHD(p, NVB0B5, SET_SEMAPHORE_A);
    P_NVB0B5_SET_SEMAPHORE_A(p, dst >> 32);
    P_NVB0B5_SET_SEMAPHORE_B(p, dst & 0xffffffff);
    P_NVB0B5_SET_SEMAPHORE_PAYLOAD(p, payload);
    P_IMMD(
        p, NVB0B5, LAUNCH_DMA,
        {
            .data_transfer_type = DATA_TRANSFER_TYPE_NON_PIPELINED,
            .flush_enable = FLUSH_ENABLE_TRUE,
            .semaphore_type = SEMAPHORE_TYPE_RELEASE_ONE_WORD_SEMAPHORE,
            .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
            .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
            .semaphore_reduction = reduction,
            .semaphore_reduction_sign = is_signed ? SEMAPHORE_REDUCTION_SIGN_SIGNED : SEMAPHORE_REDUCTION_SIGN_UNSIGNED,
            .semaphore_reduction_enable = SEMAPHORE_REDUCTION_ENABLE_TRUE,
        });
}
