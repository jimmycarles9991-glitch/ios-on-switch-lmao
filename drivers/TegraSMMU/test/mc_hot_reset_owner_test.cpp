// MC_CLIENT_HOTRESET_CTRL (0x200) and CTRL_1 (0x970) hold the hot-reset bits of clients different drivers power: CTRL
// VIC 18, NVENC 11, HC 6, DCB 3; CTRL_1 GPU 2, NVDEC 5, NVJPG 8 (Tegra X1 TRM 18.11.1.56, 18.11.1.114). Two drivers
// powering their engines at once must not lose each other's bit, so tegra_mc_hot_reset is their one writer.
//
// The model runs the real TegraSMMU.cpp, TegraDisplayB.cpp and the GM20B's platform.c on a register file. A second
// driver's power step (tegra_mc_hot_reset for VIC, NVDEC or NVJPG) lands at the first read of its register by anyone
// else: at once if that read is outside the MC's lock, or, as a second CPU spinning on the lock would, right after the
// lock is released.
#include "drivers/SwitchDisplayPort/TegraDisplayB.h"
#include "drivers/TegraGM20B/shim/include/nvxnu.h"
#include "drivers/TegraI2C/TegraI2C.h"
#include "drivers/TegraSMMU/TegraSMMU.h"
#include "kits/SwitchCAR/SwitchCAR.h"
#include "kits/SwitchCAR/SwitchPMC.h"
#include "mc_hot_reset_stubs.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace {
constexpr uintptr_t kMc = 0x70019000;
constexpr uint32_t kCtrl = 0x200, kCtrl1 = 0x970, kStatus1 = 0x974;
constexpr uint32_t kVic = 1u << 18, kDcb = 1u << 3;
constexpr uint32_t kGpu = 1u << 2, kNvdec = 1u << 5, kNvjpg = 1u << 8;
constexpr uint8_t kEnCtrl = 0x06, kEnM1 = 0x01, kEnM4 = 0x40;

std::map<uintptr_t, uint32_t> gRegisters;
int gLocksHeld;
std::vector<std::function<void()>> gSpinning;
std::function<void()> gSecond;
uint32_t gSecondAt;
bool gSecondRan;
uint8_t gEnCtrl;

uint32_t &mc(uint32_t offset)
{
    return gRegisters[kMc + offset];
}

void landsAtTheNextRead(uint32_t offset, std::function<void()> step)
{
    gSecondAt = offset;
    gSecond = std::move(step);
    gSecondRan = false;
}

class McHotReset : public testing::Test {
protected:
    void SetUp() override
    {
        gRegisters.clear();
        gSpinning.clear();
        gSecond = nullptr;
        gSecondRan = false;
        gEnCtrl = kEnM4 | kEnM1;
    }

    TegraDisplayB displayB;
    char why[96] = "";
};

TEST_F(McHotReset, ADpPowerUpKeepsTheVicFlushThatLandsInsideIt)
{
    mc(kCtrl) = kDcb;
    landsAtTheNextRead(kCtrl, [] {
        tegra_mc_hot_reset(kCtrl, kVic, true);
    });
    ASSERT_TRUE(displayB.powerUp(why, sizeof(why))) << why;
    EXPECT_TRUE(gSecondRan);
    EXPECT_EQ(mc(kCtrl), kVic) << "VIC flushed while display B powered up: its client must stay blocked";
}

TEST_F(McHotReset, ADpPowerUpKeepsTheVicReleaseThatLandsInsideIt)
{
    mc(kCtrl) = kDcb | kVic;
    landsAtTheNextRead(kCtrl, [] {
        tegra_mc_hot_reset(kCtrl, kVic, false);
    });
    ASSERT_TRUE(displayB.powerUp(why, sizeof(why))) << why;
    EXPECT_TRUE(gSecondRan);
    EXPECT_EQ(mc(kCtrl), 0u) << "VIC powered on while display B powered up: its client must not stay in hot reset";
}

TEST_F(McHotReset, AGpuFlushKeepsTheNvjpgReleaseThatLandsInsideIt)
{
    ASSERT_EQ(tegra_gm20b_platform_map(), 0);
    mc(kCtrl1) = kNvjpg;
    landsAtTheNextRead(kCtrl1, [] {
        tegra_mc_hot_reset(kCtrl1, kNvjpg, false);
    });
    EXPECT_EQ(tegra_gm20b_mc_flush(1), 0);
    EXPECT_TRUE(gSecondRan);
    EXPECT_EQ(mc(kCtrl1), kGpu) << "NVJPG powered on while the GPU flushed: its client must not stay in hot reset";
}

TEST_F(McHotReset, AGpuReleaseKeepsTheNvdecFlushThatLandsInsideIt)
{
    ASSERT_EQ(tegra_gm20b_platform_map(), 0);
    mc(kCtrl1) = kGpu;
    landsAtTheNextRead(kCtrl1, [] {
        tegra_mc_hot_reset(kCtrl1, kNvdec, true);
    });
    EXPECT_EQ(tegra_gm20b_mc_flush(0), 0);
    EXPECT_TRUE(gSecondRan);
    EXPECT_EQ(mc(kCtrl1), kNvdec) << "NVDEC flushed while the GPU was released: its client must stay blocked";
}

TEST_F(McHotReset, TheGpuRailDownKeepsTheNvjpgReleaseThatLandsInsideIt)
{
    ASSERT_EQ(tegra_gm20b_platform_map(), 0);
    mc(kCtrl1) = kGpu | kNvjpg;
    landsAtTheNextRead(kCtrl1, [] {
        tegra_mc_hot_reset(kCtrl1, kNvjpg, false);
    });
    int vdd = -1;
    EXPECT_EQ(tegra_gm20b_rail_gate(&vdd), 0);
    EXPECT_EQ(vdd, 0);
    EXPECT_TRUE(gSecondRan);
    EXPECT_EQ(gEnCtrl, kEnM4);
    EXPECT_EQ(mc(kCtrl1), kGpu)
        << "NVJPG powered on while the GPU rail went down: its client must not stay in hot reset";
}

// Every other driver and kit names the hot-reset registers only to hand them to tegra_mc_hot_reset.
TEST(McHotResetSource, OnlyTheOwnerWritesTheHotResetRegisters)
{
    const std::regex write(R"((write32\w*|rmw32|\bw32|nvkm_wr32|nvkm_mask)\s*\()");
    const std::regex named(R"(hot_?reset)", std::regex::icase);
    const std::regex offset(R"(\b(g?[mM]c|mc_|mcRegs)\b[^;]*\b0x(200|970)\b)");
    unsigned units = 0;
    std::vector<std::string> writers;
    for (const char *tree : {"../../", "../../../kits/"}) {
        const std::filesystem::path root = switch_ios::test::PackagePath(tree);
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            const std::filesystem::path &path = entry.path();
            const std::string ext = path.extension().string();
            const std::string name = path.generic_string();
            if ((ext != ".c" && ext != ".cpp" && ext != ".h") || name.find("/test/") != std::string::npos ||
                name.ends_with("TegraSMMU/TegraSMMU.cpp"))
                continue;
            ++units;
            std::ifstream file(path, std::ios::binary);
            std::string line;
            for (unsigned number = 1; std::getline(file, line); ++number) {
                if (line.find("write32") == std::string::npos && line.find("rmw32") == std::string::npos &&
                    line.find("w32") == std::string::npos && line.find("nvkm_mask") == std::string::npos)
                    continue;
                if (std::regex_search(line, write) &&
                    (std::regex_search(line, named) || std::regex_search(line, offset)))
                    writers.push_back(name + ":" + std::to_string(number) + ": " + line);
            }
        }
    }
    EXPECT_GT(units, 300u) << "the drivers' and kits' sources are not in the test's data";
    EXPECT_THAT(writers, testing::IsEmpty());
}

TEST(McHotResetSource, TheDisplayAndHost1xHandTheirBitsToTheOwner)
{
    const std::string displayB = switch_ios::test::SourceText("../../SwitchDisplayPort/TegraDisplayB.cpp");
    const std::string host1x = switch_ios::test::SourceText("../../TegraHost1x/TegraHost1x.cpp");
    EXPECT_NE(displayB.find("tegra_mc_hot_reset(kMcHotResetControl, kMcHotResetDcb, false)"), std::string::npos);
    EXPECT_NE(host1x.find("tegra_mc_hot_reset(MC_HOTRESET_CTRL, MC_HOTRESET_HC, false)"), std::string::npos);
}
}  // namespace

IOSimpleLock *IOSimpleLockAlloc()
{
    return new IOSimpleLock{};
}

void IOSimpleLockFree(IOSimpleLock *lock)
{
    delete lock;
}

void IOSimpleLockLock(IOSimpleLock *lock)
{
    EXPECT_FALSE(lock->held) << "a step that found the MC's lock held ran before its release";
    lock->held = true;
    ++gLocksHeld;
}

void IOSimpleLockUnlock(IOSimpleLock *lock)
{
    lock->held = false;
    --gLocksHeld;
    std::vector<std::function<void()>> spinning;
    spinning.swap(gSpinning);
    for (const auto &step : spinning)
        step();
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    if (address == kMc + kStatus1)
        return gRegisters[kMc + kCtrl1];
    const uint32_t value = gRegisters[address];
    if (address == kMc + gSecondAt && gSecond) {
        std::function<void()> second = std::move(gSecond);
        gSecond = nullptr;
        std::function<void()> run = [second] {
            second();
            gSecondRan = true;
        };
        if (gLocksHeld)
            gSpinning.push_back(run);
        else
            run();
    }
    return value;
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gRegisters[address] = value;
}

uint16_t switch_mmio_model_read16(uintptr_t address)
{
    return static_cast<uint16_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write16(uintptr_t address, uint16_t value)
{
    switch_mmio_model_write32(address, value);
}

uint8_t switch_mmio_model_read8(uintptr_t address)
{
    return static_cast<uint8_t>(switch_mmio_model_read32(address));
}

void switch_mmio_model_write8(uintptr_t address, uint8_t value)
{
    switch_mmio_model_write32(address, value);
}

void IOLog(const char *, ...)
{}

void switch_mc_errors_resume(void)
{}

uint32_t tegra_mc_error_latest(uint32_t *intstatus, uint32_t *status, uint32_t *address)
{
    *intstatus = *status = *address = 0;
    return 0;
}

uint64_t mach_absolute_time(void)
{
    return 0;
}

void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *result)
{
    *result = 0;
}

uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}

bool switch_pmc_power_up(unsigned, unsigned, unsigned)
{
    return true;
}

bool switch_pmc_partition_off(unsigned)
{
    return true;
}

void switch_car_rmw(uint32_t, uint32_t, uint32_t)
{}

bool switch_car_clock_enabled(unsigned)
{
    return true;
}

void switch_car_clock_disable(unsigned)
{}

void switch_car_reset_assert(unsigned)
{}

volatile void *switch_linux_ioremap(uint64_t pa, uint64_t, int)
{
    return reinterpret_cast<volatile void *>(static_cast<uintptr_t>(pa));
}

void switch_linux_udelay(uint64_t)
{}

void switch_linux_usleep_range(uint64_t, uint64_t)
{}

void nvxnu_log(const char *, ...)
{}

void switch_car_fuse_readable(void)
{}

void nvxnu_step_impl(const char *, ...)
{}

int tegra_i2c_init(unsigned)
{
    return 0;
}

int tegra_i2c_read(unsigned, uint8_t, uint8_t reg, void *buf, size_t len)
{
    if (reg != kEnCtrl || len != 1)
        return TEGRA_I2C_EIO;
    *static_cast<uint8_t *>(buf) = gEnCtrl;
    return 0;
}

int tegra_i2c_write(unsigned, uint8_t, uint8_t reg, const void *buf, size_t len)
{
    if (reg != kEnCtrl || len != 1)
        return TEGRA_I2C_EIO;
    gEnCtrl = *static_cast<const uint8_t *>(buf);
    return 0;
}
}
