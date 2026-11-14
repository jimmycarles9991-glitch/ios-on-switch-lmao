// The kernel as SwitchFalconInterrupt.cpp sees it in switch_falcon_interrupt_test: IOLog, IOLock and panic beside the
// shared event-source stand-ins; IOLog or a blocking lock throws while interrupts are off.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include "kits/SwitchFalcon/test/switch_falcon_interrupt_stubs.h"

inline std::vector<std::string> switch_test_lines;

inline void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void IOLog(const char *format, ...)
{
    switch_test_interrupts_off_forbids("IOLog");
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    switch_test_lines.emplace_back(line);
}

[[noreturn]] inline void panic(const char *format, ...)
{
    char line[1024];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    throw std::runtime_error(std::string("panic: ") + line);
}

struct IOLock {
    bool held;
};

inline IOLock *IOLockAlloc()
{
    switch_test_interrupts_off_forbids("IOLockAlloc");
    return new IOLock{};
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    switch_test_interrupts_off_forbids("IOLockLock");
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
}

#define NSEC_PER_USEC 1000u

inline uint64_t mach_absolute_time()
{
    return 0;
}

inline void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *deadline)
{
    *deadline = 0;
}

extern "C" uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t size);
