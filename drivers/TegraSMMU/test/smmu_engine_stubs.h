// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_TEST_SMMU_ENGINE_STUBS_H
#define SWITCH_TEST_SMMU_ENGINE_STUBS_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef uintptr_t vm_offset_t;
typedef uint32_t IOOptionBits;
class IOBufferMemoryDescriptor;

#define NSEC_PER_USEC 1000u

struct IOSimpleLock {
    bool held;
};

struct IOLock {
    bool held;
};

inline IOSimpleLock *IOSimpleLockAlloc()
{
    return new IOSimpleLock{};
}

inline void IOSimpleLockFree(IOSimpleLock *lock)
{
    delete lock;
}

inline void IOSimpleLockLock(IOSimpleLock *lock)
{
    lock->held = true;
}

inline void IOSimpleLockUnlock(IOSimpleLock *lock)
{
    lock->held = false;
}

inline IOLock *IOLockAlloc()
{
    return new IOLock{};
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
}

inline void *IOMalloc(size_t size)
{
    return malloc(size);
}

inline void IOFree(void *address, size_t)
{
    free(address);
}

inline void IOSleep(unsigned)
{}

inline void IODelay(unsigned)
{}

inline void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

inline void IOLog(const char *format, ...)
{
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
}

inline void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *result)
{
    *result = 0;
}

inline uint64_t mach_absolute_time()
{
    return 0;
}

#define IONew(type, count) static_cast<type *>(calloc(count, sizeof(type)))
#define IODelete(ptr, type, count) \
    do {                           \
        (void)(count);             \
        free(ptr);                 \
    } while (0)

#ifndef SWITCH_DCACHE_OP
#define SWITCH_DCACHE_OP(op, address) \
    do {                              \
        (void)(op);                   \
        (void)(address);              \
    } while (0)
#endif
#ifndef SWITCH_DCACHE_BARRIER
#define SWITCH_DCACHE_BARRIER() \
    do {                        \
    } while (0)
#endif

#endif
