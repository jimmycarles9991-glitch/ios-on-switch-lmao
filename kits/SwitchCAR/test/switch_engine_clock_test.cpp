// kits/SwitchCAR/SwitchEngineClock against L4T's numbers and a model of PLLC2/PLLC3: the 637 mV rate per SoC process,
// the BASE it takes, the bring-up order from off, and PLLP whenever the PLL is someone else's or does not lock.
#include "kits/SwitchCAR/SwitchEngineClock.h"
#include "switch_car_stubs.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdarg>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
constexpr uintptr_t kCar = 0x60006000, kFuseSpeedo = 0x7000F800 + 0x134, kHidrev = 0x70000804;
constexpr uintptr_t kC2Base = kCar + 0x4e8, kC2Misc0 = kCar + 0x4ec, kC2Misc1 = kCar + 0x4f0;
constexpr uintptr_t kC3Base = kCar + 0x4fc, kC3Misc0 = kCar + 0x500, kC3Misc1 = kCar + 0x504;
constexpr uint32_t kEnable = 1u << 30, kLock = 1u << 26, kReset = 1u << 30, kIddq = 1u << 27;
constexpr uint32_t kB01 = (0x21u << 8) | (2u << 4);

struct Model {
    std::mutex mutex;
    std::map<uintptr_t, uint32_t> registers;
    std::vector<std::pair<uintptr_t, uint32_t>> writes;
    std::vector<std::string> lines;
    bool locks = true;
};

Model gModel;

void reset(int32_t speedo, uint32_t hidrev = kB01)
{
    std::lock_guard<std::mutex> hold(gModel.mutex);
    gModel.registers.clear();
    gModel.writes.clear();
    gModel.lines.clear();
    gModel.locks = true;
    gModel.registers[kFuseSpeedo] = static_cast<uint32_t>(speedo);
    gModel.registers[kHidrev] = hidrev;
}

std::vector<std::pair<uintptr_t, uint32_t>> pllWrites(uintptr_t first, uintptr_t last)
{
    std::lock_guard<std::mutex> hold(gModel.mutex);
    std::vector<std::pair<uintptr_t, uint32_t>> found;
    for (const auto &write : gModel.writes) {
        if (write.first >= first && write.first <= last)
            found.push_back(write);
    }
    return found;
}

std::string lines()
{
    std::lock_guard<std::mutex> hold(gModel.mutex);
    std::string all;
    for (const auto &line : gModel.lines)
        all += line;
    return all;
}

void preset(uintptr_t address, uint32_t value)
{
    std::lock_guard<std::mutex> hold(gModel.mutex);
    gModel.registers[address] = value;
}

constexpr uint32_t base(uint32_t p, uint32_t n)
{
    return (p << 20) | (n << 10) | 2u;
}
}  // namespace

// A PLLC2/PLLC3 BASE reads its lock bit once it is enabled out of IDDQ and reset.
extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    std::lock_guard<std::mutex> hold(gModel.mutex);
    uint32_t value = gModel.registers[address];
    for (auto [pllBase, misc0, misc1] :
         {std::tuple{kC2Base, kC2Misc0, kC2Misc1}, std::tuple{kC3Base, kC3Misc0, kC3Misc1}}) {
        if (address == pllBase && (value & kEnable) && !(gModel.registers[misc0] & kReset) &&
            !(gModel.registers[misc1] & kIddq) && gModel.locks)
            value |= kLock;
    }
    return value;
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    std::lock_guard<std::mutex> hold(gModel.mutex);
    gModel.writes.emplace_back(address, value);
    gModel.registers[address] = value;
}

extern "C" uint16_t switch_mmio_model_read16(uintptr_t)
{
    return 0;
}

extern "C" void switch_mmio_model_write16(uintptr_t, uint16_t)
{}

extern "C" uint8_t switch_mmio_model_read8(uintptr_t)
{
    return 0;
}

extern "C" void switch_mmio_model_write8(uintptr_t, uint8_t)
{}

extern "C" uint64_t mach_absolute_time(void)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

extern "C" void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = mach_absolute_time() + (uint64_t)interval * scale;
}

extern "C" uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}

extern "C" void IOLog(const char *format, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    std::lock_guard<std::mutex> hold(gModel.mutex);
    gModel.lines.emplace_back(line);
}

TEST(SwitchEngineClockPlan, SocProcessIsL4TsT210B01Corners)
{
    EXPECT_EQ(switch_engine_soc_process(0), -1);
    EXPECT_EQ(switch_engine_soc_process(-5), -1);
    EXPECT_EQ(switch_engine_soc_process(1597), 0);
    EXPECT_EQ(switch_engine_soc_process(1598), 1);
    EXPECT_EQ(switch_engine_soc_process(1698), 1);  // this unit (TegraDFLL, 11-11: "SoC Speedo Value 1698")
    EXPECT_EQ(switch_engine_soc_process(1708), 1);
    EXPECT_EQ(switch_engine_soc_process(1709), 2);
}

TEST(SwitchEngineClockPlan, VminColumn)
{
    EXPECT_EQ(switch_engine_vmin_khz(SWITCH_ENGINE_BUS_C3, 0), 364800u);
    EXPECT_EQ(switch_engine_vmin_khz(SWITCH_ENGINE_BUS_C3, 1), 460800u);
    EXPECT_EQ(switch_engine_vmin_khz(SWITCH_ENGINE_BUS_C3, 2), 556800u);
    EXPECT_EQ(switch_engine_vmin_khz(SWITCH_ENGINE_BUS_C2, 0), 307200u);
    EXPECT_EQ(switch_engine_vmin_khz(SWITCH_ENGINE_BUS_C2, 1), 422400u);
    EXPECT_EQ(switch_engine_vmin_khz(SWITCH_ENGINE_BUS_C2, 2), 499200u);
    EXPECT_EQ(switch_engine_vmin_khz(SWITCH_ENGINE_BUS_C3, -1), 0u);
    EXPECT_EQ(switch_engine_vmin_khz(1, 1), 0u);
}

TEST(SwitchEngineClockPlan, PllBaseIsL4TsFixedMDivider)
{
    EXPECT_EQ(switch_engine_pllcx_base(460800), base(1, 48));  // 38.4 / 2 * 48 / 2
    EXPECT_EQ(switch_engine_pllcx_base(556800), base(1, 58));
    EXPECT_EQ(switch_engine_pllcx_base(422400), base(1, 44));
    EXPECT_EQ(switch_engine_pllcx_base(499200), base(1, 52));
    EXPECT_EQ(switch_engine_pllcx_base(998400), base(0, 52));
    EXPECT_EQ(switch_engine_pllcx_base(408000), 0u);  // 816 MHz is no multiple of 19.2 MHz
    EXPECT_EQ(switch_engine_pllcx_base(0), 0u);
    EXPECT_EQ(switch_engine_pllcx_base(1300000), 0u);
}

TEST(SwitchEngineClockPlan, ProcessZeroAndUnfusedKeepPllp)
{
    const auto p0 = switch_engine_clock_plan_for(SWITCH_ENGINE_BUS_C3, 1500);
    EXPECT_EQ(p0.khz, SWITCH_ENGINE_PLLP_KHZ);
    EXPECT_EQ(p0.pll_base, 0u);
    const auto unfused = switch_engine_clock_plan_for(SWITCH_ENGINE_BUS_C3, 0);
    EXPECT_EQ(unfused.khz, SWITCH_ENGINE_PLLP_KHZ);
    EXPECT_EQ(unfused.pll_base, 0u);
    const auto p1 = switch_engine_clock_plan_for(SWITCH_ENGINE_BUS_C3, 1698);
    EXPECT_EQ(p1.khz, 460800u);
    EXPECT_EQ(p1.pll_base, base(1, 48));
}

TEST(SwitchEngineClock, BringsPllc3UpInL4TsOrderOnce)
{
    reset(1698);
    uint32_t khz = 0;
    EXPECT_EQ(switch_engine_clock_source(SWITCH_ENGINE_BUS_C3, &khz), SWITCH_ENGINE_SOURCE_PLLC3);
    EXPECT_EQ(khz, 460800u);
    const std::vector<std::pair<uintptr_t, uint32_t>> expected = {
        {kC3Base + 0x4, 0x40080000},  {kC3Base + 0x8, 0x08000000},     {kC3Base + 0xc, 0x1f720f05},
        {kC3Base + 0x10, 0x000000c4}, {kC3Base, base(1, 48)},          {kC3Base + 0x8, 0x00000000},
        {kC3Base + 0x4, 0x00080000},  {kC3Base, base(1, 48) | kEnable}};
    EXPECT_EQ(pllWrites(kC3Base, kC3Base + 0x10), expected);
    EXPECT_NE(lines().find("PLLC3 for NVDEC and NVENC: 460800 kHz, L4T's 637 mV point (BASE 0 as found"),
              std::string::npos)
        << lines();
    EXPECT_NE(lines().find("SoC speedo 1698, process 1"), std::string::npos);
    const auto fuseClock = pllWrites(kCar + 0x328, kCar + 0x328);
    ASSERT_FALSE(fuseClock.empty());
    EXPECT_EQ(fuseClock.front().second, 1u << 7);
    const auto visible = pllWrites(kCar + 0x48, kCar + 0x48);
    ASSERT_FALSE(visible.empty());
    EXPECT_EQ(visible.back().second & (1u << 28), 1u << 28);

    EXPECT_EQ(switch_engine_clock_source(SWITCH_ENGINE_BUS_C3, &khz), SWITCH_ENGINE_SOURCE_PLLC3);
    EXPECT_EQ(khz, 460800u);
    EXPECT_EQ(pllWrites(kC3Base, kC3Base + 0x10).size(), expected.size());
    EXPECT_TRUE(pllWrites(kC2Base, kC2Base + 0x10).empty());
}

TEST(SwitchEngineClock, Pllc2ForNvjpg)
{
    reset(1698);
    uint32_t khz = 0;
    EXPECT_EQ(switch_engine_clock_source(SWITCH_ENGINE_BUS_C2, &khz), SWITCH_ENGINE_SOURCE_PLLC2);
    EXPECT_EQ(khz, 422400u);
    EXPECT_EQ(pllWrites(kC2Base, kC2Base).back().second, base(1, 44) | kEnable);
    EXPECT_TRUE(pllWrites(kC3Base, kC3Base + 0x10).empty());
}

TEST(SwitchEngineClock, SomeoneElsesPllKeepsPllp)
{
    reset(1698);
    preset(kC3Base, kEnable | base(1, 40) | kLock);
    uint32_t khz = 0;
    EXPECT_EQ(switch_engine_clock_source(SWITCH_ENGINE_BUS_C3, &khz), SWITCH_ENGINE_SOURCE_PLLP);
    EXPECT_EQ(khz, SWITCH_ENGINE_PLLP_KHZ);
    EXPECT_TRUE(pllWrites(kC3Base, kC3Base + 0x10).empty());
    EXPECT_NE(lines().find("running with dividers that are not ours"), std::string::npos) << lines();
}

TEST(SwitchEngineClock, NoLockPutsThePllBackOff)
{
    reset(1698);
    {
        std::lock_guard<std::mutex> hold(gModel.mutex);
        gModel.locks = false;
    }
    uint32_t khz = 0;
    EXPECT_EQ(switch_engine_clock_source(SWITCH_ENGINE_BUS_C2, &khz), SWITCH_ENGINE_SOURCE_PLLP);
    EXPECT_EQ(khz, SWITCH_ENGINE_PLLP_KHZ);
    const auto writes = pllWrites(kC2Base, kC2Base + 0x10);
    ASSERT_GE(writes.size(), 3u);
    EXPECT_EQ(writes[writes.size() - 3], std::make_pair(kC2Base, base(1, 44)));
    EXPECT_EQ(writes[writes.size() - 2].second & kReset, kReset);
    EXPECT_EQ(writes.back().second & kIddq, kIddq);
    EXPECT_NE(lines().find("no lock in 600 us"), std::string::npos) << lines();
}

TEST(SwitchEngineClock, ProcessZeroAndOtherChipsStayOnPllp)
{
    for (auto [speedo, hidrev] :
         {std::pair{1500, kB01}, std::pair{1698, (0x21u << 8) | (1u << 4)}, std::pair{0, kB01}}) {
        reset(speedo, hidrev);
        uint32_t khz = 0;
        EXPECT_EQ(switch_engine_clock_source(SWITCH_ENGINE_BUS_C3, &khz), SWITCH_ENGINE_SOURCE_PLLP);
        EXPECT_EQ(khz, SWITCH_ENGINE_PLLP_KHZ);
        EXPECT_TRUE(pllWrites(kC3Base, kC3Base + 0x10).empty());
    }
}
