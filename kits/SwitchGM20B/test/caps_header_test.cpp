// SPDX-License-Identifier: MIT
// SwitchGM20BCaps.h as C++ and as C (caps_header_c.c), and its rows against the class headers.
#include "kits/SwitchGM20B/SwitchGM20BCaps.h"
#include "kits/SwitchGM20B/SwitchGM20B.h"

#include <gtest/gtest.h>

extern "C" uint32_t switch_gm20b_caps_c_count(void);

namespace {

constexpr const switch_gm20b_format *find(uint16_t mtl)
{
    for (const switch_gm20b_format &f : switch_gm20b_formats)
        if (f.mtl == mtl)
            return &f;
    return nullptr;
}

static_assert(find(80) && find(80)->ct == NVB197_SET_COLOR_TARGET_FORMAT_V_A8R8G8B8, "BGRA8Unorm's target");
static_assert(find(260) && find(260)->zt == NVB197_SET_ZT_FORMAT_V_ZF32_X24S8, "Depth32Float_Stencil8's zeta");
static_assert(SWITCH_GM20B_LIMIT_COLOR_TARGETS == 8, "eight colour targets");

TEST(SwitchGM20BCaps, OneTableForCAndCpp)
{
    EXPECT_EQ(switch_gm20b_caps_c_count(), SWITCH_GM20B_FORMAT_COUNT);
    EXPECT_EQ(sizeof(switch_gm20b_features) / sizeof(switch_gm20b_features[0]), size_t(SWITCH_GM20B_FEATURE_COUNT));
}

TEST(SwitchGM20BCaps, FormatsAreUnique)
{
    for (const switch_gm20b_format &f : switch_gm20b_formats)
        EXPECT_EQ(find(f.mtl), &f) << f.mtl;
}

}  // namespace
