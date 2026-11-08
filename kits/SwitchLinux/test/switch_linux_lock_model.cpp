// The kernel the locks run on, per host thread: preemption level, interrupt mask, thread identity; one shared clock.
#include "switch_linux_lock_stubs.h"
#include "kits/SwitchLinux/SwitchLinuxLock.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace {
thread_local int tPreemption;
thread_local int tIrqsOn = 1;
thread_local char tThread;
std::atomic<uint64_t> gClockNs{1000};
std::atomic<uint64_t> gTickNs{0};
}  // namespace

extern "C" {
uint64_t mach_absolute_time(void)
{
    return gClockNs.fetch_add(gTickNs.load());
}

void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs)
{
    *abs = ns;
}

void panic(const char *fmt, ...)
{
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, fmt);
    std::fputs("panic: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputs("\n", stderr);
    va_end(ap);
    std::abort();
}

void _disable_preemption(void)  // NOLINT(bugprone-reserved-identifier): XNU's name
{
    ++tPreemption;
}

void _enable_preemption(void)  // NOLINT(bugprone-reserved-identifier): XNU's name
{
    if (--tPreemption < 0)
        panic("preemption enabled more often than disabled");
}

unsigned long switch_linux_irq_save(void)
{
    const unsigned long was = tIrqsOn ? 1UL : 0UL;
    tIrqsOn = 0;
    return was;
}

void switch_linux_irq_restore(unsigned long flags)
{
    tIrqsOn = flags ? 1 : 0;
}

uintptr_t switch_linux_model_thread(void)
{
    return reinterpret_cast<uintptr_t>(&tThread);
}

int switch_linux_model_preemption_level(void)
{
    return tPreemption;
}

int switch_linux_model_irqs_on(void)
{
    return tIrqsOn;
}

void switch_linux_model_set_irqs_on(int on)
{
    tIrqsOn = on;
}

void switch_linux_model_tick_clock(uint64_t ns_per_read)
{
    gTickNs = ns_per_read;
}
}
