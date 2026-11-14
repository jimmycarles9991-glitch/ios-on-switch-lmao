// switch_car_owner_model.h's register file, clock, lock and deep power-down machine.
#include "switch_car_owner_model.h"
#include "switch_car_owner_stubs.h"

#include <gtest/gtest.h>

#include <map>

namespace switch_car_owner_model {
namespace {
struct DpdOperation {
    uint64_t doneNs;
    uintptr_t status;
    uint32_t pads, code;
};

std::map<uintptr_t, uint32_t> gRegisters;
std::vector<std::pair<uintptr_t, uint32_t>> gWrites;
uint64_t gNowNs;
int gLocksHeld;
std::vector<std::function<void()>> gBlocked;
uintptr_t gSecondAt;
std::function<void()> gSecond;
bool gSecondRan;
uint32_t gApbKHz = 12800;
uint64_t gDpdBusyUntilNs;
std::vector<DpdOperation> gDpd;
unsigned gDropped;

uint64_t apbClocksNs(uint32_t clocks)
{
    return static_cast<uint64_t>(clocks) * 1000000u / gApbKHz;
}

void tick()
{
    gNowNs += 100;
    for (auto it = gDpd.begin(); it != gDpd.end();) {
        if (it->doneNs > gNowNs) {
            ++it;
            continue;
        }
        uint32_t &status = gRegisters[it->status];
        status = it->code == 2 ? status | it->pads : status & ~it->pads;
        it = gDpd.erase(it);
    }
}

void dpdRequest(uintptr_t status, uint32_t value)
{
    if (gNowNs < gDpdBusyUntilNs) {
        ++gDropped;
        return;
    }
    const uint32_t timer = gRegisters[kPmc + kSelDpdTim] & 0x7f;
    gDpd.push_back({gNowNs + apbClocksNs(timer), status, value & 0x3fffffffu, value >> 30});
    gDpdBusyUntilNs = gNowNs + apbClocksNs(timer + 5);
}
}  // namespace

void reset()
{
    gRegisters.clear();
    gWrites.clear();
    gNowNs = 0;
    gBlocked.clear();
    gSecond = nullptr;
    gSecondRan = false;
    gApbKHz = 12800;
    gDpdBusyUntilNs = 0;
    gDpd.clear();
    gDropped = 0;
    gRegisters[kPmc + kSelDpdTim] = 0x7f;
}

uint32_t &reg(uintptr_t address)
{
    return gRegisters[address];
}

std::vector<std::pair<uintptr_t, uint32_t>> writes()
{
    return gWrites;
}

void landsAtTheNextRead(uintptr_t address, std::function<void()> step)
{
    gSecondAt = address;
    gSecond = std::move(step);
    gSecondRan = false;
}

bool secondRan()
{
    return gSecondRan;
}

void apbKHz(uint32_t khz)
{
    gApbKHz = khz;
}

unsigned droppedDpdRequests()
{
    return gDropped;
}
}  // namespace switch_car_owner_model

using namespace switch_car_owner_model;

IOLock *IOLockAlloc()
{
    return new IOLock{};
}

void IOLockFree(IOLock *lock)
{
    delete lock;
}

void IOLockLock(IOLock *lock)
{
    EXPECT_FALSE(lock->held) << "a step that found the kit's lock held ran before its release";
    lock->held = true;
    ++gLocksHeld;
}

void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
    --gLocksHeld;
    std::vector<std::function<void()>> blocked;
    blocked.swap(gBlocked);
    for (const auto &step : blocked)
        step();
}

void IODelay(unsigned us)
{
    gNowNs += static_cast<uint64_t>(us) * 1000u;
    tick();
}

void panic(const char *format, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw SwitchCarPanic(line);
}

extern "C" {
uint32_t switch_mmio_model_read32(uintptr_t address)
{
    tick();
    const uint32_t value = gRegisters[address];
    if (address == gSecondAt && gSecond) {
        std::function<void()> second = std::move(gSecond);
        gSecond = nullptr;
        std::function<void()> run = [second] {
            second();
            gSecondRan = true;
        };
        if (gLocksHeld)
            gBlocked.push_back(run);
        else
            run();
    }
    return value;
}

void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    tick();
    gRegisters[address] = value;
    gWrites.emplace_back(address, value);
    if (address == kPmc + kDpdReq)
        dpdRequest(kPmc + kDpdStatus, value);
    else if (address == kPmc + kDpd2Req)
        dpdRequest(kPmc + kDpd2Status, value);
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

uint64_t mach_absolute_time(void)
{
    return gNowNs;
}

void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result)
{
    *result = gNowNs + static_cast<uint64_t>(interval) * scale_factor;
}

uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}
}
