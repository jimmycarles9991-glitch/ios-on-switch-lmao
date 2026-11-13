// SPDX-License-Identifier: MIT
// Render enable, the inline word it gates and the copy engine's reduction, method for method against the class headers'
// fields, and within each header's word bound.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitPredicate.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

constexpr uint32_t kCompute = SUBC_NVB1C0, kCopy = SUBC_NVB0B5;

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

TEST(EmitPredicate, RenderEnableComparesTheOperandsAtTheAddress)
{
    const auto w = emitted(SWITCH_GM20B_RENDER_ENABLE_WORDS, [](nv_push *p) {
        switch_gm20b_render_enable(p, 0x0312345670ull, NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL);
    });
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {kCompute, NVB1C0_SET_GLOBAL_RENDER_ENABLE_A, 0x03},
                              {kCompute, NVB1C0_SET_GLOBAL_RENDER_ENABLE_B, 0x12345670},
                              {kCompute, NVB1C0_SET_GLOBAL_RENDER_ENABLE_C,
                               NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_RENDER_IF_NOT_EQUAL},
                          }));
}

TEST(EmitPredicate, AlwaysIsNvksEndOfConditionalRendering)
{
    const auto w = emitted(SWITCH_GM20B_RENDER_ENABLE_WORDS, switch_gm20b_render_enable_always);
    EXPECT_EQ(Methods(w),
              (std::vector<Method>{
                  {kCompute, NVB1C0_SET_GLOBAL_RENDER_ENABLE_A, 0},
                  {kCompute, NVB1C0_SET_GLOBAL_RENDER_ENABLE_B, 0},
                  {kCompute, NVB1C0_SET_GLOBAL_RENDER_ENABLE_C, NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE},
              }));
}

TEST(EmitPredicate, InlineWordIsOnePitchLineFlushed)
{
    const auto w = emitted(SWITCH_GM20B_INLINE_WORD_WORDS, [](nv_push *p) {
        switch_gm20b_compute_inline_word(p, 0x0300000024ull, 0xdeadbeef);
    });
    EXPECT_EQ(w.size(), SWITCH_GM20B_INLINE_WORD_WORDS);
    const uint32_t launch = uint32_t(NVDEF(NVB1C0, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
                                     NVDEF(NVB1C0, LAUNCH_DMA, COMPLETION_TYPE, FLUSH_ONLY));
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {kCompute, NVB1C0_LINE_LENGTH_IN, 4},
                              {kCompute, NVB1C0_LINE_COUNT, 1},
                              {kCompute, NVB1C0_OFFSET_OUT_UPPER, 0x03},
                              {kCompute, NVB1C0_OFFSET_OUT, 0x24},
                              {kCompute, NVB1C0_LAUNCH_DMA, launch},
                              {kCompute, NVB1C0_LOAD_INLINE_DATA, 0xdeadbeef},
                          }));
}

TEST(EmitPredicate, IdleFlushedWaitsThenFlushesShaderData)
{
    const auto w = emitted(SWITCH_GM20B_COMPUTE_IDLE_WORDS, switch_gm20b_compute_idle_flushed);
    EXPECT_EQ(Methods(w), (std::vector<Method>{
                              {kCompute, NVB1C0_WAIT_FOR_IDLE, 0},
                              {kCompute, NVB1C0_INVALIDATE_SHADER_CACHES,
                               uint32_t(NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE) |
                                        NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE))},
                          }));
}

TEST(EmitPredicate, WordReduceCopiesThenReleasesItsReductionOnTheCopy)
{
    for (const bool is_signed : {false, true}) {
        const auto w = emitted(SWITCH_GM20B_COPY_WORD_REDUCE_WORDS, [&](nv_push *p) {
            switch_gm20b_copy_word_reduce(p, 0x0400000124ull, 0x0300000040ull, 7,
                                          NVB0B5_LAUNCH_DMA_SEMAPHORE_REDUCTION_IMAX, is_signed);
        });
        EXPECT_EQ(w.size(), SWITCH_GM20B_COPY_WORD_REDUCE_WORDS);
        const uint32_t launch = uint32_t(NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
                                         NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
                                         NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_TYPE, RELEASE_ONE_WORD_SEMAPHORE) |
                                         NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
                                         NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
                                         NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_REDUCTION, IMAX) |
                                         (is_signed ? NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_REDUCTION_SIGN, SIGNED)
                                                    : NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_REDUCTION_SIGN, UNSIGNED)) |
                                         NVDEF(NVB0B5, LAUNCH_DMA, SEMAPHORE_REDUCTION_ENABLE, TRUE));
        EXPECT_EQ(Methods(w), (std::vector<Method>{
                                  {kCopy, NVB0B5_OFFSET_IN_UPPER, 0x04},
                                  {kCopy, NVB0B5_OFFSET_IN_LOWER, 0x124},
                                  {kCopy, NVB0B5_OFFSET_OUT_UPPER, 0x03},
                                  {kCopy, NVB0B5_OFFSET_OUT_LOWER, 0x40},
                                  {kCopy, NVB0B5_PITCH_IN, 4},
                                  {kCopy, NVB0B5_PITCH_OUT, 4},
                                  {kCopy, NVB0B5_LINE_LENGTH_IN, 4},
                                  {kCopy, NVB0B5_LINE_COUNT, 1},
                                  {kCopy, NVB0B5_SET_SEMAPHORE_A, 0x03},
                                  {kCopy, NVB0B5_SET_SEMAPHORE_B, 0x40},
                                  {kCopy, NVB0B5_SET_SEMAPHORE_PAYLOAD, 7},
                                  {kCopy, NVB0B5_LAUNCH_DMA, launch},
                              }));
    }
}

}  // namespace
