// SPDX-License-Identifier: MIT
// Core Animation's blur-chain and fill draws (raw_ca_route.h) take their state's own program. Choosing the 2D engine's
// blit, the ROP's constant colour or one bounding-box triangle for them needs their per-draw bytes (taps, texture matrix,
// corners, indices, every vertex's colour), which the encoder would read on the CPU every frame, so none of those routes
// exists.
#include <gtest/gtest.h>
#include <type_traits>
#include <utility>
#include "../raw/runtime/raw_ca_route.h"
#include "../raw/runtime/raw_gpu.h"

namespace raw {
struct EncoderHarness {
    template <class E> static constexpr auto shortcuts(int) -> decltype(sizeof(typename E::Shortcut), true)
    {
        return true;
    }

    template <class E> static constexpr bool shortcuts(...)
    {
        return false;
    }
};
}  // namespace raw

namespace {

template <class T, class = void> struct NamesABlurRoute : std::false_type {};

template <class T>
struct NamesABlurRoute<T, std::void_t<decltype(std::declval<T &>().blurFunction)>> : std::true_type {};

TEST(BlurDraw, FillDrawsTakeNoShortcutTheCpuChooses)
{
    EXPECT_FALSE(raw::EncoderHarness::shortcuts<raw::RenderEncoder>(0));
}

TEST(BlurDraw, BlurChainStatesCarryNoDrawRoute)
{
    EXPECT_EQ(raw::caRouteOf("PRGhAXm_Tds4A2Xhf_Isrc"), raw::CaRoute::Blur);
    EXPECT_EQ(raw::caRouteOf("PRGhAXm_Tdb2A2Xhf_Isrc"), raw::CaRoute::Blur);
    EXPECT_FALSE(NamesABlurRoute<raw::Pipeline>::value);
}

}  // namespace
