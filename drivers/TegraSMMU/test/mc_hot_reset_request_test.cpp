// tegra_mc_hot_reset reads its request bit back. MC_CLIENT_HOTRESET_STATUS reads done for a client with no flush
// requested (Tegra X1 TRM 18.11.1.114), so a request bit that did not take would pass every later STATUS check: the
// owner names the client in one error line instead. The model runs the real TegraSMMU.cpp on a register file whose
// hot-reset registers can refuse a bit.
#include "drivers/TegraSMMU/TegraSMMU.h"
#include "mc_hot_reset_stubs.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
constexpr uintptr_t kMc = 0x70019000;
constexpr uint32_t kCtrl = 0x200, kCtrl1 = 0x970;

std::map<uintptr_t, uint32_t> gRegisters;
std::map<uintptr_t, uint32_t> gStuckClear;  // bits a register will not set
std::set<uintptr_t> gFrozen;                // registers that ignore writes
std::vector<std::string> gLines;

class McHotResetRequest : public testing::Test {
protected:
    void SetUp() override
    {
        gRegisters.clear();
        gStuckClear.clear();
        gFrozen.clear();
        gLines.clear();
    }
};

TEST_F(McHotResetRequest, ARequestThatTakesLogsNothing)
{
    tegra_mc_hot_reset(kCtrl1, 1u << 8, true);
    EXPECT_EQ(gRegisters[kMc + kCtrl1], 1u << 8);
    tegra_mc_hot_reset(kCtrl1, 1u << 8, false);
    EXPECT_EQ(gRegisters[kMc + kCtrl1], 0u);
    EXPECT_TRUE(gLines.empty());
}

TEST_F(McHotResetRequest, ARequestBitThatDoesNotTakeIsOneLineNamingTheClient)
{
    gStuckClear[kMc + kCtrl1] = 1u << 8;
    tegra_mc_hot_reset(kCtrl1, 1u << 8, true);
    ASSERT_EQ(gLines.size(), 1u);
    EXPECT_EQ(gLines[0], "TegraSMMU: MC hot reset request of NVJPG did not take: MC 0x970 reads 0 after 0x100 was "
                         "written\n");
}

TEST_F(McHotResetRequest, AReleaseThatDoesNotTakeIsOneLineNamingTheClient)
{
    gRegisters[kMc + kCtrl] = 1u << 18;
    gFrozen.insert(kMc + kCtrl);
    tegra_mc_hot_reset(kCtrl, 1u << 18, false);
    ASSERT_EQ(gLines.size(), 1u);
    EXPECT_EQ(gLines[0], "TegraSMMU: MC hot reset release of VIC did not take: MC 0x200 reads 0x40000 after 0 was "
                         "written\n");
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
    lock->held = true;
}

void IOSimpleLockUnlock(IOSimpleLock *lock)
{
    lock->held = false;
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return gRegisters[address];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    if (gFrozen.count(address))
        return;
    gRegisters[address] = value & ~gStuckClear[address];
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

void IOLog(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    gLines.emplace_back(line);
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

void switch_mc_errors_resume(void)
{}
}
