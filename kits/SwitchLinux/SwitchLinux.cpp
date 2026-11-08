// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchLinux.h"
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread_call.h>
#include "../SwitchTime/SwitchTime.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
// NOLINTBEGIN(readability-redundant-declaration): not every kernel configuration's headers declare them
boolean_t ml_set_interrupts_enabled(boolean_t enable);
boolean_t ml_get_interrupts_enabled(void);
int get_preemption_level(void);
int cpu_number(void);
void thread_yield_to_preemption(void);
int vsnprintf(char *, size_t, const char *, va_list);
// NOLINTEND(readability-redundant-declaration)
}

namespace {
constexpr uint64_t kMallocLimit = 1ULL << 30;
constexpr uint64_t kNoDeadline = ~0ULL;

// The size sits in front of the block, 16 bytes so the block keeps IOMalloc's alignment.
constexpr size_t kHeader = 16;

lck_grp_t *gGroup;

lck_grp_t *group()
{
    lck_grp_t *have = __atomic_load_n(&gGroup, __ATOMIC_ACQUIRE);
    if (have)
        return have;
    lck_grp_t *fresh = lck_grp_alloc_init("SwitchLinux", LCK_GRP_ATTR_NULL);
    if (__atomic_compare_exchange_n(&gGroup, &have, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    lck_grp_free(fresh);
    return have;
}

// NOLINTNEXTLINE(misc-const-correctness): the compare-exchange writes *slot
void *lazy(void **slot, void *(*alloc)(), void (*release)(void *))
{
    void *have = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (have)
        return have;
    void *fresh = alloc();
    if (__atomic_compare_exchange_n(slot, &have, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    release(fresh);
    return have;
}

event_t eventOf(const void *event)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): an event is only an address to XNU
    return reinterpret_cast<event_t>(const_cast<void *>(event));
}

thread_call_t callOf(void *call)
{
    return static_cast<thread_call_t>(call);
}
}  // namespace

extern "C" void switch_linux_log_line(const char *line)
{
    if (ml_get_interrupts_enabled() && get_preemption_level() == 0) {
        IOLog("%s", line);
    } else {
        os_log_error(OS_LOG_DEFAULT, "%s", line);
    }
}

extern "C" void switch_linux_vlog(const char *fmt, va_list ap)
{
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    switch_linux_log_line(line);
}

extern "C" void switch_linux_log(const char *fmt, ...)
{
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    switch_linux_vlog(fmt, ap);
    va_end(ap);
}

extern "C" void *switch_linux_malloc(uint64_t size, int zero)
{
    if (size > kMallocLimit)
        return nullptr;
    const vm_size_t bytes = static_cast<vm_size_t>(size) + kHeader;
    auto *block = static_cast<uint64_t *>(zero ? IOMallocZero(bytes) : IOMalloc(bytes));
    if (!block)
        return nullptr;
    block[0] = size;
    return reinterpret_cast<uint8_t *>(block) + kHeader;
}

extern "C" uint64_t switch_linux_malloc_size(const void *block)
{
    return block ? *reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(block) - kHeader) : 0;
}

extern "C" void switch_linux_free(const void *block)
{
    if (!block)
        return;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): kfree takes a const pointer
    auto *start = const_cast<uint8_t *>(static_cast<const uint8_t *>(block) - kHeader);
    IOFree(start, static_cast<vm_size_t>(*reinterpret_cast<uint64_t *>(start)) + kHeader);
}

extern "C" unsigned long switch_linux_irq_save(void)
{
    return ml_set_interrupts_enabled(FALSE) ? 1UL : 0UL;
}

extern "C" void switch_linux_irq_restore(unsigned long flags)
{
    ml_set_interrupts_enabled(flags ? TRUE : FALSE);
}

extern "C" bool switch_linux_irqs_disabled(void)
{
    return ml_get_interrupts_enabled() == FALSE;
}

extern "C" int switch_linux_cpu(void)
{
    return cpu_number();
}

extern "C" uint64_t switch_linux_nanotime(void)
{
    return switch_linux_abstime_to_ns(mach_absolute_time());
}

extern "C" uint64_t switch_linux_abstime_to_ns(uint64_t abstime)
{
    return switch_abs_to_ns(abstime);
}

extern "C" void switch_linux_udelay(uint64_t us)
{
    delay_for_interval(static_cast<uint32_t>(us), NSEC_PER_USEC);
}

extern "C" void switch_linux_usleep_range(uint64_t min_us, uint64_t max_us)
{
    const uint64_t slack = max_us > min_us ? max_us - min_us : 0;
    delay_for_interval_with_leeway(static_cast<uint32_t>(min_us), static_cast<uint32_t>(slack), NSEC_PER_USEC);
}

extern "C" void switch_linux_cond_resched(void)
{
    thread_yield_to_preemption();
}

extern "C" void switch_linux_wait_prepare(const void *event, uint64_t deadline_ns)
{
    if (deadline_ns == kNoDeadline) {
        assert_wait(eventOf(event), THREAD_UNINT);
    } else {
        assert_wait_deadline(eventOf(event), THREAD_UNINT, switch_abs_from_ns(deadline_ns));
    }
}

extern "C" bool switch_linux_wait_block(void)
{
    return thread_block(THREAD_CONTINUE_NULL) != THREAD_TIMED_OUT;
}

extern "C" void switch_linux_wakeup(const void *event)
{
    thread_wakeup(eventOf(event));
}

extern "C" void switch_linux_wakeup_one(const void *event)
{
    thread_wakeup_one(eventOf(event));
}

extern "C" void *switch_linux_mutex_alloc(void)
{
    return lck_mtx_alloc_init(group(), LCK_ATTR_NULL);
}

extern "C" void switch_linux_mutex_free(void *mutex)
{
    if (mutex)
        lck_mtx_free(static_cast<lck_mtx_t *>(mutex), group());
}

extern "C" void *switch_linux_mutex_of(void **slot)
{
    return lazy(slot, switch_linux_mutex_alloc, switch_linux_mutex_free);
}

extern "C" void switch_linux_mutex_lock(void *mutex)
{
    lck_mtx_lock(static_cast<lck_mtx_t *>(mutex));
}

extern "C" void switch_linux_mutex_unlock(void *mutex)
{
    lck_mtx_unlock(static_cast<lck_mtx_t *>(mutex));
}

extern "C" bool switch_linux_mutex_trylock(void *mutex)
{
    return lck_mtx_try_lock(static_cast<lck_mtx_t *>(mutex)) != FALSE;
}

extern "C" bool switch_linux_mutex_sleep(void *mutex, const void *event, uint64_t deadline_ns)
{
    auto *lock = static_cast<lck_mtx_t *>(mutex);
    const wait_result_t result = deadline_ns == kNoDeadline
                                     ? lck_mtx_sleep(lock, LCK_SLEEP_DEFAULT, eventOf(event), THREAD_UNINT)
                                     : lck_mtx_sleep_deadline(lock, LCK_SLEEP_DEFAULT, eventOf(event), THREAD_UNINT,
                                                              switch_abs_from_ns(deadline_ns));
    return result != THREAD_TIMED_OUT;
}

extern "C" void *switch_linux_rwsem_alloc(void)
{
    return lck_rw_alloc_init(group(), LCK_ATTR_NULL);
}

extern "C" void switch_linux_rwsem_free(void *rwsem)
{
    if (rwsem)
        lck_rw_free(static_cast<lck_rw_t *>(rwsem), group());
}

extern "C" void *switch_linux_rwsem_of(void **slot)
{
    return lazy(slot, switch_linux_rwsem_alloc, switch_linux_rwsem_free);
}

extern "C" void switch_linux_down_read(void *rwsem)
{
    lck_rw_lock_shared(static_cast<lck_rw_t *>(rwsem));
}

extern "C" void switch_linux_up_read(void *rwsem)
{
    lck_rw_unlock_shared(static_cast<lck_rw_t *>(rwsem));
}

extern "C" void switch_linux_down_write(void *rwsem)
{
    lck_rw_lock_exclusive(static_cast<lck_rw_t *>(rwsem));
}

extern "C" void switch_linux_up_write(void *rwsem)
{
    lck_rw_unlock_exclusive(static_cast<lck_rw_t *>(rwsem));
}

extern "C" void *switch_linux_call_alloc(void (*fn)(void *arg), void *arg)
{
    // Through void * on purpose: a direct cast trips -Wcast-function-type-mismatch. thread_call passes a second
    // argument, which fn ignores (AAPCS64: it lands in x1).
    void *const entry = reinterpret_cast<void *>(fn);
    return thread_call_allocate_with_options(reinterpret_cast<thread_call_func_t>(entry), arg,
                                             THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
}

extern "C" void switch_linux_call_free(void *call)
{
    if (call)
        thread_call_free(callOf(call));
}

extern "C" bool switch_linux_call_enter(void *call)
{
    return thread_call_enter(callOf(call)) != FALSE;
}

extern "C" bool switch_linux_call_enter_after(void *call, uint64_t delay_ns)
{
    if (!delay_ns)
        return switch_linux_call_enter(call);
    return thread_call_enter_delayed(callOf(call), mach_absolute_time() + switch_abs_from_ns(delay_ns)) != FALSE;
}

extern "C" bool switch_linux_call_cancel(void *call)
{
    return thread_call_cancel(callOf(call)) != FALSE;
}

// On a THREAD_CALL_OPTIONS_ONCE call XNU's cancel_wait waits for the run under way even when it cancelled a pending one
// (a plain call's does not), so nothing of the call is running when it returns.
extern "C" bool switch_linux_call_cancel_wait(void *call)
{
    return thread_call_cancel_wait(callOf(call)) != FALSE;
}

extern "C" bool switch_linux_call_flush(void *call, void (*fn)(void *arg), void *arg)
{
    if (!switch_linux_call_cancel_wait(call))
        return false;
    fn(arg);
    return true;
}

extern "C" volatile void *switch_linux_ioremap(uint64_t pa, uint64_t size, int write_combined)
{
    const vm_offset_t va = write_combined ? ml_io_map_wcomb(static_cast<vm_offset_t>(pa), static_cast<vm_size_t>(size))
                                          : ml_io_map(static_cast<vm_offset_t>(pa), static_cast<vm_size_t>(size));
    return reinterpret_cast<volatile void *>(va);
}
