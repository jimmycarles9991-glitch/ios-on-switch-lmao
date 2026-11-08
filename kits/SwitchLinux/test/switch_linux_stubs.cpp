// The stand-ins switch_linux_stubs.h declares: a recorder of every kernel call SwitchLinux.cpp makes.
#include "switch_linux_stubs.h"
#include "switch_linux_record.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

std::vector<std::string> gSwitchLinuxCalls;
boolean_t switch_linux_stub_interrupts = TRUE;
int switch_linux_stub_preemption;
uint64_t switch_linux_stub_now = 5000;
wait_result_t switch_linux_stub_wait_result = THREAD_AWAKENED;

namespace {
// Two "abs" ticks per ns, so a conversion the glue skips shows up in the numbers.
constexpr uint64_t kAbsPerNs = 2;

struct Fake {
    int id;
};

void *fake(const char *kind)
{
    static int next;
    auto *made = new Fake{++next};
    switch_linux_record("%s %d", kind, made->id);
    return made;
}

int idOf(const void *object)
{
    return static_cast<const Fake *>(object)->id;
}
}  // namespace

extern "C" {
void switch_linux_record(const char *fmt, ...)
{
    char line[256];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    gSwitchLinuxCalls.emplace_back(line);
}

void IOLog(const char *fmt, ...)
{
    char line[256];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    switch_linux_record("IOLog %s", line);
}

void *IOMalloc(vm_size_t size)
{
    switch_linux_record("IOMalloc %zu", static_cast<size_t>(size));
    void *block = std::malloc(size);
    std::memset(block, 0xa5, size);
    return block;
}

void *IOMallocZero(vm_size_t size)
{
    switch_linux_record("IOMallocZero %zu", static_cast<size_t>(size));
    return std::calloc(1, size);
}

void IOFree(void *address, vm_size_t size)
{
    switch_linux_record("IOFree %zu", static_cast<size_t>(size));
    std::free(address);
}

uint64_t mach_absolute_time(void)
{
    return switch_linux_stub_now;
}

void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs / kAbsPerNs;
}

void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs)
{
    *abs = ns * kAbsPerNs;
}

void delay_for_interval(uint32_t interval, uint32_t scale)
{
    switch_linux_record("delay %u %u", interval, scale);
}

void delay_for_interval_with_leeway(uint32_t interval, uint32_t leeway, uint32_t scale)
{
    switch_linux_record("delay %u leeway %u %u", interval, leeway, scale);
}

wait_result_t assert_wait(event_t event, wait_interrupt_t)
{
    switch_linux_record("assert_wait %p", event);
    return THREAD_AWAKENED;
}

wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t, uint64_t deadline)
{
    switch_linux_record("assert_wait_deadline %p %llu", event, static_cast<unsigned long long>(deadline));
    return THREAD_AWAKENED;
}

wait_result_t thread_block(void *)
{
    switch_linux_record("thread_block");
    return switch_linux_stub_wait_result;
}

void thread_wakeup_prim(event_t event, boolean_t one, wait_result_t)
{
    switch_linux_record("wakeup%s %p", one ? "_one" : "", event);
}

lck_grp_t *lck_grp_alloc_init(const char *name, void *)
{
    switch_linux_record("lck_grp %s", name);
    static int group;
    return reinterpret_cast<lck_grp_t *>(&group);
}

void lck_grp_free(lck_grp_t *)
{
    switch_linux_record("lck_grp_free");
}

lck_mtx_t *lck_mtx_alloc_init(lck_grp_t *, void *)
{
    return static_cast<lck_mtx_t *>(fake("lck_mtx"));
}

void lck_mtx_free(lck_mtx_t *mutex, lck_grp_t *)
{
    switch_linux_record("lck_mtx_free %d", idOf(mutex));
    delete reinterpret_cast<Fake *>(mutex);
}

void lck_mtx_lock(lck_mtx_t *mutex)
{
    switch_linux_record("lck_mtx_lock %d", idOf(mutex));
}

void lck_mtx_unlock(lck_mtx_t *mutex)
{
    switch_linux_record("lck_mtx_unlock %d", idOf(mutex));
}

boolean_t lck_mtx_try_lock(lck_mtx_t *mutex)
{
    switch_linux_record("lck_mtx_try_lock %d", idOf(mutex));
    return TRUE;
}

wait_result_t lck_mtx_sleep(lck_mtx_t *mutex, lck_sleep_action_t, event_t event, wait_interrupt_t)
{
    switch_linux_record("lck_mtx_sleep %d %p", idOf(mutex), event);
    return switch_linux_stub_wait_result;
}

wait_result_t lck_mtx_sleep_deadline(lck_mtx_t *mutex, lck_sleep_action_t, event_t event, wait_interrupt_t,
                                     uint64_t deadline)
{
    switch_linux_record("lck_mtx_sleep_deadline %d %p %llu", idOf(mutex), event,
                        static_cast<unsigned long long>(deadline));
    return switch_linux_stub_wait_result;
}

lck_rw_t *lck_rw_alloc_init(lck_grp_t *, void *)
{
    return static_cast<lck_rw_t *>(fake("lck_rw"));
}

void lck_rw_free(lck_rw_t *rw, lck_grp_t *)
{
    switch_linux_record("lck_rw_free %d", idOf(rw));
    delete reinterpret_cast<Fake *>(rw);
}

void lck_rw_lock_shared(lck_rw_t *rw)
{
    switch_linux_record("lck_rw_lock_shared %d", idOf(rw));
}

void lck_rw_unlock_shared(lck_rw_t *rw)
{
    switch_linux_record("lck_rw_unlock_shared %d", idOf(rw));
}

void lck_rw_lock_exclusive(lck_rw_t *rw)
{
    switch_linux_record("lck_rw_lock_exclusive %d", idOf(rw));
}

void lck_rw_unlock_exclusive(lck_rw_t *rw)
{
    switch_linux_record("lck_rw_unlock_exclusive %d", idOf(rw));
}

struct thread_call {
    thread_call_func_t func;
    thread_call_param_t param0;
    bool pending;
};

thread_call_t thread_call_allocate_with_options(thread_call_func_t func, thread_call_param_t param0,
                                                thread_call_priority_t pri, thread_call_options_t options)
{
    switch_linux_record("thread_call_allocate priority %d options %u", pri, options);
    return new thread_call{func, param0, false};
}

boolean_t thread_call_free(thread_call_t call)
{
    switch_linux_record("thread_call_free");
    delete call;
    return TRUE;
}

boolean_t thread_call_enter(thread_call_t call)
{
    switch_linux_record("thread_call_enter");
    const bool was = call->pending;
    call->pending = true;
    return was ? TRUE : FALSE;
}

boolean_t thread_call_enter_delayed(thread_call_t call, uint64_t deadline)
{
    switch_linux_record("thread_call_enter_delayed %llu", static_cast<unsigned long long>(deadline));
    const bool was = call->pending;
    call->pending = true;
    return was ? TRUE : FALSE;
}

boolean_t thread_call_cancel(thread_call_t call)
{
    switch_linux_record("thread_call_cancel");
    const bool was = call->pending;
    call->pending = false;
    return was ? TRUE : FALSE;
}

boolean_t thread_call_cancel_wait(thread_call_t call)
{
    switch_linux_record("thread_call_cancel_wait");
    const bool was = call->pending;
    call->pending = false;
    return was ? TRUE : FALSE;
}

boolean_t ml_set_interrupts_enabled(boolean_t enable)
{
    const boolean_t was = switch_linux_stub_interrupts;
    switch_linux_stub_interrupts = enable;
    return was;
}

boolean_t ml_get_interrupts_enabled(void)
{
    return switch_linux_stub_interrupts;
}

int get_preemption_level(void)
{
    return switch_linux_stub_preemption;
}

int cpu_number(void)
{
    return 2;
}

void thread_yield_to_preemption(void)
{
    switch_linux_record("thread_yield_to_preemption");
}

vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size)
{
    switch_linux_record("ml_io_map %#lx %#lx", static_cast<unsigned long>(phys_addr), static_cast<unsigned long>(size));
    return phys_addr + 0x1000000;
}

vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size)
{
    switch_linux_record("ml_io_map_wcomb %#lx %#lx", static_cast<unsigned long>(phys_addr),
                        static_cast<unsigned long>(size));
    return phys_addr + 0x2000000;
}
}

// The thread call's own entry, as XNU would run it.
void switch_linux_stub_fire(void *call)
{
    auto *c = static_cast<thread_call *>(call);
    c->pending = false;
    c->func(c->param0, nullptr);
}
