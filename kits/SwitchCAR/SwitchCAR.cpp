// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchCAR.h"
#include "SwitchPMC.h"
#include <IOKit/IOLib.h>
#include "../SwitchMMIO/SwitchMMIOKernel.h"

namespace {
struct Bank {
    uint32_t clockOut, clockSet, clockClear, reset, resetSet, resetClear;
};

// Linux drivers/clk/tegra/clk.c periph_regs, banks L H U V W X Y.
constexpr Bank kBanks[SWITCH_CAR_BANKS] = {
    {0x010, 0x320, 0x324, 0x004, 0x300, 0x304}, {0x014, 0x328, 0x32c, 0x008, 0x308, 0x30c},
    {0x018, 0x330, 0x334, 0x00c, 0x310, 0x314}, {0x360, 0x440, 0x444, 0x358, 0x430, 0x434},
    {0x364, 0x448, 0x44c, 0x35c, 0x438, 0x43c}, {0x280, 0x284, 0x288, 0x28c, 0x290, 0x294},
    {0x298, 0x29c, 0x2a0, 0x2a4, 0x2a8, 0x2ac},
};

enum : uint32_t {
    PWRGATE_TOGGLE = 0x30,
    REMOVE_CLAMPING = 0x34,
    PWRGATE_STATUS = 0x38,
    IO_DPD_REQ = 0x1b8,
    IO_DPD_STATUS = 0x1bc,
    IO_DPD2_REQ = 0x1c0,
    IO_DPD2_STATUS = 0x1c4,
};

constexpr uint32_t kToggleStart = 1u << 8;
constexpr uint32_t kDpdCodeOff = 1u << 30, kDpdCodeOn = 2u << 30;
constexpr uint32_t kToggleUs = 5000, kDpdUs = 250, kSequenceStepUs = 10;

volatile uint8_t *gCar, *gPmc;
IOLock *gLock;

// First use is in a driver's start (thread context); each pointer is published once and never changes. A losing
// racer's spare mapping or lock is dropped: a page or a few bytes, at most once per boot.
template <typename T, typename Make> T once(T *slot, Make make)
{
    T value = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (value)
        return value;
    T made = make();
    if (__atomic_compare_exchange_n(slot, &value, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return made;
    return value;
}

volatile uint8_t *car()
{
    return once(&gCar, [] {
        return switch_mmio_map(SWITCH_CAR_PA, 0x1000);
    });
}

volatile uint8_t *pmc()
{
    return once(&gPmc, [] {
        return switch_mmio_map(SWITCH_PMC_PA & ~0xfffULL, 0x1000) + 0x400;
    });
}

IOLock *lock()
{
    return once(&gLock, [] {
        return IOLockAlloc();
    });
}

const Bank &bankOf(unsigned id)
{
    if (id >= SWITCH_CAR_MAX_ID)
        panic("SwitchCAR: clock or reset %u is past bank Y", id);
    return kBanks[id / 32];
}

uint32_t bitOf(unsigned id)
{
    return 1u << (id % 32);
}

bool toggleLocked(unsigned partition, bool on)
{
    volatile uint8_t *p = pmc();
    if ((((switch_mmio_read32(p, PWRGATE_STATUS) >> partition) & 1) != 0) == on)
        return true;
    if (!switch_mmio_wait32(p, PWRGATE_TOGGLE, kToggleStart, 0, kToggleUs))
        return false;
    switch_mmio_write32(p, PWRGATE_TOGGLE, partition | kToggleStart);
    return switch_mmio_wait32(p, PWRGATE_STATUS, 1u << partition, on ? 1u << partition : 0, kToggleUs);
}

bool toggle(unsigned partition, bool on)
{
    IOLockLock(lock());
    const bool done = toggleLocked(partition, on);
    IOLockUnlock(lock());
    return done;
}

bool pad(unsigned dpd, uint32_t code)
{
    const bool second = dpd >= 32;
    const uint32_t mask = 1u << (dpd % 32);
    volatile uint8_t *p = pmc();
    IOLockLock(lock());
    switch_mmio_write32(p, second ? IO_DPD2_REQ : IO_DPD_REQ, code | mask);
    const bool done =
        switch_mmio_wait32(p, second ? IO_DPD2_STATUS : IO_DPD_STATUS, mask, code == kDpdCodeOn ? mask : 0, kDpdUs);
    IOLockUnlock(lock());
    return done;
}
}  // namespace

void switch_car_clock_enable(unsigned id)
{
    switch_mmio_write32(car(), bankOf(id).clockSet, bitOf(id));
}

void switch_car_clock_disable(unsigned id)
{
    switch_mmio_write32(car(), bankOf(id).clockClear, bitOf(id));
}

bool switch_car_clock_enabled(unsigned id)
{
    return (switch_mmio_read32(car(), bankOf(id).clockOut) & bitOf(id)) != 0;
}

void switch_car_reset_assert(unsigned id)
{
    switch_mmio_write32(car(), bankOf(id).resetSet, bitOf(id));
}

void switch_car_reset_deassert(unsigned id)
{
    switch_mmio_write32(car(), bankOf(id).resetClear, bitOf(id));
}

bool switch_car_reset_asserted(unsigned id)
{
    return (switch_mmio_read32(car(), bankOf(id).reset) & bitOf(id)) != 0;
}

uint32_t switch_car_read(uint32_t offset)
{
    return switch_mmio_read32(car(), offset);
}

void switch_car_write(uint32_t offset, uint32_t value)
{
    switch_mmio_write32(car(), offset, value);
}

void switch_car_rmw(uint32_t offset, uint32_t clear, uint32_t set)
{
    IOLockLock(lock());
    switch_mmio_rmw32(car(), offset, clear, set);
    IOLockUnlock(lock());
}

bool switch_car_wait(uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_us)
{
    return switch_mmio_wait32(car(), offset, mask, value, timeout_us);
}

bool switch_pmc_partition_is_on(unsigned partition)
{
    return ((switch_mmio_read32(pmc(), PWRGATE_STATUS) >> partition) & 1) != 0;
}

bool switch_pmc_partition_on(unsigned partition)
{
    return toggle(partition, true);
}

bool switch_pmc_partition_off(unsigned partition)
{
    return toggle(partition, false);
}

void switch_pmc_remove_clamping(unsigned partition)
{
    switch_mmio_write32(pmc(), REMOVE_CLAMPING, 1u << partition);
}

bool switch_pmc_power_up_in_reset(unsigned partition, unsigned clock_id, unsigned reset_id)
{
    switch_car_reset_assert(reset_id);
    if (!switch_pmc_partition_on(partition))
        return false;
    IODelay(kSequenceStepUs);
    switch_car_clock_enable(clock_id);
    IODelay(kSequenceStepUs);
    switch_pmc_remove_clamping(partition);
    IODelay(kSequenceStepUs);
    return true;
}

bool switch_pmc_power_up(unsigned partition, unsigned clock_id, unsigned reset_id)
{
    if (!switch_pmc_power_up_in_reset(partition, clock_id, reset_id))
        return false;
    switch_car_reset_deassert(reset_id);
    IODelay(kSequenceStepUs);
    return true;
}

bool switch_pmc_power_down(unsigned partition, unsigned clock_id, unsigned reset_id)
{
    switch_car_clock_enable(clock_id);
    IODelay(kSequenceStepUs);
    switch_car_reset_assert(reset_id);
    IODelay(kSequenceStepUs);
    switch_car_clock_disable(clock_id);
    IODelay(kSequenceStepUs);
    return switch_pmc_partition_off(partition);
}

bool switch_pmc_io_pad_on(unsigned dpd)
{
    return pad(dpd, kDpdCodeOff);
}

bool switch_pmc_io_pad_off(unsigned dpd)
{
    return pad(dpd, kDpdCodeOn);
}

uint32_t switch_pmc_read(uint32_t offset)
{
    return switch_mmio_read32(pmc(), offset);
}

void switch_pmc_write(uint32_t offset, uint32_t value)
{
    switch_mmio_write32(pmc(), offset, value);
}

void switch_pmc_rmw(uint32_t offset, uint32_t clear, uint32_t set)
{
    IOLockLock(lock());
    switch_mmio_rmw32(pmc(), offset, clear, set);
    IOLockUnlock(lock());
}
