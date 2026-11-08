// Host stand-ins for what kits/SwitchLinux's locks take from the kernel: a per-thread preemption level and interrupt
// mask, the thread identity TPIDR_EL1 gives, a clock the test can move, and panic as a message and abort().
#ifndef SWITCH_TEST_SWITCH_LINUX_LOCK_STUBS_H
#define SWITCH_TEST_SWITCH_LINUX_LOCK_STUBS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t mach_absolute_time(void);
void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs);
__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);

int switch_linux_model_preemption_level(void);
int switch_linux_model_irqs_on(void);
void switch_linux_model_set_irqs_on(int on);
// Each look at the clock moves it on by ns_per_read (0: it stands still).
void switch_linux_model_tick_clock(uint64_t ns_per_read);

#ifdef __cplusplus
}
#endif

#endif
