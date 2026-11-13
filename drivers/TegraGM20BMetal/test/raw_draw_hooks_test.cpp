// SPDX-License-Identifier: MIT
// The CPU's reading behind the constant-colour and fill-via-triangle shortcuts: a quad from six indices, one colour.
#include <gtest/gtest.h>
#include <cstring>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_draw_hooks.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_quad_fill.h"

namespace {

TEST(DrawHooks, CoreAnimationsQuadIndicesAreAStrip)
{
    const uint32_t list[6] = {0, 1, 2, 2, 1, 3};
    uint32_t c[4];
    ASSERT_TRUE(raw::quadOfIndices(list, c));
    EXPECT_EQ(c[0], 0u);
    EXPECT_EQ(c[3], 3u);
    EXPECT_TRUE((c[1] == 1 && c[2] == 2) || (c[1] == 2 && c[2] == 1));
    const uint32_t disjoint[6] = {0, 1, 2, 3, 4, 5}, same[6] = {0, 1, 2, 0, 1, 2};
    EXPECT_FALSE(raw::quadOfIndices(disjoint, c));
    EXPECT_FALSE(raw::quadOfIndices(same, c));
}

TEST(DrawHooks, OneColourOrNone)
{
    struct Vertex {
        float position[4];
        _Float16 colour[4];
    } v[4] = {};

    for (auto &x : v)
        for (int c = 0; c < 4; c++)
            x.colour[c] = _Float16(c == 3 ? 1.0f : 0.5f);
    raw::FillStream s;
    s.bytes = reinterpret_cast<const uint8_t *>(v);
    s.size = sizeof v;
    s.stride = sizeof(Vertex);
    s.colorOffset = 16;
    uint32_t rgba[4];
    ASSERT_TRUE(raw::uniformColour(s, nullptr, 4, rgba));
    float f;
    memcpy(&f, &rgba[0], 4);
    EXPECT_EQ(f, 0.5f);
    v[2].colour[1] = _Float16(0.25f);
    EXPECT_FALSE(raw::uniformColour(s, nullptr, 4, rgba));
    const uint32_t first3[3] = {0, 1, 3};
    EXPECT_TRUE(raw::uniformColour(s, first3, 3, rgba));
    s.colorOffset = -1;
    EXPECT_FALSE(raw::uniformColour(s, nullptr, 4, rgba));
}

TEST(DrawHooks, TheQuadsWindowCornersDecideTheBox)
{
    struct Vertex {
        float position[4];
    } v[4] = {{{-1, -1, 0, 1}}, {{1, -1, 0, 1}}, {{-1, 1, 0, 1}}, {{1, 1, 0, 1}}};

    raw::FillStream s;
    s.bytes = reinterpret_cast<const uint8_t *>(v);
    s.size = sizeof v;
    s.stride = sizeof(Vertex);
    s.positionComponents = 4;
    const double viewport[4] = {0, 0, 64, 32};
    const uint32_t corners[4] = {0, 1, 2, 3};
    float window[8];
    ASSERT_TRUE(raw::quadWindow(s, corners, nullptr, viewport, window));
    EXPECT_TRUE(raw::axisAlignedQuad(window));
    v[3].position[0] = 0.5f;
    ASSERT_TRUE(raw::quadWindow(s, corners, nullptr, viewport, window));
    EXPECT_FALSE(raw::axisAlignedQuad(window));
}

}  // namespace
