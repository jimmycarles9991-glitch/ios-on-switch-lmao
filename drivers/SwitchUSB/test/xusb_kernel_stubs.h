// Host stand-ins for what the XUSB bring-up (XusbUtmiPad.cpp, XusbHostPower.cpp, kits/SwitchCAR) takes from the
// kernel: delays and sleeps reported to the test's register model (it logs them between the writes), IOLock as a
// mutex, panic as an exception, os_log as IOLog, the clock, and ml_io_map handing back the physical address.
#ifndef SWITCH_TEST_XUSB_KERNEL_STUBS_H
#define SWITCH_TEST_XUSB_KERNEL_STUBS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
#include <mutex>
#include <stdexcept>

extern "C" void switch_test_delay_us(unsigned us);
extern "C" void switch_test_sleep_ms(unsigned ms);

struct IOLock {
    std::mutex mutex;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(const IOLock *lock)
{
    delete lock;
}

__attribute__((no_thread_safety_analysis)) inline void IOLockLock(IOLock *lock)
{
    lock->mutex.lock();
}

__attribute__((no_thread_safety_analysis)) inline void IOLockUnlock(IOLock *lock)
{
    lock->mutex.unlock();
}

inline void IODelay(unsigned us)
{
    switch_test_delay_us(us);
}

inline void IOSleep(unsigned ms)
{
    switch_test_sleep_ms(ms);
}

struct SwitchXusbPanic : std::runtime_error {
    using std::runtime_error::runtime_error;
};

__attribute__((format(printf, 1, 2), noreturn)) inline void panic(const char *format, ...)
{
    char line[256];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw SwitchXusbPanic(line);
}

extern "C" {
#endif

#define NSEC_PER_USEC 1000u
__attribute__((format(printf, 1, 2))) void IOLog(const char *format, ...);
#define OS_LOG_DEFAULT   0
#define os_log(log, ...) IOLog(__VA_ARGS__)
uint64_t mach_absolute_time(void);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline);
uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t size);

#ifdef __cplusplus
}
#endif

#endif
