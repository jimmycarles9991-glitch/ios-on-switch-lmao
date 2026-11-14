// SwitchSocThermThrottle.h: SOC_THERM's hardware throttle as L4T programs it on T210B01 (soctherm.c, the switchroot
// device tree), against a model of the block at its TRM 40.9 reset values, the line the boot log and
// kern.switch_soctherm print, and where the kernel programs it.
#include "drivers/SwitchPower/SwitchSocThermThrottle.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {
constexpr uintptr_t kSocTherm = 0x700e2000;
constexpr uint32_t kIndicatorStride = 0x30, kLite = 0x430, kOc5 = 0x550, kGpuOffset = 8;
constexpr uint32_t kLowMedHigh = 1u << 30;

struct Block {
    std::map<uint32_t, uint32_t> regs;
    std::vector<uint32_t> writes;
} gBlock;

// TRM 40.9's reset values where they are not 0: GLOBAL_THROTTLE_CFG LEGACY_PSKIP_MODE and ENB, every OC alarm STICKY
// with its throttle off, every GPU vector's read-only LOW_MED_HIGH.
void ResetBlock()
{
    gBlock = {};
    gBlock.regs[SWITCH_SOCTHERM_GLOBAL_CFG] = 0x11;
    for (uint32_t oc = 0; oc < 5; ++oc)
        gBlock.regs[SWITCH_SOCTHERM_OC1_CFG + oc * SWITCH_SOCTHERM_OC_STRIDE] = 0x4;
    gBlock.regs[0x410] = kLowMedHigh;
    for (uint32_t indicator = kLite; indicator <= kOc5; indicator += kIndicatorStride)
        gBlock.regs[indicator + kGpuOffset] = kLowMedHigh;
}

volatile uint8_t *Regs()
{
    return reinterpret_cast<volatile uint8_t *>(kSocTherm);
}

uint32_t Reg(uint32_t offset)
{
    return gBlock.regs[offset];
}

void Program()
{
    switch_soctherm_throttle_vectors(Regs());
    switch_soctherm_throttle_monitors(Regs());
}

std::string Line(uint32_t cclkg_divider)
{
    char line[320];
    const switch_soctherm_throttle_state state = switch_soctherm_throttle_read(Regs(), cclkg_divider);
    EXPECT_LT(switch_soctherm_throttle_format(&state, line, sizeof(line)), static_cast<int>(sizeof(line)));
    return line;
}

TEST(SwitchSocThermThrottle, HeavyAsL4TSetsItOnT210B01)
{
    ResetBlock();
    Program();
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_HEAVY_CPU_PSKIP), 0x8000ffffu);                // ENB, 256/256: cpu-throt-percent 0
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_HEAVY_CPU_RAMP), 0x0000ff0fu);                 // duration 0xff, step 0xf
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_HEAVY_GPU_PSKIP), 0x80000000u | kLowMedHigh);  // ENB, gpu-throt-level NONE
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_HEAVY_PRIORITY), 100u);
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_HEAVY_DELAY), 0u);
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_PRIORITY_LOCK), 100u);
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_GLOBAL_CFG), 0x1u);  // ENB alone: L4T drops LEGACY_PSKIP_MODE
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_STATS_CTL), 0xfu);
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_OC_STATS_CTL), 0x1u);
    // THERMCTL_LEVEL2_GROUP_TSENSE: UP = DN = 189 (94.5 C), CPU HEAVY, GPU HEAVY, EN (throttrip_program's LEVEL2).
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_LEVEL2_TSENSE), 189u << 18 | 189u << 9 | 2u << 5 | 2u << 3 | 1u << 8);
}

TEST(SwitchSocThermThrottle, NoIndicatorDrivesTheGpuOrSkipsTheCpu)
{
    ResetBlock();
    Program();
    for (uint32_t indicator = kLite; indicator <= kOc5; indicator += kIndicatorStride) {
        const uint32_t cpu = Reg(indicator);
        const uint32_t gpu = Reg(indicator + kGpuOffset);
        EXPECT_TRUE(!(gpu & SWITCH_SOCTHERM_PSKIP_ENB) || !(gpu & SWITCH_SOCTHERM_PSKIP_GPU_VECTOR)) << indicator;
        EXPECT_TRUE(!(cpu & SWITCH_SOCTHERM_PSKIP_ENB) || ((cpu >> 8) & 0xffu) == (cpu & 0xffu)) << indicator;
    }
    for (uint32_t oc = 0; oc < 5; ++oc)
        EXPECT_EQ(Reg(SWITCH_SOCTHERM_OC1_CFG + oc * SWITCH_SOCTHERM_OC_STRIDE) & SWITCH_SOCTHERM_OC_EN_THROTTLE, 0u);
}

TEST(SwitchSocThermThrottle, VectorsBeforeTheGlobalEnableAndTheMonitorLast)
{
    ResetBlock();
    Program();
    const std::vector<uint32_t> order = {
        SWITCH_SOCTHERM_HEAVY_CPU_PSKIP, SWITCH_SOCTHERM_HEAVY_CPU_RAMP, SWITCH_SOCTHERM_HEAVY_GPU_PSKIP,
        SWITCH_SOCTHERM_HEAVY_PRIORITY,  SWITCH_SOCTHERM_HEAVY_DELAY,    SWITCH_SOCTHERM_PRIORITY_LOCK,
        SWITCH_SOCTHERM_GLOBAL_CFG,      SWITCH_SOCTHERM_STATS_CTL,      SWITCH_SOCTHERM_OC_STATS_CTL,
        SWITCH_SOCTHERM_LEVEL2_TSENSE,
    };
    EXPECT_EQ(gBlock.writes, order);
}

TEST(SwitchSocThermThrottle, AHigherPriorityLockIsKept)
{
    ResetBlock();
    gBlock.regs[SWITCH_SOCTHERM_PRIORITY_LOCK] = 200;
    Program();
    EXPECT_EQ(Reg(SWITCH_SOCTHERM_PRIORITY_LOCK), 200u);
}

TEST(SwitchSocThermThrottle, L4TsDepthAndLevelEncodings)
{
    EXPECT_EQ(switch_soctherm_depth_dividend(0), 255u);
    EXPECT_EQ(switch_soctherm_depth_dividend(85), 37u);  // T210's heavy: 38/256 of the clock
    EXPECT_EQ(switch_soctherm_gpu_vector(0), 0u);
    EXPECT_EQ(switch_soctherm_gpu_vector(1), 1u);
    EXPECT_EQ(switch_soctherm_gpu_vector(2), 3u);
    EXPECT_EQ(switch_soctherm_gpu_vector(3), 7u);  // T210's heavy: every EXT_THERM input, the GPU's /8
    EXPECT_STREQ(switch_soctherm_gpu_level_name(0x80070000u), "high");
    EXPECT_STREQ(switch_soctherm_gpu_level_name(0x00070000u), "off");
}

TEST(SwitchSocThermThrottle, TheStateLineOnceProgrammed)
{
    ResetBlock();
    Program();
    EXPECT_EQ(Line(SWITCH_CAR_CDIV_USE_THERM | 0x80000000u),
              "heavy_pllx_c=94.5 heavy_req=cpu+gpu heavy_cpu=256/256 heavy_gpu=none heavy_prio=100 lock=100 global=on "
              "light=off oc_throttle=0x0 cdiv_therm=on gpu_depth=0 cpu_skip=off throt_status=0x0 heavy_ups=0 "
              "oc_events=0,0,0,0");
}

TEST(SwitchSocThermThrottle, TheStateLineAtResetAndUnderThrottle)
{
    ResetBlock();
    EXPECT_THAT(Line(0), testing::StartsWith("heavy_pllx_c=off heavy_req=none heavy_cpu=off heavy_gpu=off heavy_prio=0 "
                                             "lock=0 global=on light=off oc_throttle=0x0 cdiv_therm=off gpu_depth=0"));
    gBlock.regs[SWITCH_SOCTHERM_GPU_PSKIP_STATUS] = 7u << 20 | 1u;
    gBlock.regs[SWITCH_SOCTHERM_CPU_PSKIP_STATUS] = 37u << 12 | 255u << 4 | 1u;
    gBlock.regs[SWITCH_SOCTHERM_OC1_CFG + SWITCH_SOCTHERM_OC_STRIDE] = 0x5;
    gBlock.regs[SWITCH_SOCTHERM_LEVEL2_UP_STATS] = 3;
    gBlock.regs[SWITCH_SOCTHERM_OC1_STATS + 4] = 9;
    const std::string line = Line(0);
    EXPECT_THAT(line, testing::HasSubstr(" oc_throttle=0x2 "));
    EXPECT_THAT(line, testing::HasSubstr(" gpu_depth=7 cpu_skip=37/255 "));
    EXPECT_THAT(line, testing::HasSubstr(" heavy_ups=3 oc_events=0,9,0,0"));
}

TEST(SwitchSocThermThrottle, TheLineFitsTheKernelsBuffer)
{
    ResetBlock();
    (void)switch_soctherm_throttle_read(Regs(), 0);  // the model now holds every register the line reads
    for (auto &entry : gBlock.regs)
        entry.second = ~0u;
    EXPECT_LT(Line(~0u).size(), 320u);
}

TEST(SwitchSocThermThrottle, OnlyTheThrottleLevelsStatusIsLeftAlone)
{
    EXPECT_EQ(SWITCH_SOCTHERM_INTR_THROTTLE_LEVELS, 0xfcfcfcfcu);  // L4T's TH_INTR_IGNORE_MASK
    for (const switch_soctherm_group_regs &group : switch_soctherm_groups)
        EXPECT_EQ(SWITCH_SOCTHERM_INTR_THROTTLE_LEVELS & switch_soctherm_intr_bits(&group), 0u);
}

// L4T's order around the CAR: vectors and global enable, SUPER_CCLKG_DIVIDER's thermal control, then the monitor; after
// THERMTRIP and before the LVL0 windows, on every arming (boot and each SC7 wake).
TEST(SwitchSocThermThrottleSource, ArmedWithThermtripOnEveryBringUp)
{
    const std::string source = switch_ios::test::SourceText("../SwitchSocTherm.cpp");
    const std::string throttle = switch_ios::test::Body(source, "void SwitchSocTherm::armThrottle(");
    const size_t vectors = throttle.find("switch_soctherm_throttle_vectors(mapped)");
    const size_t car = throttle.find("switch_car_rmw(SWITCH_CAR_SUPER_CCLKG_DIVIDER, 0, SWITCH_CAR_CDIV_USE_THERM)");
    const size_t monitors = throttle.find("switch_soctherm_throttle_monitors(mapped)");
    ASSERT_NE(vectors, std::string::npos);
    ASSERT_NE(car, std::string::npos);
    ASSERT_NE(monitors, std::string::npos);
    EXPECT_LT(vectors, car);
    EXPECT_LT(car, monitors);
    const std::string arm = switch_ios::test::Body(source, "IOReturn SwitchSocTherm::armAction(");
    const size_t thermtrip = arm.find("self->armThermtrip(regs)");
    const size_t armed = arm.find("self->armThrottle(regs)");
    ASSERT_NE(thermtrip, std::string::npos);
    ASSERT_NE(armed, std::string::npos);
    EXPECT_LT(thermtrip, armed);
    EXPECT_LT(armed, arm.find("centre(regs, group, millic)"));
}

// SC7 wakes with SOC_THERM reset again (SwitchSensors' bring-up): the CPU's divider is handed back before that reset, so
// SOC_THERM is reset and reprogrammed with the bit clear, as at boot (L4T sets it once SOC_THERM is programmed).
TEST(SwitchSocThermThrottleSource, TheWakeHandsTheCpuDividerBackBeforeTheReset)
{
    const std::string arm = switch_ios::test::Body(switch_ios::test::SourceText("../SwitchSocTherm.cpp"),
                                                   "IOReturn SwitchSocTherm::armAction(");
    const size_t down = arm.find("} else {");
    ASSERT_NE(down, std::string::npos);
    EXPECT_THAT(arm.substr(down),
                testing::HasSubstr("switch_car_rmw(SWITCH_CAR_SUPER_CCLKG_DIVIDER, SWITCH_CAR_CDIV_USE_THERM, 0)"));
    const std::string resume = switch_ios::test::Body(switch_ios::test::SourceText("../SwitchSensors.cpp"),
                                                      "extern \"C\" void switch_sensors_resume(void)");
    const size_t gone = resume.find("switch_soctherm_armed(nullptr)");
    ASSERT_NE(gone, std::string::npos);
    EXPECT_LT(gone, resume.find("socThermStart()"));
}

// The GPU's clock count carries SOC_THERM's GPU throttle at its start: a /8 count with depth 7 is thermal, with 0 it is
// on the clock side.
TEST(SwitchSocThermThrottleSource, TheGpuClockCountSamplesTheThermalSide)
{
    const std::string source = switch_ios::test::SourceText("../../TegraGM20B/gpu_stats_clock.c");
    const std::string sample = switch_ios::test::Body(source, "static void clk_sample(");
    EXPECT_THAT(sample, testing::HasSubstr("at->therm = switch_soctherm_gpu_throttle_status();"));
    EXPECT_THAT(switch_ios::test::Body(source, "void tgm_clk_measure("), testing::HasSubstr("counted.therm"));
}

}  // namespace

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return gBlock.regs[static_cast<uint32_t>(address - kSocTherm)];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const auto offset = static_cast<uint32_t>(address - kSocTherm);
    gBlock.writes.push_back(offset);
    const bool gpuVector =
        offset >= kLite && offset <= kOc5 + kGpuOffset && (offset - kLite) % kIndicatorStride == kGpuOffset;
    const uint32_t readOnly = gpuVector ? kLowMedHigh : 0;
    gBlock.regs[offset] = (value & ~readOnly) | (gBlock.regs[offset] & readOnly);
}

uint16_t switch_mmio_model_read16(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write16(uintptr_t, uint16_t)
{}

uint8_t switch_mmio_model_read8(uintptr_t)
{
    return 0;
}

void switch_mmio_model_write8(uintptr_t, uint8_t)
{}
}
