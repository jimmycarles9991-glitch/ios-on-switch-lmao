// SPDX-License-Identifier: MIT
// The counter, report and small copy-engine writes, method for method against the class headers' fields as NVK sets
// them, and within each header's word bound.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitReport.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

constexpr uint32_t k3D = SUBC_NVB197, kCopy = SUBC_NVB0B5;

template <typename F> std::vector<uint32_t> emitted(uint32_t bound, F emit)
{
    std::vector<uint32_t> words(64);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    emit(&p);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), bound);
    return words;
}

TEST(EmitReport, SamplesBeginClearsTheCounterThenCounts)
{
    const auto w = emitted(SWITCH_GM20B_SAMPLES_BEGIN_WORDS, switch_gm20b_samples_begin);
    EXPECT_EQ(w.size(), SWITCH_GM20B_SAMPLES_BEGIN_WORDS);
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {k3D, NVB197_CLEAR_REPORT_VALUE, NVB197_CLEAR_REPORT_VALUE_TYPE_ZPASS_PIXEL_CNT},
                              {k3D, NVB197_SET_ZPASS_PIXEL_COUNT, NVB197_SET_ZPASS_PIXEL_COUNT_ENABLE_TRUE},
                          }));
}

TEST(EmitReport, SamplesEndStopsCounting)
{
    const auto w = emitted(SWITCH_GM20B_SAMPLES_END_WORDS, switch_gm20b_samples_end);
    EXPECT_EQ(Methods(w), (std::vector<Method>{{k3D, NVB197_SET_ZPASS_PIXEL_COUNT, 0}}));
}

TEST(EmitReport, SamplesReportIsTheFourWordCounterAfterAllStages)
{
    const auto w = emitted(SWITCH_GM20B_SAMPLES_REPORT_WORDS, [](nv_push *p) {
        switch_gm20b_samples_report(p, 0x12345678f0ull);
    });
    EXPECT_EQ(w.size(), SWITCH_GM20B_SAMPLES_REPORT_WORDS);
    const uint32_t d = uint32_t(NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, REPORT_ONLY) |
                                NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL) |
                                NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, REPORT, ZPASS_PIXEL_CNT64) |
                                NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, FOUR_WORDS) |
                                NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, FLUSH_DISABLE, TRUE));
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {k3D, NVB197_SET_REPORT_SEMAPHORE_A, 0x12},
                              {k3D, NVB197_SET_REPORT_SEMAPHORE_B, 0x345678f0},
                              {k3D, NVB197_SET_REPORT_SEMAPHORE_C, 0},
                              {k3D, NVB197_SET_REPORT_SEMAPHORE_D, d},
                          }));
}

TEST(EmitReport, CopyBytesIsOnePitchLine)
{
    const auto w = emitted(SWITCH_GM20B_COPY_BYTES_WORDS, [](nv_push *p) {
        switch_gm20b_copy_bytes(p, 0x0100002000ull, 0x0200003008ull, 8,
                                NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED);
    });
    const uint32_t launch = uint32_t(
        NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) | NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
        NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) | NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH));
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {kCopy, NVB0B5_OFFSET_IN_UPPER, 0x01},
                              {kCopy, NVB0B5_OFFSET_IN_LOWER, 0x00002000},
                              {kCopy, NVB0B5_OFFSET_OUT_UPPER, 0x02},
                              {kCopy, NVB0B5_OFFSET_OUT_LOWER, 0x00003008},
                              {kCopy, NVB0B5_PITCH_IN, 8},
                              {kCopy, NVB0B5_PITCH_OUT, 8},
                              {kCopy, NVB0B5_LINE_LENGTH_IN, 8},
                              {kCopy, NVB0B5_LINE_COUNT, 1},
                              {kCopy, NVB0B5_LAUNCH_DMA, launch},
                          }));
}

TEST(EmitReport, CopyU64IsTwoRemapConstantsOfFourBytes)
{
    const auto w = emitted(SWITCH_GM20B_COPY_U64_WORDS, [](nv_push *p) {
        switch_gm20b_copy_u64(p, 0x0200003008ull, 0x0000000700000001ull,
                              NVB0B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED);
    });
    const uint32_t remap = uint32_t(
        NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) | NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_Y, CONST_B) |
        NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_Z, NO_WRITE) | NVDEF(NVB0B5, SET_REMAP_COMPONENTS, DST_W, NO_WRITE) |
        NVDEF(NVB0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, FOUR) |
        NVDEF(NVB0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, TWO) |
        NVDEF(NVB0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, TWO));
    const uint32_t launch =
        uint32_t(NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
                 NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) | NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
                 NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) | NVDEF(NVB0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {kCopy, NVB0B5_SET_REMAP_CONST_A, 1},
                              {kCopy, NVB0B5_SET_REMAP_CONST_B, 7},
                              {kCopy, NVB0B5_SET_REMAP_COMPONENTS, remap},
                              {kCopy, NVB0B5_OFFSET_OUT_UPPER, 0x02},
                              {kCopy, NVB0B5_OFFSET_OUT_LOWER, 0x00003008},
                              {kCopy, NVB0B5_LINE_LENGTH_IN, 1},
                              {kCopy, NVB0B5_LAUNCH_DMA, launch},
                          }));
}

TEST(EmitReport, CopyPredicateNamesTheValueAndTheMode)
{
    const auto w = emitted(SWITCH_GM20B_COPY_PREDICATE_WORDS, [](nv_push *p) {
        switch_gm20b_copy_predicate(p, 0x0200003008ull, NVB0B5_SET_RENDER_ENABLE_C_MODE_CONDITIONAL);
    });
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {kCopy, NVB0B5_SET_RENDER_ENABLE_A, 0x02},
                              {kCopy, NVB0B5_SET_RENDER_ENABLE_B, 0x00003008},
                              {kCopy, NVB0B5_SET_RENDER_ENABLE_C, NVB0B5_SET_RENDER_ENABLE_C_MODE_CONDITIONAL},
                          }));
}

TEST(EmitReport, CopyReleaseIsASemaphoreOnlyLaunch)
{
    const auto w = emitted(SWITCH_GM20B_COPY_RELEASE_WORDS, [](nv_push *p) {
        switch_gm20b_copy_release(p, 0x0300000040ull, 99);
    });
    const uint32_t launch =
        uint32_t(NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NONE) | NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
                 NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_TYPE, RELEASE_ONE_WORD_SEMAPHORE));
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {kCopy, NVB0B5_SET_SEMAPHORE_A, 0x03},
                              {kCopy, NVB0B5_SET_SEMAPHORE_B, 0x40},
                              {kCopy, NVB0B5_SET_SEMAPHORE_PAYLOAD, 99},
                              {kCopy, NVB0B5_LAUNCH_DMA, launch},
                          }));
}

}  // namespace
