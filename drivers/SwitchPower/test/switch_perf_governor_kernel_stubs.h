// SPDX-License-Identifier: BSD-2-Clause-Patent
// What drivers/SwitchPower/SwitchPerfGovernor.cpp sees of XNU and of the drivers beside it, for the host: a clock the
// test drives, the perfcontrol registration it captures, and locks, interrupt masking and logging it counts. The
// scheduler's callouts and the touch filter run in callout context (switch_gov_test_callout set around them), where a
// lock, a sleep or an allocation throws. Anything else the governor would call has no declaration here and does not
// compile.
#ifndef SWITCH_PERF_GOVERNOR_KERNEL_STUBS_H
#define SWITCH_PERF_GOVERNOR_KERNEL_STUBS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int boolean_t;
#define FALSE 0
#define TRUE  1

// IOKit/IOLib.h
typedef struct switch_gov_test_lock IOSimpleLock;
typedef struct switch_gov_test_lock IOLock;
typedef int IOInterruptState;
IOSimpleLock *IOSimpleLockAlloc(void);
IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock);
void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock, IOInterruptState state);
IOLock *IOLockAlloc(void);
void IOLockLock(IOLock *lock);
void IOLockUnlock(IOLock *lock);
void IOSleep(unsigned milliseconds);
void *IOMalloc(size_t size);
void IOLog(const char *format, ...) __printflike(1, 2);

// kern/clock.h, kern/cpu_number.h
#define NSEC_PER_USEC 1000ull
uint64_t mach_absolute_time(void);
void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
int cpu_number(void);

// machine/machine_routines.h (osfmk/arm/machine_routines.h, kern/kern_types.h)
typedef enum {
    CONTEXT_SWITCH = 1,
    IDLE = 11,
} perfcontrol_event;
struct perfcontrol_thread_data;
struct perfcontrol_cpu_counters;
typedef void (*sched_perfcontrol_deadline_passed_t)(uint64_t deadline);
typedef void (*sched_perfcontrol_csw_t)(perfcontrol_event event, uint32_t cpu_id, uint64_t timestamp, uint32_t flags,
                                        struct perfcontrol_thread_data *offcore, struct perfcontrol_thread_data *oncore,
                                        struct perfcontrol_cpu_counters *cpu_counters, void *unused);
#define SCHED_PERFCONTROL_CALLBACKS_VERSION_5 5

struct sched_perfcontrol_callbacks {
    unsigned long version;
    sched_perfcontrol_deadline_passed_t deadline_passed;
    sched_perfcontrol_csw_t csw;
};

void sched_perfcontrol_register_callbacks(struct sched_perfcontrol_callbacks *callbacks, unsigned long size_of_state);
boolean_t sched_perfcontrol_update_callback_deadline(uint64_t deadline);
unsigned int ml_get_cpu_count(void);
boolean_t ml_set_interrupts_enabled(boolean_t enable);

// os/log.h
#define OS_LOG_DEFAULT ((void *)0)
void os_log(void *log, const char *format, ...) __printflike(2, 3);

#ifdef __cplusplus
}
#endif

#endif
