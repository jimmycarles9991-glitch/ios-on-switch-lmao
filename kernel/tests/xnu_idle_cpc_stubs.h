// What kernel/switch/idle/idle.c sees of xnu-12377 (with CONFIG_CPU_COUNTERS) on the host: a clock and a WFI the test
// drives, the thread call and log it reports refusals through, and cpc_cpu_transition, recorded with the WFI and the
// C7 requests in one event list (xnu_idle_cpc_test.cpp).
#ifndef SWITCH_XNU_IDLE_CPC_STUBS_H
#define SWITCH_XNU_IDLE_CPC_STUBS_H

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

#define xnu_idle_order_relaxed   __ATOMIC_RELAXED
#define xnu_idle_order_acquire   __ATOMIC_ACQUIRE
#define xnu_idle_order_release   __ATOMIC_RELEASE
#define os_atomic_load(p, m)     __atomic_load_n((p), xnu_idle_order_##m)
#define os_atomic_store(p, v, m) __atomic_store_n((p), (v), xnu_idle_order_##m)

typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
typedef struct xnu_idle_thread_call *thread_call_t;
#define THREAD_CALL_PRIORITY_KERNEL 2
#define THREAD_CALL_OPTIONS_ONCE    1u

#define OS_LOG_DEFAULT ((void *)0)

typedef enum {
    CPC_CPU_EARLY_INIT,
    CPC_CPU_INIT,
    CPC_CPU_ONLINE,
    CPC_CPU_OFFLINE,
    CPC_CPU_IDLE,
    CPC_CPU_ACTIVE_COLD,
    CPC_CPU_ACTIVE_WARM,
} cpc_cpu_event_t;

void cpc_cpu_transition(cpc_cpu_event_t event, void *cpu_data);

uint64_t mach_absolute_time(void);
cpu_data_t *getCpuDatap(void);
void ClearIdlePop(int);
void switch_secondary_vectors_leave(void);
void xnu_idle_wfi(void);
#define __builtin_arm_wfi() xnu_idle_wfi()

thread_call_t thread_call_allocate_with_options(thread_call_func_t func, thread_call_param_t param, int priority,
                                                unsigned options);
bool thread_call_enter(thread_call_t call);
void os_log(void *log, const char *format, ...) __printflike(2, 3);
void os_log_error(void *log, const char *format, ...) __printflike(2, 3);

#ifdef __cplusplus
}
#endif

#endif
