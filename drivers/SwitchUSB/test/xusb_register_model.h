// A register model of what the XUSB bring-up touches (CAR, PMC, fuses, APB_MISC, MC, pad controller, FPCI, IPFS, xHCI),
// for host tests that assert the bring-up's writes in order: every write, delay and sleep lands in one log. Registers
// read back what was written; the PMC toggles a partition and removes its clamps at once, and a PLL (PLLU, UTMIPLL,
// PLLRE) locks as soon as it is enabled.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

struct XusbEvent {
    enum Kind : uint8_t {
        Write,
        DelayUs,
        SleepMs
    } kind;

    uintptr_t address;  // Write
    uint32_t value;     // the value written, or the delay
    bool operator==(const XusbEvent &other) const = default;
};

struct XusbModel {
    std::map<uintptr_t, uint32_t> registers;
    std::map<uintptr_t, unsigned> reads;  // per address: a status bit read in a loop shows here
    bool realLaneUp = false;
    bool pllsLock = true;                 // false: PLLU, UTMIPLL and PLLRE never report lock
    std::vector<XusbEvent> log;
    // Runs after the model's own reaction to a write (a test's extra hardware behaviour).
    std::function<void(uintptr_t address, uint32_t value)> afterWrite;
};

XusbModel &xusbModel();
void xusbModelReset();

// The log's writes to `address`, in order.
std::vector<uint32_t> xusbWritesTo(uintptr_t address);
// The log position of the first write to `address` (whose value has all of `bits` set when bits != 0), or -1.
long xusbFirstWrite(uintptr_t address, uint32_t bits = 0);

inline XusbEvent xusbWrite(uintptr_t address, uint32_t value)
{
    return {XusbEvent::Write, address, value};
}

inline XusbEvent xusbDelay(uint32_t us)
{
    return {XusbEvent::DelayUs, 0, us};
}

inline XusbEvent xusbSleep(uint32_t ms)
{
    return {XusbEvent::SleepMs, 0, ms};
}
