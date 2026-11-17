// Host stand-ins for what TegraSMMU.cpp takes from the kernel in smmu_flush_range_test: the MC's IOSimpleLock,
// ml_io_map handing back the physical address, and every call that can lock, sleep, wait or allocate. One made while a
// simple lock is held is noted in switch_test_under_spinlock, which the test requires to stay empty.
#ifndef SWITCH_TEST_SMMU_FLUSH_STUBS_H
#define SWITCH_TEST_SMMU_FLUSH_STUBS_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

typedef uintptr_t vm_offset_t;
#define NSEC_PER_USEC 1000u

inline unsigned switch_test_spinlocks_held;
inline std::vector<std::string> switch_test_under_spinlock;

inline void switch_test_may_block(const char *what)
{
    if (switch_test_spinlocks_held)
        switch_test_under_spinlock.emplace_back(what);
}

struct IOSimpleLock {
    bool held;
};

struct IOLock {
    bool held;
};

inline IOSimpleLock *IOSimpleLockAlloc()
{
    switch_test_may_block("IOSimpleLockAlloc");
    return new IOSimpleLock{};
}

inline void IOSimpleLockFree(IOSimpleLock *lock)
{
    switch_test_may_block("IOSimpleLockFree");
    delete lock;
}

inline void IOSimpleLockLock(IOSimpleLock *lock)
{
    switch_test_may_block("IOSimpleLockLock");
    lock->held = true;
    ++switch_test_spinlocks_held;
}

inline void IOSimpleLockUnlock(IOSimpleLock *lock)
{
    lock->held = false;
    --switch_test_spinlocks_held;
}

inline IOLock *IOLockAlloc()
{
    switch_test_may_block("IOLockAlloc");
    return new IOLock{};
}

inline void IOLockFree(IOLock *lock)
{
    switch_test_may_block("IOLockFree");
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    switch_test_may_block("IOLockLock");
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    switch_test_may_block("IOLockUnlock");
    lock->held = false;
}

inline void *IOMalloc(size_t size)
{
    switch_test_may_block("IOMalloc");
    return malloc(size);
}

inline void IOFree(void *address, size_t)
{
    switch_test_may_block("IOFree");
    free(address);
}

inline void IOSleep(unsigned)
{
    switch_test_may_block("IOSleep");
}

inline void IODelay(unsigned)
{
    switch_test_may_block("IODelay");
}

inline void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void IOLog(const char *format, ...)
{
    switch_test_may_block("IOLog");
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
}

// switch_mmio_wait32's deadline: a CPU wait.
inline void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *result)
{
    switch_test_may_block("clock_interval_to_deadline");
    *result = 0;
}

inline uint64_t mach_absolute_time()
{
    return 0;
}

#endif
