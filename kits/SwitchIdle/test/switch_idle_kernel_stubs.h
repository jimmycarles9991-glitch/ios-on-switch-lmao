// SPDX-License-Identifier: BSD-2-Clause-Patent
// What kernel/switch/idle/idle.c sees of XNU, for the host: a clock and a WFI the test drives, the thread call it
// runs, the os_log lines it reads. The idle path runs with interrupts masked (idle_test_masked set around it), where
// XNU panics on a mutex, a sleep or an allocation and logging belongs to the thread call: each stand-in for one records
// a violation while masked. Anything else the idle path would call has no declaration here and does not compile.
#ifndef SWITCH_IDLE_KERNEL_STUBS_H
#define SWITCH_IDLE_KERNEL_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __improbable
#define __improbable(x) __builtin_expect(!!(x), 0)
#endif
#define FALSE                 0
#define SWITCH_SMP_CORE_COUNT 4

struct cpu_data {
    int cpu_number;
    uint64_t rtcPop;
};
typedef struct cpu_data cpu_data_t;

// Each read-modify-write the idle code issues is counted (defined by the test).
void idle_test_count_rmw(void);

#define idle_test_order_relaxed  __ATOMIC_RELAXED
#define idle_test_order_acquire  __ATOMIC_ACQUIRE
#define idle_test_order_release  __ATOMIC_RELEASE
#define idle_test_order_acq_rel  __ATOMIC_ACQ_REL
#define os_atomic_load(p, m)     __atomic_load_n((p), idle_test_order_##m)
#define os_atomic_store(p, v, m) __atomic_store_n((p), (v), idle_test_order_##m)
#define os_atomic_xchg(p, v, m)  (idle_test_count_rmw(), __atomic_exchange_n((p), (v), idle_test_order_##m))
#define os_atomic_cmpxchg(p, e, v, m)                                                                             \
    ({                                                                                                            \
        __typeof__(*(p)) idle_test_expected = (e);                                                                \
        idle_test_count_rmw();                                                                                    \
        __atomic_compare_exchange_n((p), &idle_test_expected, (v), false, idle_test_order_##m, __ATOMIC_RELAXED); \
    })

typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
typedef struct idle_test_thread_call *thread_call_t;
#define THREAD_CALL_PRIORITY_KERNEL 2
#define THREAD_CALL_OPTIONS_ONCE    1u

#define OS_LOG_DEFAULT ((void *)0)

extern int idle_test_masked;
extern unsigned idle_test_violations;
extern const char *idle_test_violation;
extern uint64_t idle_test_now;
extern uint64_t idle_test_wfi_until;
extern unsigned idle_test_thread_call_enters;

uint64_t mach_absolute_time(void);
cpu_data_t *getCpuDatap(void);
void ClearIdlePop(int);
void switch_secondary_vectors_leave(void);
void idle_test_wfi(void);
#define __builtin_arm_wfi() idle_test_wfi()

thread_call_t thread_call_allocate_with_options(thread_call_func_t func, thread_call_param_t param, int priority,
                                                unsigned options);
// XNU's thread_call_enter takes the call group's spin lock with interrupts disabled: callable from interrupt context.
bool thread_call_enter(thread_call_t call);

void os_log(void *log, const char *format, ...) __printflike(2, 3);
void os_log_error(void *log, const char *format, ...) __printflike(2, 3);
void lck_mtx_lock(void *lock);
void *kalloc_data(size_t size, int flags);
void *IOMalloc(size_t size);
void IOSleep(unsigned milliseconds);
void delay(int microseconds);
int thread_block(void *continuation);

#ifdef __cplusplus
}
#endif

#endif
