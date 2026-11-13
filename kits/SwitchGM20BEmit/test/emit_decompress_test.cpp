// SPDX-License-Identifier: MIT
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitDecompress.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

template <typename Emit> std::vector<Method> emit(Emit e)
{
    std::vector<uint32_t> words(8);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    e(&p);
    words.resize(nv_push_dw_count(&p));
    EXPECT_LE(words.size(), SWITCH_GM20B_DECOMPRESS_WORDS);
    return Methods(words);
}

TEST(EmitDecompress, ColourTargetAndLayerInTheirFields)
{
    EXPECT_EQ(emit([](nv_push *p) {
                  switch_gm20b_decompress_color(p, 0, 0);
              }),
              (std::vector<Method>{{0, 0x02e0, 0}}));
    EXPECT_EQ(emit([](nv_push *p) {
                  switch_gm20b_decompress_color(p, 3, 5);
              }),
              (std::vector<Method>{{0, 0x02e0, 3u | 5u << 4}}));
}

TEST(EmitDecompress, ZetaPlanesByTheirEnables)
{
    EXPECT_EQ(emit([](nv_push *p) {
                  switch_gm20b_decompress_zeta(p, true, false);
              }),
              (std::vector<Method>{{0, 0x11fc, 1}}));
    EXPECT_EQ(emit([](nv_push *p) {
                  switch_gm20b_decompress_zeta(p, true, true);
              }),
              (std::vector<Method>{{0, 0x11fc, 0x11}}));
}

}  // namespace
