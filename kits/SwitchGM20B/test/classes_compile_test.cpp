// SPDX-License-Identifier: MIT
#include "../SwitchGM20B.h"

#include <gtest/gtest.h>

TEST(SwitchGM20BClasses, OneHeaderPerEngine)
{
    EXPECT_EQ(NVB197_WAIT_FOR_IDLE, 0x0110);
    EXPECT_EQ(NVB0B5_LAUNCH_DMA, 0x0300);
}
