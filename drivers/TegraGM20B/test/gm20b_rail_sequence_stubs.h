// Host stand-ins for what TegraSMMU.cpp (the MC hot-reset owner platform.c hands the GPU's flush to) takes from the
// kernel in gm20b_rail_sequence_test: the MC's IOSimpleLock, IOLog, IODelay, the clock, and ml_io_map handing back the
// physical address (the register model is keyed by it).
#ifndef SWITCH_TEST_GM20B_RAIL_SEQUENCE_STUBS_H
#define SWITCH_TEST_GM20B_RAIL_SEQUENCE_STUBS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef uintptr_t vm_offset_t;

#ifdef __cplusplus
struct IOSimpleLock {
	bool held;
};

IOSimpleLock *IOSimpleLockAlloc();
void IOSimpleLockFree(IOSimpleLock *lock);
void IOSimpleLockLock(IOSimpleLock *lock);
void IOSimpleLockUnlock(IOSimpleLock *lock);

inline void IODelay(unsigned)
{
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
