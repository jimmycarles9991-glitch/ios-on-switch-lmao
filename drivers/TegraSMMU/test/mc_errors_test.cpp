// The memory controller's error interrupt (TegraMCErrors.cpp, TegraMCErrors.h) on a register file: the filter runs with
// interrupts off and takes, then clears, only the error bits (Tegra X1 TRM 18.11.1.1-4); the action prints one line
// naming the kind, client and address and publishes the error that GM20B's and the engines' lines read.
#include "mc_errors_stubs.h"
#include "drivers/TegraSMMU/TegraMCErrors.h"
#include "drivers/TegraSMMU/TegraSMMU.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uintptr_t kMc = 0x70019000;
std::map<uintptr_t, uint32_t> gRegisters;
std::vector<std::pair<uint32_t, uint32_t>> gWrites;
std::vector<uint32_t> gReads;

uint32_t &mc(uint32_t offset)
{
    return gRegisters[kMc + offset];
}

IOFilterInterruptEventSource *started()
{
    static IOFilterInterruptEventSource *source = [] {
        EXPECT_EQ(tegra_mc_errors_start(), 0);
        return IOFilterInterruptEventSource::last();
    }();
    return source;
}

class McErrors : public testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_NE(started(), nullptr);
        gRegisters.clear();
        gWrites.clear();
        gReads.clear();
        switch_test_lines.clear();
    }
};

TEST_F(McErrors, TheMaskUnmasksTheErrorsButNotTheArbitrationWarningAndSleepRestoresIt)
{
    EXPECT_TRUE(started()->enabled);
    switch_mc_errors_resume();
    EXPECT_EQ(mc(0x4), 1u << 6 | 1u << 8 | 1u << 10 | 1u << 11 | 1u << 12 | 1u << 13 | 1u << 16 | 1u << 17);
}

// Boot 19's capture: ERR_STATUS 0x20000080 at 0, DECERR_EMEM by client 0x80 csr_sesrd.
TEST_F(McErrors, TheFilterTakesTheCaptureBeforeClearingAndTheActionNamesTheClient)
{
    mc(0x0) = 1u << 6 | 1u << 9;
    mc(0x8) = 0x20000080;
    mc(0xc) = 0;
    ASSERT_TRUE(started()->fire());
    ASSERT_EQ(gWrites.size(), 1u);
    EXPECT_EQ(gWrites[0], std::make_pair(0x0u, 1u << 6))
        << "only the error bit is cleared, ARBITRATION_EMEM is not ours";
    EXPECT_EQ(gReads, (std::vector<uint32_t>{0x0, 0x8, 0xc})) << "ERR_STATUS and ERR_ADR before the clear";
    ASSERT_EQ(switch_test_lines.size(), 1u);
    EXPECT_EQ(switch_test_lines[0], "TegraSMMU: MC error DECERR_EMEM: DECERR_EMEM (no DRAM at the address), a read by "
                                    "csr_sesrd (SE) (client 0x80) at 0; INTSTATUS 0x40 ERR_STATUS 0x20000080 ERR_ADR "
                                    "0\n");
    uint32_t intstatus = 0, status = 0, address = 0;
    EXPECT_GE(tegra_mc_error_latest(&intstatus, &status, &address), 1u);
    EXPECT_EQ(intstatus, 1u << 6);
    EXPECT_EQ(status, 0x20000080u);
}

// The probe log's VIC fault: INVALID_SMMU_PAGE, ERR_STATUS 0x6000006c at 0x4001d400, a read by csr_vicsrd.
TEST_F(McErrors, AnSmmuFaultNamesThePagesPermissions)
{
    mc(0x0) = 1u << 10;
    mc(0x8) = 0x6a00006c;
    mc(0xc) = 0x4001d400;
    ASSERT_TRUE(started()->fire());
    ASSERT_EQ(switch_test_lines.size(), 1u);
    EXPECT_EQ(switch_test_lines[0], "TegraSMMU: MC error INVALID_SMMU_PAGE: INVALID_SMMU_PAGE (page readable 1 "
                                    "writable 0 nonsecure 1), a read by csr_vicsrd (VIC) (client 0x6c) at 0x4001d400; "
                                    "INTSTATUS 0x400 ERR_STATUS 0x6a00006c ERR_ADR 0x4001d400\n");
}

TEST_F(McErrors, AnInterruptWithNoErrorBitTakesNothing)
{
    mc(0x0) = 1u << 9;
    EXPECT_FALSE(started()->fire());
    EXPECT_TRUE(gWrites.empty());
    EXPECT_TRUE(switch_test_lines.empty());
}

TEST(McErrorsDescribe, AKindWithItsOwnCaptureRegistersIsNamedAlone)
{
    char line[256];
    EXPECT_STREQ(switch_mc::describe({1u << 12, 0x6000006c, 0x1000}, line, sizeof(line)),
                 "DECERR_VPR (not captured in MC_ERR_STATUS); INTSTATUS 0x1000");
}
}  // namespace

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    gReads.push_back(static_cast<uint32_t>(address - kMc));
    return gRegisters[address];
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gWrites.emplace_back(static_cast<uint32_t>(address - kMc), value);
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

uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return &nub;
}
