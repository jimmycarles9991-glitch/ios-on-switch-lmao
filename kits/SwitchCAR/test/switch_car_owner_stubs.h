// Host stand-ins for what kits/SwitchCAR takes from the kernel in its owner tests: the kit's IOLock as a second thread
// sees it (a step that finds it held runs once it is released), IODelay and the clock on the model's time, panic as an
// exception, and ml_io_map handing back the physical address (the register model is keyed by it).
#ifndef SWITCH_TEST_SWITCH_CAR_OWNER_STUBS_H
#define SWITCH_TEST_SWITCH_CAR_OWNER_STUBS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
#include <stdexcept>

struct IOLock {
    bool held;
};

IOLock *IOLockAlloc();
void IOLockFree(IOLock *lock);
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);
void IODelay(unsigned us);

struct SwitchCarPanic : std::runtime_error {
    using std::runtime_error::runtime_error;
};

__attribute__((format(printf, 1, 2), noreturn)) void panic(const char *format, ...);

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
