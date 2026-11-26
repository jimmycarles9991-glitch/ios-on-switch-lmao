// The XUSB register model (xusb_register_model.h) behind kits/SwitchMMIO's SWITCH_MMIO_HOST_MODEL hooks.
#include "xusb_register_model.h"
#include "xusb_kernel_stubs.h"

#include <chrono>
#include <cstdio>

namespace {
constexpr uintptr_t kPmc = 0x7000e400, kCar = 0x60006000;
constexpr uint32_t kToggle = 0x30, kRemoveClamping = 0x34, kStatus = 0x38, kStart = 1u << 8;
constexpr uint32_t kPllEnable = 1u << 30, kPllLock = 1u << 27;
constexpr uintptr_t kPlluBase = kCar + 0x0c0, kPllreBase = kCar + 0x4c4, kPllreMisc = kCar + 0x4c8;
constexpr uintptr_t kUtmipPllCfg1 = kCar + 0x484, kUtmipllHwPwrdnCfg0 = kCar + 0x52c;
constexpr uint32_t kForcePllEnablePowerup = 1u << 15, kUtmipllLock = 1u << 31;

XusbModel gModel;

void react(uintptr_t address, uint32_t value)
{
    auto &r = gModel.registers;
    if (address == kPmc + kRemoveClamping)
        r[address] = 0;
    if (address == kPmc + kToggle && (value & kStart)) {
        r[kPmc + kStatus] ^= 1u << (value & 0x1f);
        r[address] = value & ~kStart;
    }
    const bool lock = gModel.pllsLock;
    if (address == kPlluBase)
        r[address] = (value & kPllEnable) && lock ? value | kPllLock : value & ~kPllLock;
    if (address == kPllreBase)
        r[kPllreMisc] = (value & kPllEnable) && lock ? r[kPllreMisc] | kPllLock : r[kPllreMisc] & ~kPllLock;
    if (address == kUtmipPllCfg1 && (value & kForcePllEnablePowerup) && lock)
        r[kUtmipllHwPwrdnCfg0] |= kUtmipllLock;
}
}  // namespace

XusbModel &xusbModel()
{
    return gModel;
}

void xusbModelReset()
{
    gModel.registers.clear();
    gModel.reads.clear();
    gModel.realLaneUp = false;
    gModel.pllsLock = true;
    gModel.log.clear();
    gModel.afterWrite = nullptr;
}

std::vector<uint32_t> xusbWritesTo(uintptr_t address)
{
    std::vector<uint32_t> values;
    for (const XusbEvent &e : gModel.log) {
        if (e.kind == XusbEvent::Write && e.address == address)
            values.push_back(e.value);
    }
    return values;
}

long xusbFirstWrite(uintptr_t address, uint32_t bits)
{
    for (size_t i = 0; i < gModel.log.size(); ++i) {
        const XusbEvent &e = gModel.log[i];
        if (e.kind == XusbEvent::Write && e.address == address && (e.value & bits) == bits)
            return static_cast<long>(i);
    }
    return -1;
}

extern "C" bool switch_usb_ss_up(void)
{
    return gModel.realLaneUp;
}

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    ++gModel.reads[address];
    return gModel.registers[address];
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gModel.log.push_back(xusbWrite(address, value));
    gModel.registers[address] = value;
    react(address, value);
    if (gModel.afterWrite)
        gModel.afterWrite(address, value);
}

extern "C" void switch_test_delay_us(unsigned us)
{
    gModel.log.push_back(xusbDelay(us));
}

extern "C" void switch_test_sleep_ms(unsigned ms)
{
    gModel.log.push_back(xusbSleep(ms));
}

extern "C" void IOLog(const char *format, ...)
{
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
}

extern "C" uint64_t mach_absolute_time(void)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

extern "C" void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = mach_absolute_time() + static_cast<uint64_t>(interval) * scale;
}

extern "C" uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}
