// Host stand-ins for what kits/SwitchCAR/SwitchCAR.cpp takes from the kernel: IOLock as a mutex, IODelay, panic as
// an exception, the clock, and ml_io_map handing back the physical address (the register model is keyed by it).
#ifndef SWITCH_TEST_SWITCH_CAR_STUBS_H
#define SWITCH_TEST_SWITCH_CAR_STUBS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
#include <mutex>
#include <stdexcept>

struct IOLock {
    std::mutex mutex;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

// A real mutex: the test drives the kit from several threads. Its lock/unlock pairs span the kit's calls, past
// what -Wthread-safety can follow.
__attribute__((no_thread_safety_analysis)) inline void IOLockLock(IOLock *lock)
{
    lock->mutex.lock();
}

__attribute__((no_thread_safety_analysis)) inline void IOLockUnlock(IOLock *lock)
{
    lock->mutex.unlock();
}

inline void IODelay(unsigned)
{}

struct SwitchCarPanic : std::runtime_error {
    using std::runtime_error::runtime_error;
};

__attribute__((format(printf, 1, 2), noreturn)) inline void panic(const char *format, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw SwitchCarPanic(line);
}

extern "C" {
#endif

#define NSEC_PER_USEC 1000u
__attribute__((format(printf, 1, 2))) void IOLog(const char *format, ...);
uint64_t mach_absolute_time(void);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t size);

#ifdef __cplusplus
}
#endif

#endif
