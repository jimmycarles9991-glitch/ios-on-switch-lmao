// SwitchSocThermWindow.h: SOC_THERM's LVL0 window around a group's temperature, its register fields (L4T soctherm.c
// and tegra210-soctherm.c's T210 masks) and the groups' registers.
#include "drivers/SwitchPower/SwitchSocThermWindow.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

uint32_t Up(uint32_t lvl0)
{
    return (lvl0 >> 18) & 0x1ffu;
}

uint32_t Down(uint32_t lvl0)
{
    return (lvl0 >> 9) & 0x1ffu;
}

TEST(SwitchSocThermWindow, OneDegreeEitherSideInHalfDegreeSteps)
{
    const uint32_t w = switch_soctherm_lvl0_window(0, 45500);
    EXPECT_EQ(Up(w), 93u);    // 46.5 C
    EXPECT_EQ(Down(w), 89u);  // 44.5 C
    EXPECT_TRUE(w & (1u << 8));
}

TEST(SwitchSocThermWindow, OtherFieldsKeptStatusAndThrottleUntouched)
{
    const uint32_t keep = 0x7fu;
    const uint32_t w = switch_soctherm_lvl0_window(keep | (0x1ffu << 18) | (0x1ffu << 9), 60000);
    EXPECT_EQ(w & 0xffu, keep);
    EXPECT_EQ(Up(w), 122u);
    EXPECT_EQ(Down(w), 118u);
    EXPECT_EQ(w >> 27, 0u);
}

TEST(SwitchSocThermWindow, ClampedToTheField)
{
    EXPECT_EQ(Down(switch_soctherm_lvl0_window(0, 500)), 0u);
    EXPECT_EQ(Down(switch_soctherm_lvl0_window(0, -20000)), 0u);
    EXPECT_EQ(Up(switch_soctherm_lvl0_window(0, 300000)), 0x1ffu);
}

TEST(SwitchSocThermWindow, GroupRegistersAsL4TsT210Masks)
{
    EXPECT_EQ(switch_soctherm_intr_bits(&switch_soctherm_groups[0]), 0x3u << 8);   // CPU: TH_INTR_CD0/CU0
    EXPECT_EQ(switch_soctherm_intr_bits(&switch_soctherm_groups[1]), 0x3u << 16);  // GPU: GD0/GU0
    EXPECT_EQ(switch_soctherm_intr_bits(&switch_soctherm_groups[2]), 0x3u << 0);   // PLLX: PD0/PU0
    EXPECT_EQ(switch_soctherm_groups[0].lvl0, 0x0u);                               // THERMCTL_LEVEL0_GROUP_CPU
    EXPECT_EQ(switch_soctherm_groups[1].lvl0, 0x4u);                               // ..._GPU
    EXPECT_EQ(switch_soctherm_groups[2].lvl0, 0xcu);                               // ..._TSENSE
    EXPECT_EQ(switch_soctherm_groups[0].temp, 0x1c8u);
    EXPECT_EQ(switch_soctherm_groups[0].temp_shift, 16u);
    EXPECT_EQ(switch_soctherm_groups[2].temp, 0x1ccu);
}

TEST(SwitchSocThermWindow, ThermtripAtL4TsTemperaturesForThisConsole)
{
    const uint32_t ctl = switch_soctherm_thermtrip_ctl(0xffffffffu);
    EXPECT_EQ(ctl & 0x1ffu, 192u);          // PLLX 96 C: the switchroot DT's nvidia,thermtrips <3 96000>
    EXPECT_EQ((ctl >> 9) & 0x1ffu, 254u);   // CPU 127 C
    EXPECT_EQ((ctl >> 18) & 0x1ffu, 254u);  // GPU 127 C
    EXPECT_EQ((ctl >> 27) & 0x7u, 0x7u);    // PLLX, CPU, GPU on
    EXPECT_EQ((ctl >> 30) & 0x3u, 0u);      // MEM and ANY_EN off
}

}  // namespace
