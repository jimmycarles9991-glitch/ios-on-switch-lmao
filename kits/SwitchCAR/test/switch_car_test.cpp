// kits/SwitchCAR against a model of the CAR banks and the PMC: which register each clock and reset ID reaches, the
// power-up order, bounded handshakes, and that concurrent partition toggles never write PWRGATE_TOGGLE while START
// is still set (the race the shared lock closes).
#include "kits/SwitchCAR/SwitchCAR.h"
#include "kits/SwitchCAR/SwitchPMC.h"
#include "switch_car_stubs.h"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr uintptr_t kCar = 0x60006000, kPmc = 0x7000e400;
constexpr uint32_t kDpdSample = 0x20, kToggle = 0x30, kClamp = 0x34, kStatus = 0x38, kDpd = 0x1b8, kDpdStatus = 0x1bc,
                   kDpd2 = 0x1c0, kDpd2Status = 0x1c4;
constexpr uint32_t kStart = 1u << 8;

struct Model {
    std::mutex mutex;
    std::map<uintptr_t, uint32_t> registers;
    std::vector<std::pair<uintptr_t, uint32_t>> writes;
    int startReads = 0;  // reads of PWRGATE_TOGGLE that still show START
    unsigned pending = 0;
    bool stuck = false;
    bool clampsStuck = false;  // REMOVE_CLAMPING_CMD's bits never clear
    int collisions = 0;
};

Model gModel;

void reset()
{
    const std::lock_guard<std::mutex> hold(gModel.mutex);
    gModel.registers.clear();
    gModel.writes.clear();
    gModel.startReads = 0;
    gModel.stuck = false;
    gModel.clampsStuck = false;
    gModel.collisions = 0;
}

std::vector<std::pair<uintptr_t, uint32_t>> writes()
{
    const std::lock_guard<std::mutex> hold(gModel.mutex);
    return gModel.writes;
}

class SwitchCAR : public ::testing::Test {
protected:
    void SetUp() override
    {
        reset();
    }
};

TEST_F(SwitchCAR, IdsReachTheirBanks)
{
    const uint32_t clockSet[] = {0x320, 0x328, 0x330, 0x440, 0x448, 0x284, 0x29c};
    const uint32_t resetClear[] = {0x304, 0x30c, 0x314, 0x434, 0x43c, 0x294, 0x2ac};
    for (unsigned bank = 0; bank < SWITCH_CAR_BANKS; ++bank) {
        reset();
        switch_car_clock_enable(bank * 32 + 5);
        switch_car_reset_deassert(bank * 32 + 31);
        EXPECT_EQ(writes(), (std::vector<std::pair<uintptr_t, uint32_t>>{{kCar + clockSet[bank], 1u << 5},
                                                                         {kCar + resetClear[bank], 1u << 31}}))
            << bank;
    }
    gModel.registers[kCar + 0x280] = 1u << 18;  // CLK_OUT_ENB_X: VIC (Linux 178)
    EXPECT_TRUE(switch_car_clock_enabled(178));
    EXPECT_FALSE(switch_car_clock_enabled(179));
    EXPECT_THROW(switch_car_clock_enable(SWITCH_CAR_MAX_ID), SwitchCarPanic);
}

TEST_F(SwitchCAR, WaitsOnCarRegisters)
{
    gModel.registers[kCar + 0x0e0] = 1u << 27;  // PLLX_BASE locked
    EXPECT_TRUE(switch_car_wait(0x0e0, 1u << 27, 1u << 27, 10));
    EXPECT_FALSE(switch_car_wait(0x0e4, 1u << 27, 1u << 27, 10));
}

TEST_F(SwitchCAR, PowerUpFollowsLinuxOrder)
{
    ASSERT_TRUE(switch_pmc_power_up(2, 178, 178, nullptr));
    EXPECT_EQ(writes(), (std::vector<std::pair<uintptr_t, uint32_t>>{
                            {kPmc + kToggle, 2 | kStart},
                            {kCar + 0x284, 1u << 18},
                            {kCar + 0x290, 1u << 18},
                            {kPmc + kClamp, 1u << 2},
                            {kCar + 0x294, 1u << 18},
                        }));
    EXPECT_TRUE(switch_pmc_partition_is_on(2));
}

// XUSBC (22) as xhci-tegra powers it: clock and reset XUSB_HOST (89), then the client's hot reset released last
// (powergate-t21x.c tegra210_pg_mc_info XUSBC: MC_CLIENT_XUSB_HOST, MC_CLIENT_HOTRESET_CTRL bit 19).
TEST_F(SwitchCAR, PowerUpReleasesTheMemoryClientLast)
{
    constexpr uintptr_t kHotReset = 0x70019000 + 0x200;
    const switch_pmc_mc_client client = {0x200, 1u << 19, [](uint32_t ctrl, uint32_t bit, bool on) {
                                             const std::lock_guard<std::mutex> hold(gModel.mutex);
                                             gModel.writes.emplace_back(0x70019000 + ctrl, on ? bit : ~bit);
                                         }};
    ASSERT_TRUE(switch_pmc_power_up(22, 89, 89, &client));
    EXPECT_EQ(writes(), (std::vector<std::pair<uintptr_t, uint32_t>>{
                            {kPmc + kToggle, 22 | kStart},
                            {kCar + 0x330, 1u << 25},
                            {kCar + 0x310, 1u << 25},
                            {kPmc + kClamp, 1u << 22},
                            {kCar + 0x314, 1u << 25},
                            {kHotReset, ~(1u << 19)},
                        }));
}

// Clamp removal is done when REMOVE_CLAMPING_CMD's own bit reads 0 (Tegra X1 TRM 12.6.14); one that never clears runs
// the power-up to its end and fails.
TEST_F(SwitchCAR, PowerUpWithClampsStuckRunsToItsEndAndFails)
{
    gModel.clampsStuck = true;
    EXPECT_FALSE(switch_pmc_power_up(20, 156, 156, nullptr));
    const auto done = writes();
    ASSERT_FALSE(done.empty());
    EXPECT_EQ(done.back(), (std::pair<uintptr_t, uint32_t>{kCar + 0x43c, 1u << 28}));
}

// PCX is partition 3 but REMOVE_CLAMPING_CMD bit 4 (Tegra X1 TRM 12.6.14, p.467), for the write and the wait alike.
TEST_F(SwitchCAR, PcxIsUnclampedThroughCommandBit4)
{
    ASSERT_TRUE(switch_pmc_unclamp(3));
    EXPECT_EQ(writes(), (std::vector<std::pair<uintptr_t, uint32_t>>{{kPmc + kClamp, 1u << 4}}));
    gModel.clampsStuck = true;
    EXPECT_FALSE(switch_pmc_unclamp(3)) << "bit 4 never cleared";
}

TEST_F(SwitchCAR, T210B01IsChip0x21AtMajorRevision2)
{
    constexpr uintptr_t kHidrev = 0x70000804;
    for (const auto &[hidrev, b01] : {std::pair{0x2121u, true}, std::pair{0x2111u, false}, std::pair{0x2131u, true},
                                      std::pair{0x1821u, false}, std::pair{0u, false}}) {
        gModel.registers[kHidrev] = hidrev;
        EXPECT_EQ(switch_car_soc_t210b01(), b01) << std::hex << hidrev;
    }
}

TEST_F(SwitchCAR, PartitionAlreadyInStateIsNotToggled)
{
    gModel.registers[kPmc + kStatus] = 1u << 9;
    EXPECT_TRUE(switch_pmc_partition_on(9));
    EXPECT_TRUE(switch_pmc_partition_off(10));
    EXPECT_TRUE(writes().empty());
}

TEST_F(SwitchCAR, UnacknowledgedToggleFailsWithinItsBound)
{
    gModel.stuck = true;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(switch_pmc_partition_on(3));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST_F(SwitchCAR, ConcurrentTogglesNeverCollide)
{
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (unsigned partition = 0; partition < 4; ++partition) {
        threads.emplace_back([partition] {
            for (int i = 0; i < 200; ++i) {
                EXPECT_TRUE(switch_pmc_partition_on(partition));
                EXPECT_TRUE(switch_pmc_partition_off(partition));
            }
        });
    }
    for (auto &thread : threads)
        thread.join();
    EXPECT_EQ(gModel.collisions, 0);
}

// Each IO deep power-down request is preceded by DPD_SAMPLE (Tegra X1 TRM 12.6.111) and the sample released after it.
TEST_F(SwitchCAR, IoPadsUseTheirRegister)
{
    EXPECT_TRUE(switch_pmc_io_pad_on(28));
    EXPECT_TRUE(switch_pmc_io_pad_on(39));
    EXPECT_TRUE(switch_pmc_io_pad_off(2));
    EXPECT_EQ(writes(), (std::vector<std::pair<uintptr_t, uint32_t>>{
                            {kPmc + kDpdSample, 1},
                            {kPmc + kDpd, (1u << 30) | (1u << 28)},
                            {kPmc + kDpdSample, 0},
                            {kPmc + kDpdSample, 1},
                            {kPmc + kDpd2, (1u << 30) | (1u << 7)},
                            {kPmc + kDpdSample, 0},
                            {kPmc + kDpdSample, 1},
                            {kPmc + kDpd, (2u << 30) | (1u << 2)},
                            {kPmc + kDpdSample, 0},
                        }));
}
}  // namespace

// The PMC model: a toggle stays in flight (START set, status unchanged) for three more reads of the PMC, then flips
// the partition's status bit and drops START. Clamp removal clears its command bit at once. DPD requests settle at once
// (code 1 takes the pad out of deep power down, code 2 puts it in).
extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    const std::lock_guard<std::mutex> hold(gModel.mutex);
    if ((address == kPmc + kToggle || address == kPmc + kStatus) && gModel.startReads > 0) {
        --gModel.startReads;
        if (gModel.startReads == 0) {
            gModel.registers[kPmc + kStatus] ^= 1u << gModel.pending;
            gModel.registers[kPmc + kToggle] &= ~kStart;
        }
    }
    return gModel.registers[address];
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const std::lock_guard<std::mutex> hold(gModel.mutex);
    gModel.writes.emplace_back(address, value);
    if (address == kPmc + kToggle) {
        if (gModel.startReads > 0)
            ++gModel.collisions;
        gModel.pending = value & 0x1f;
        gModel.startReads = gModel.stuck ? 1 << 30 : 3;
        gModel.registers[address] = value;
        return;
    }
    if (address == kPmc + kClamp && !gModel.clampsStuck) {
        gModel.registers[address] = 0;
        return;
    }
    if (address == kPmc + kDpd || address == kPmc + kDpd2) {
        const uintptr_t status = address == kPmc + kDpd ? kPmc + kDpdStatus : kPmc + kDpd2Status;
        const uint32_t mask = value & 0x3fffffff;
        if ((value >> 30) == 1)
            gModel.registers[status] &= ~mask;
        if ((value >> 30) == 2)
            gModel.registers[status] |= mask;
    }
    gModel.registers[address] = value;
}

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
