// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XNU services the Linux-compat shims (TegraBPMP, TegraDFLL, TegraGM20B) build Linux's primitives on, each with the
// semantics of the Linux call it backs. Plain C with fixed-width types only, so the Linux-flavoured units that include
// it never see XNU's names. Spin and reader-writer locks are inline: SwitchLinuxLock.h.
#ifndef SWITCH_LINUX_H
#define SWITCH_LINUX_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One kernel log line, from any context: with interrupts or preemption off it goes to os_log's message buffer path
// (IOLog may block there), formatted with a plain %s since that path has no privacy specifiers.
void switch_linux_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void switch_linux_vlog(const char *fmt, va_list ap);
void switch_linux_log_line(const char *line);

// Kernel heap whose blocks know their size, as kfree and ksize need. NULL for a request over 1 GiB.
void *switch_linux_malloc(uint64_t size, int zero);
void switch_linux_free(const void *block);
uint64_t switch_linux_malloc_size(const void *block);

// This CPU's interrupt mask (local_irq_save / local_irq_restore). Restoring "enabled" takes a pending preemption.
unsigned long switch_linux_irq_save(void);
void switch_linux_irq_restore(unsigned long flags);
bool switch_linux_irqs_disabled(void);
int switch_linux_cpu(void);

uint64_t switch_linux_nanotime(void);
uint64_t switch_linux_abstime_to_ns(uint64_t abstime);

// udelay and usleep_range are XNU's delay: it spins where the caller may not block (interrupts or preemption off) or
// where the wait is shorter than a wake-up costs, and blocks otherwise, so neither ever sleeps where Linux's would not.
// usleep_range's slack becomes the timer's leeway.
void switch_linux_udelay(uint64_t us);
void switch_linux_usleep_range(uint64_t min_us, uint64_t max_us);
void switch_linux_cond_resched(void);

// Event sleeps, Mach's assert-then-block: switch_linux_wait_prepare under the lock that guards the condition (deadline
// ~0: none), drop the lock, then switch_linux_wait_block, which returns true when woken, false at the deadline. A wakeup in
// between is not lost. Thread context.
void switch_linux_wait_prepare(const void *event, uint64_t deadline_ns);
bool switch_linux_wait_block(void);
void switch_linux_wakeup(const void *event);
void switch_linux_wakeup_one(const void *event);

// mutex and rw_semaphore: sleeping locks, thread context only. A slot that is still NULL (DEFINE_MUTEX, a zeroed
// object) gets its lock on first use; switch_linux_*_free takes a slot's lock back.
void *switch_linux_mutex_alloc(void);
void switch_linux_mutex_free(void *mutex);
void *switch_linux_mutex_of(void **slot);
void switch_linux_mutex_lock(void *mutex);
void switch_linux_mutex_unlock(void *mutex);
bool switch_linux_mutex_trylock(void *mutex);
// With `mutex` held: drops it, sleeps until switch_linux_wakeup(event) or the deadline (~0: none), takes it again.
// True if woken.
bool switch_linux_mutex_sleep(void *mutex, const void *event, uint64_t deadline_ns);
void *switch_linux_rwsem_alloc(void);
void switch_linux_rwsem_free(void *rwsem);
void *switch_linux_rwsem_of(void **slot);
void switch_linux_down_read(void *rwsem);
void switch_linux_up_read(void *rwsem);
void switch_linux_down_write(void *rwsem);
void switch_linux_up_write(void *rwsem);

// One-shot calls on a kernel thread (work_struct, timer_list, hrtimer). A call never runs twice at once, as a Linux work
// item or timer never does: entered while it runs, it runs again once that run returns (an XNU thread call would start
// a second run on another worker). Entering a pending call moves it (mod_timer, hrtimer_start); enter and cancel are safe
// with interrupts off and return true if the call was pending. cancel_wait cancels a pending run and waits for one under
// way (cancel_work_sync, del_timer_sync, hrtimer_cancel); flush waits the same way but makes a pending run itself,
// fn(arg) on the caller's thread (flush_work). Both are thread context, never from the call; both return true if a run
// was pending. alloc is thread context; free takes a call that is neither pending nor running.
void *switch_linux_call_alloc(void (*fn)(void *arg), void *arg);
void switch_linux_call_free(void *call);
bool switch_linux_call_enter(void *call);
bool switch_linux_call_enter_after(void *call, uint64_t delay_ns);
bool switch_linux_call_cancel(void *call);
bool switch_linux_call_cancel_wait(void *call);
bool switch_linux_call_flush(void *call, void (*fn)(void *arg), void *arg);

// Device memory, uncached (or write-combined), mapped for the kernel's life (ml_io_map). NULL on failure.
volatile void *switch_linux_ioremap(uint64_t pa, uint64_t size, int write_combined);

#ifdef __cplusplus
}
#endif

#endif
