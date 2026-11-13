// SPDX-License-Identifier: MIT
// The raster and ROP shortcuts, method for method against the class header's fields, within their word bounds.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "kits/SwitchGM20B/SwitchGM20B.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h"
#include "push_words.h"

namespace {

using switch_gm20b_test::Method;
using switch_gm20b_test::Methods;

constexpr uint32_t k3D = SUBC_NVB197;

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

TEST(EmitRaster, ConstantColourIsTheEnableThenFourWords)
{
    const uint32_t rgba[4] = {0x3f4ccccd, 0x3dcccccd, 0x3e99999a, 0x3f800000};
    const auto w = emitted(SWITCH_GM20B_CONSTANT_COLOR_WORDS, [&](nv_push *p) {
        switch_gm20b_constant_color(p, rgba);
    });
    EXPECT_EQ(w.size(), SWITCH_GM20B_CONSTANT_COLOR_WORDS);
    EXPECT_EQ(Methods(w),
              (std::vector<Method>{
                  {k3D, NVB197_SET_CONSTANT_COLOR_RENDERING, NVB197_SET_CONSTANT_COLOR_RENDERING_ENABLE_TRUE},
                  {k3D, NVB197_SET_CONSTANT_COLOR_RENDERING_RED, rgba[0]},
                  {k3D, NVB197_SET_CONSTANT_COLOR_RENDERING_GREEN, rgba[1]},
                  {k3D, NVB197_SET_CONSTANT_COLOR_RENDERING_BLUE, rgba[2]},
                  {k3D, NVB197_SET_CONSTANT_COLOR_RENDERING_ALPHA, rgba[3]},
              }));
}

TEST(EmitRaster, ConstantColourOffIsOneWord)
{
    const auto w = emitted(1, switch_gm20b_constant_color_off);
    EXPECT_EQ(Methods(w), (std::vector<Method>{{k3D, NVB197_SET_CONSTANT_COLOR_RENDERING, 0}}));
}

TEST(EmitRaster, FillViaTriangleIsOneImmediateWord)
{
    for (uint32_t mode :
         {uint32_t(NVB197_SET_FILL_VIA_TRIANGLE_MODE_DISABLED), uint32_t(NVB197_SET_FILL_VIA_TRIANGLE_MODE_FILL_ALL),
          uint32_t(NVB197_SET_FILL_VIA_TRIANGLE_MODE_FILL_BBOX)}) {
        const auto w = emitted(SWITCH_GM20B_FILL_VIA_TRIANGLE_WORDS, [&](nv_push *p) {
            switch_gm20b_fill_via_triangle(p, mode);
        });
        EXPECT_EQ(w.size(), 1u);
        EXPECT_EQ(Methods(w), (std::vector<Method>{{k3D, NVB197_SET_FILL_VIA_TRIANGLE, mode}}));
    }
}

}  // namespace
