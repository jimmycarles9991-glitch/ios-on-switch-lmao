// SwitchFalconInterrupt.cpp and SwitchFalconError.h on a register file. The falcon's error reaches the CPU through
// THI's FALCON_INT on the engine's own line (Tegra X1 TRM 15.4.1.4, 15.5.8): the filter, with interrupts off, only
// reads the error (the falcon stays parked on it); the action turns the line off and hands the error on; the
// cancellation clears it as L4T's nvhost_flcn_common_isr does; a pending error the filter never took is a panic.
#include "switch_falcon_interrupt_test_stubs.h"
#include "../SwitchFalconInterrupt.cpp"

#include <gtest/gtest.h>

#include <cstring>
#include <utility>
#include <vector>

namespace {
alignas(4096) uint8_t gEngine[0x2000];
std::vector<std::pair<uint32_t, uint32_t>> gWrites;
std::vector<switch_falcon_error> gFailed;

uint32_t &reg(uint32_t offset)
{
    return *reinterpret_cast<uint32_t *>(gEngine + offset);
}

void failed(const switch_falcon_error &error)
{
    gFailed.push_back(error);
}

// The OS's application error: MAILBOX0 MSDECOS_ERROR_APPLICATION, MAILBOX1 its code, SWGEN0 raised to the host.
void raise()
{
    reg(SWITCH_FALCON_THI_INT_STATUS) = 1;
    reg(SWITCH_FALCON_ERR_IRQSTAT) = 1u << 6;
    reg(SWITCH_FALCON_ERR_IRQDEST) = 0xfff0;
    reg(SWITCH_FALCON_ERR_MAILBOX0) = 8;
    reg(SWITCH_FALCON_ERR_MAILBOX1) = 0x1234;
}

SwitchFalconInterrupt line;
IOFilterInterruptEventSource *source;

class FalconInterrupt : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(line.start("TegraTest", 40, gEngine, failed));
        source = IOWorkLoop::last();
        ASSERT_NE(source, nullptr);
    }

    void SetUp() override
    {
        memset(gEngine, 0, sizeof(gEngine));
        gWrites.clear();
        gFailed.clear();
        switch_test_lines.clear();
        line.off();
    }
};

TEST_F(FalconInterrupt, TheFilterOnlyReadsTheParkedErrorAndTheActionTurnsTheLineOff)
{
    line.on();
    EXPECT_EQ(gWrites, (std::vector<std::pair<uint32_t, uint32_t>>{{SWITCH_FALCON_THI_INT_MASK, 1}}))
        << "on() enables THI's falcon interrupt";
    gWrites.clear();
    raise();
    ASSERT_TRUE(source->fire());
    EXPECT_TRUE(gWrites.empty()) << "the falcon must stay parked: nothing is cleared before the cancellation";
    ASSERT_EQ(gFailed.size(), 1u);
    EXPECT_EQ(gFailed[0].mailbox0, 8u);
    EXPECT_EQ(gFailed[0].mailbox1, 0x1234u);
    EXPECT_FALSE(source->enabled) << "the line stays asserted until the reset: it must not come back on by itself";
    char why[256];
    ASSERT_TRUE(line.failed(why, sizeof(why)));
    EXPECT_STREQ(why, "falcon error interrupt: MAILBOX0 0x8 (MSDECOS_ERROR_APPLICATION) MAILBOX1 0x1234; IRQSTAT 0x40 "
                      "(host 0x40) EXCI 0 CPUCTL 0");
    line.on();
    EXPECT_FALSE(line.failed(why, sizeof(why))) << "on() after the reset starts a new epoch";
}

TEST_F(FalconInterrupt, AnEngineThatIsOffIsNotRead)
{
    line.on();
    line.off();
    raise();
    source->enabled = true;  // an interrupt already on its way when the line was turned off
    EXPECT_FALSE(source->fire());
    EXPECT_TRUE(gFailed.empty());
}

TEST_F(FalconInterrupt, TheCancellationClearsAsNvhostDoes)
{
    raise();
    switch_falcon_error_clear(gEngine);
    EXPECT_EQ(gWrites, (std::vector<std::pair<uint32_t, uint32_t>>{{SWITCH_FALCON_THI_INT_STATUS, 1},
                                                                   {SWITCH_FALCON_ERR_IRQSCLR, 1u << 6}}));
}

TEST_F(FalconInterrupt, AnErrorPendingThroughACancellationThatTheFilterNeverTookIsAPanic)
{
    line.on();
    raise();
    line.lostCheckStart();
    EXPECT_THROW(line.lostCheckEnd(), std::runtime_error);
    line.lostCheckStart();
    ASSERT_TRUE(source->fire());
    EXPECT_NO_THROW(line.lostCheckEnd()) << "taken while the check ran";
}
}  // namespace

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return *reinterpret_cast<const uint32_t *>(address);
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gWrites.emplace_back(static_cast<uint32_t>(address - reinterpret_cast<uintptr_t>(gEngine)), value);
    *reinterpret_cast<uint32_t *>(address) = value;
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
