// SwitchMMIO.h and SwitchMMIOKernel.h against a register model and a clock that ticks once per look.
#include "kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "kits/SwitchMMIO/SwitchMMIOWords.h"

#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <vector>

namespace {
std::map<uintptr_t, uint32_t> gRegisters;
std::vector<std::pair<uintptr_t, uint32_t>> gWrites;
std::function<uint32_t(uintptr_t, uint32_t)> gOnRead;
uint64_t gNow, gDeadlines;
uint8_t gBlock[0x100];

volatile uint8_t *base()
{
    return gBlock;
}

uintptr_t at(uint32_t offset)
{
    return reinterpret_cast<uintptr_t>(gBlock) + offset;
}

class SwitchMMIO : public ::testing::Test {
protected:
    void SetUp() override
    {
        gRegisters.clear();
        gWrites.clear();
        gOnRead = nullptr;
        gNow = 1000;
        gDeadlines = 0;
    }
};

TEST_F(SwitchMMIO, ReadWriteAndModify)
{
    gRegisters[at(0x10)] = 0xf0f0f0f0;
    EXPECT_EQ(switch_mmio_read32(base(), 0x10), 0xf0f0f0f0u);
    switch_mmio_rmw32(base(), 0x10, 0x000000f0, 0x00000003);
    EXPECT_EQ(gRegisters[at(0x10)], 0xf0f0f003u);
    switch_mmio_write32_publish(base(), 0x14, 7);
    EXPECT_EQ(gWrites.back(), std::make_pair(at(0x14), 7u));
}

TEST_F(SwitchMMIO, FlushedWriteIsReadBack)
{
    std::vector<uintptr_t> reads;
    gOnRead = [&](uintptr_t address, uint32_t value) {
        reads.push_back(address);
        EXPECT_EQ(gWrites.size(), 1u);
        return value;
    };
    switch_mmio_write32_flush(base(), 0x18, 0x55);
    EXPECT_EQ(gWrites.back(), std::make_pair(at(0x18), 0x55u));
    EXPECT_EQ(reads, std::vector<uintptr_t>{at(0x18)});
}

// The display's register blocks, numbered in words: each index is a read or a write of that word, nothing cached.
TEST_F(SwitchMMIO, WordBlocksReadAndWriteTheWord)
{
    const SwitchMMIOWords regs(base());
    regs[0x10] = 0x0f;
    EXPECT_EQ(gWrites.back(), std::make_pair(at(0x40), 0x0fu));
    regs[0x10] |= 0x30;
    EXPECT_EQ(gRegisters[at(0x40)], 0x3fu);
    regs[0x10] &= ~0x01u;
    EXPECT_EQ(gRegisters[at(0x40)], 0x3eu);
    regs[0x11] = regs[0x10];
    EXPECT_EQ(gRegisters[at(0x44)], 0x3eu);
    gRegisters[at(0x48)] = 7;
    const uint32_t value = regs[0x12];
    EXPECT_EQ(value, 7u);
    const SwitchMMIOWords window = regs + 0x10;
    window[2] = 9;
    EXPECT_EQ(gRegisters[at(0x48)], 9u);
    EXPECT_TRUE(static_cast<bool>(regs));
    EXPECT_FALSE(static_cast<bool>(SwitchMMIOWords()));
}

TEST_F(SwitchMMIO, FieldMacros)
{
    static_assert(SWITCH_BIT(5) == 0x20u);
    static_assert(SWITCH_FIELD_GET(0x00f0u, 0x1234u) == 0x3u);
    static_assert(SWITCH_FIELD_PREP(0x00f0u, 0x5u) == 0x50u);
    static_assert(SWITCH_FIELD_PREP(0x00f0u, 0x15u) == 0x50u);
    static_assert(SWITCH_FIELD_GET(0xff00000000ull, 0xab12345678ull) == 0xabu);
}

TEST_F(SwitchMMIO, WaitThatIsAlreadyDoneTakesNoDeadline)
{
    gRegisters[at(0x20)] = 0x4;
    EXPECT_TRUE(switch_mmio_wait32(base(), 0x20, 0x4, 0x4, 10));
    EXPECT_EQ(gDeadlines, 0u);
}

TEST_F(SwitchMMIO, WaitReturnsOnTheReadThatShowsTheBits)
{
    int reads = 0;
    gOnRead = [&](uintptr_t, uint32_t value) {
        return ++reads >= 5 ? value | 1u : value;
    };
    EXPECT_TRUE(switch_mmio_wait32(base(), 0x20, 1, 1, 1000));
    EXPECT_EQ(reads, 5);
    EXPECT_EQ(gDeadlines, 1u);
}

TEST_F(SwitchMMIO, WaitTimesOutAfterItsBound)
{
    EXPECT_FALSE(switch_mmio_wait32(base(), 0x20, 1, 1, 50));
    EXPECT_GE(gNow, 1000u + 50u * 1000u);
}

TEST_F(SwitchMMIO, WaitRechecksOnceAfterTheDeadline)
{
    gOnRead = [&](uintptr_t, uint32_t value) {
        return gNow >= 1000 + 50 * 1000 ? value | 1u : value;
    };
    EXPECT_TRUE(switch_mmio_wait32(base(), 0x20, 1, 1, 50));
}
}  // namespace

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    const uint32_t value = gRegisters[address];
    return gOnRead ? gOnRead(address, value) : value;
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gRegisters[address] = value;
    gWrites.emplace_back(address, value);
}

// One tick per look at the clock, in nanoseconds.
extern "C" uint64_t mach_absolute_time(void)
{
    return gNow += 1;
}

extern "C" void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    ++gDeadlines;
    *deadline = gNow + (uint64_t)interval * scale;
}

extern "C" uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}
