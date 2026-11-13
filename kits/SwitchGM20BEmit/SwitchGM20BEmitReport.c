// SPDX-License-Identifier: MIT
// The counter's words follow Mesa NVK's occlusion queries (nvk_query_pool.c nvk_CmdBeginQueryIndexedEXT,
// nvk_CmdEndQueryIndexedEXT) and the copy engine's semaphore release NVK's event set (nvk_event.c), MIT.
#include "SwitchGM20BEmitReport.h"

#include "nv_push_clb0b5.h"
#include "nv_push_clb197.h"

void switch_gm20b_samples_begin(struct nv_push *p)
{
    P_IMMD(p, NVB197, CLEAR_REPORT_VALUE, TYPE_ZPASS_PIXEL_CNT);
    P_IMMD(p, NVB197, SET_ZPASS_PIXEL_COUNT, ENABLE_TRUE);
}

void switch_gm20b_samples_end(struct nv_push *p)
{
    P_IMMD(p, NVB197, SET_ZPASS_PIXEL_COUNT, ENABLE_FALSE);
}

void switch_gm20b_samples_report(struct nv_push *p, uint64_t va)
{
    assert(!(va & 15));
    P_MTHD(p, NVB197, SET_REPORT_SEMAPHORE_A);
    P_NVB197_SET_REPORT_SEMAPHORE_A(p, va >> 32);
    P_NVB197_SET_REPORT_SEMAPHORE_B(p, va & 0xffffffff);
    P_NVB197_SET_REPORT_SEMAPHORE_C(p, 0);
    P_NVB197_SET_REPORT_SEMAPHORE_D(p, {
                                           .operation = OPERATION_REPORT_ONLY,
                                           .pipeline_location = PIPELINE_LOCATION_ALL,
                                           .report = REPORT_ZPASS_PIXEL_CNT64,
                                           .structure_size = STRUCTURE_SIZE_FOUR_WORDS,
                                           .flush_disable = FLUSH_DISABLE_TRUE,
                                       });
}

void switch_gm20b_copy_bytes(struct nv_push *p, uint64_t src, uint64_t dst, uint32_t bytes, uint8_t data_transfer_type)
{
    assert(bytes && bytes < (1u << 15));
    P_MTHD(p, NVB0B5, OFFSET_IN_UPPER);
    P_NVB0B5_OFFSET_IN_UPPER(p, src >> 32);
    P_NVB0B5_OFFSET_IN_LOWER(p, src & 0xffffffff);
    P_NVB0B5_OFFSET_OUT_UPPER(p, dst >> 32);
    P_NVB0B5_OFFSET_OUT_LOWER(p, dst & 0xffffffff);
    P_NVB0B5_PITCH_IN(p, bytes);
    P_NVB0B5_PITCH_OUT(p, bytes);
    P_NVB0B5_LINE_LENGTH_IN(p, bytes);
    P_NVB0B5_LINE_COUNT(p, 1);
    P_IMMD(p, NVB0B5, LAUNCH_DMA,
           {
               .data_transfer_type = data_transfer_type,
               .flush_enable = FLUSH_ENABLE_TRUE,
               .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
               .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
           });
}

void switch_gm20b_copy_u64(struct nv_push *p, uint64_t dst, uint64_t value, uint8_t data_transfer_type)
{
    assert(!(dst & 7));
    P_MTHD(p, NVB0B5, SET_REMAP_CONST_A);
    P_NVB0B5_SET_REMAP_CONST_A(p, value & 0xffffffff);
    P_NVB0B5_SET_REMAP_CONST_B(p, value >> 32);
    P_NVB0B5_SET_REMAP_COMPONENTS(p, {
                                         .dst_x = DST_X_CONST_A,
                                         .dst_y = DST_Y_CONST_B,
                                         .dst_z = DST_Z_NO_WRITE,
                                         .dst_w = DST_W_NO_WRITE,
                                         .component_size = COMPONENT_SIZE_FOUR,
                                         .num_src_components = NUM_SRC_COMPONENTS_TWO,
                                         .num_dst_components = NUM_DST_COMPONENTS_TWO,
                                     });
    P_MTHD(p, NVB0B5, OFFSET_OUT_UPPER);
    P_NVB0B5_OFFSET_OUT_UPPER(p, dst >> 32);
    P_NVB0B5_OFFSET_OUT_LOWER(p, dst & 0xffffffff);
    P_IMMD(p, NVB0B5, LINE_LENGTH_IN, 1);
    P_IMMD(p, NVB0B5, LAUNCH_DMA,
           {
               .data_transfer_type = data_transfer_type,
               .flush_enable = FLUSH_ENABLE_TRUE,
               .src_memory_layout = SRC_MEMORY_LAYOUT_PITCH,
               .dst_memory_layout = DST_MEMORY_LAYOUT_PITCH,
               .remap_enable = REMAP_ENABLE_TRUE,
           });
}

void switch_gm20b_copy_predicate(struct nv_push *p, uint64_t va, uint32_t mode)
{
    P_MTHD(p, NVB0B5, SET_RENDER_ENABLE_A);
    P_NVB0B5_SET_RENDER_ENABLE_A(p, va >> 32);
    P_NVB0B5_SET_RENDER_ENABLE_B(p, va & 0xffffffff);
    P_NVB0B5_SET_RENDER_ENABLE_C(p, mode);
}

void switch_gm20b_copy_release(struct nv_push *p, uint64_t va, uint32_t payload)
{
    assert(!(va & 3));
    P_MTHD(p, NVB0B5, SET_SEMAPHORE_A);
    P_NVB0B5_SET_SEMAPHORE_A(p, va >> 32);
    P_NVB0B5_SET_SEMAPHORE_B(p, va & 0xffffffff);
    P_NVB0B5_SET_SEMAPHORE_PAYLOAD(p, payload);
    P_IMMD(p, NVB0B5, LAUNCH_DMA,
           {
               .data_transfer_type = DATA_TRANSFER_TYPE_NONE,
               .flush_enable = FLUSH_ENABLE_TRUE,
               .semaphore_type = SEMAPHORE_TYPE_RELEASE_ONE_WORD_SEMAPHORE,
           });
}
