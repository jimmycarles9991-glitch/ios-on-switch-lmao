// What SwitchGpuInheritorSleep.h's kernel branch sees of XNU, for gm20b_inheritor_sleep_test.cpp: the lock, thread
// and wait types, and the two sleep and two wake calls, which the test records.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lck_mtx lck_mtx_t;
typedef struct IOLock IOLock;
typedef struct thread *thread_t;
typedef void *event_t;
typedef unsigned int lck_sleep_action_t;
typedef int wait_interrupt_t;
typedef int wait_result_t;
typedef int kern_return_t;
typedef uint32_t UInt32;

#define LCK_SLEEP_DEFAULT 0u
#define THREAD_UNINT 0
#define THREAD_AWAKENED 0
#define TIMEOUT_WAIT_FOREVER ((uint64_t)0)

lck_mtx_t *IOLockGetMachLock(IOLock *lock);
int IOLockSleep(IOLock *lock, void *event, UInt32 interType);
void IOLockWakeup(IOLock *lock, void *event, bool oneThread);
thread_t current_thread(void);

#ifdef __cplusplus
}
#endif
