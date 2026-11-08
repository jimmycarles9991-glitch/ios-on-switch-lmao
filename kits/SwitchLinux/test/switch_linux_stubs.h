// Host stand-ins for what kits/SwitchLinux/SwitchLinux.cpp takes from XNU: each records its call in a log the test
// reads ("IOLog <line>", "delay 5 0 1000", ...), and the interrupt mask and preemption level are settable.
#ifndef SWITCH_TEST_SWITCH_LINUX_STUBS_H
#define SWITCH_TEST_SWITCH_LINUX_STUBS_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int boolean_t;
#define TRUE  1
#define FALSE 0
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef void *event_t;
typedef int wait_result_t;
typedef int wait_interrupt_t;
typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
typedef struct thread_call *thread_call_t;
typedef int thread_call_priority_t;
typedef uint32_t thread_call_options_t;
typedef struct lck_grp lck_grp_t;
typedef struct lck_mtx lck_mtx_t;
typedef struct lck_rw lck_rw_t;
typedef int lck_sleep_action_t;

#define NSEC_PER_USEC             1000u
#define NSEC_PER_MSEC             1000000u
#define THREAD_UNINT              0
#define THREAD_AWAKENED           0
#define THREAD_TIMED_OUT          1
#define THREAD_CONTINUE_NULL      ((void *)0)
#define LCK_GRP_ATTR_NULL         ((void *)0)
#define LCK_ATTR_NULL             ((void *)0)
#define LCK_SLEEP_DEFAULT         0
#define OS_LOG_DEFAULT            0
#define THREAD_CALL_PRIORITY_HIGH 0
#define THREAD_CALL_OPTIONS_ONCE  1u

void switch_linux_record(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern boolean_t switch_linux_stub_interrupts;
extern int switch_linux_stub_preemption;
extern uint64_t switch_linux_stub_now;
extern wait_result_t switch_linux_stub_wait_result;

void IOLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define os_log_error(log, fmt, ...) switch_linux_record("os_log_error " fmt, __VA_ARGS__)
void *IOMalloc(vm_size_t size);
void *IOMallocZero(vm_size_t size);
void IOFree(void *address, vm_size_t size);
uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns);
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline);
void delay_for_interval(uint32_t interval, uint32_t scale);
void delay_for_interval_with_leeway(uint32_t interval, uint32_t leeway, uint32_t scale);
wait_result_t assert_wait(event_t event, wait_interrupt_t interruptible);
wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t interruptible, uint64_t deadline);
wait_result_t thread_block(void *continuation);
void thread_wakeup_prim(event_t event, boolean_t one, wait_result_t result);
#define thread_wakeup(event)     thread_wakeup_prim((event), FALSE, THREAD_AWAKENED)
#define thread_wakeup_one(event) thread_wakeup_prim((event), TRUE, THREAD_AWAKENED)
lck_grp_t *lck_grp_alloc_init(const char *name, void *attr);
void lck_grp_free(lck_grp_t *group);
lck_mtx_t *lck_mtx_alloc_init(lck_grp_t *group, void *attr);
void lck_mtx_free(lck_mtx_t *mutex, lck_grp_t *group);
void lck_mtx_lock(lck_mtx_t *mutex);
void lck_mtx_unlock(lck_mtx_t *mutex);
boolean_t lck_mtx_try_lock(lck_mtx_t *mutex);
wait_result_t lck_mtx_sleep(lck_mtx_t *mutex, lck_sleep_action_t action, event_t event, wait_interrupt_t interruptible);
wait_result_t lck_mtx_sleep_deadline(lck_mtx_t *mutex, lck_sleep_action_t action, event_t event,
                                     wait_interrupt_t interruptible, uint64_t deadline);
lck_rw_t *lck_rw_alloc_init(lck_grp_t *group, void *attr);
void lck_rw_free(lck_rw_t *rw, lck_grp_t *group);
void lck_rw_lock_shared(lck_rw_t *rw);
void lck_rw_unlock_shared(lck_rw_t *rw);
void lck_rw_lock_exclusive(lck_rw_t *rw);
void lck_rw_unlock_exclusive(lck_rw_t *rw);
thread_call_t thread_call_allocate_with_options(thread_call_func_t func, thread_call_param_t param0,
                                                thread_call_priority_t pri, thread_call_options_t options);
boolean_t thread_call_free(thread_call_t call);
boolean_t thread_call_enter(thread_call_t call);
boolean_t thread_call_enter_delayed(thread_call_t call, uint64_t deadline);
boolean_t thread_call_cancel(thread_call_t call);
boolean_t thread_call_cancel_wait(thread_call_t call);

#ifdef __cplusplus
}
#endif

#endif
