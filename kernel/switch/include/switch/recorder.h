// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder (kernel/switch/recorder), in a debug kernel built with --//kernel:recorder (SWITCH_RECORDER).
 * kdebug runs from boot as a wrapping ring through a typefilter, kperf's timer samples the running thread's kernel and
 * user stacks into it, and a trigger (a late frame, a UI main thread's busy stretch of SWITCH_RECORDER_HANG_NS, a mark)
 * has the ring and a stackshot copied into memory, later written to a dump in mobile's CrashReporter, where
 * `idevicecrashreport` fetches it. Without the setting
 * every hook below is Apple's expression unchanged and no recorder function is declared, so a call left outside these
 * macros does not compile.
 */
#ifndef SWITCH_RECORDER_HOOKS_H
#define SWITCH_RECORDER_HOOKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/* Our tracepoints share the frame timeline's class and subclass (TegraDisplayFlip/SwitchFrameTimeline.h: DBG_DRIVERS,
 * DBG_DRVGRAPHICS, codes from 0x3f0); the recorder's take 0x3e0-0x3e7, the GPU job ring's 0x3e8-0x3ef, the SD queue's
 * (NativeSwitchBlock) 0x3d8-0x3df. */
#define SWITCH_RECORDER_KDBG(code)   ((6u << 24) | (18u << 16) | ((uint32_t)(code) << 2))
#define SWITCH_RECORDER_KDBG_TRIGGER 0x3e0 /* reason, thread id (0: none), value */
#define SWITCH_RECORDER_KDBG_DUMP    0x3e1 /* DBG_FUNC_START / END around a dump: reason, slot */

#if SWITCH_RECORDER
#if !(DEVELOPMENT || DEBUG)
#error "--//kernel:recorder is for debug kernels only"
#endif

#define SWITCH_RECORDER_RING_EVENTS (1u << 18) /* 16 MiB of 64-byte events */
/* kdebug_startup: without a trace= boot-arg the recorder's ring, typefilter and wrapping; with one, Apple's. */
#define SWITCH_RECORDER_EVENTS(n)    ((n) ? (n) : SWITCH_RECORDER_RING_EVENTS)
#define SWITCH_RECORDER_FILTER(n, f) switch_recorder_boot_filter((n), (f))
#define SWITCH_RECORDER_OPTS(n, o)   ((n) ? (o) : ((o) | KDOPT_WRAPPING))
/* kdbg_set_typefilter_string's line per class: none for the recorder's own filter (its summary line stays). */
#define SWITCH_RECORDER_QUIET(call)        \
    do {                                   \
        if (!switch_recorder_own_filter) { \
            call;                          \
        }                                  \
    } while (0)
/* kdebug's copy of merged events to a reader's buffer: the recorder's capture thread reads into a kernel buffer
 * (switch_recorder_kdebug_read), which copyout would refuse. */
#define SWITCH_RECORDER_COPYOUT(src, dst, size) \
    (switch_recorder_dumping() ? (memcpy((void *)(uintptr_t)(dst), (src), (size)), 0) : copyout((src), (dst), (size)))
/* kern_shutdown.c: Apple's shutdown trace only for tracing someone asked for, not the recorder's own ring. */
#define SWITCH_RECORDER_SHUTDOWN_TRACE(enabled) ((enabled) && !switch_recorder_owns_ring())
/* kperf_init_early: without Apple's kperf= boot-arg, the recorder's timer and samplers, then its stack depths. */
#define SWITCH_RECORDER_KPERF(found, config, size) ((found) = switch_recorder_kperf((found), (config), (size)))
#define SWITCH_RECORDER_KPERF_CONFIGURED()         switch_recorder_kperf_configured()
/* kptimer_fire, in the timer interrupt: one sample taken on this CPU. */
#define SWITCH_RECORDER_SAMPLED() switch_recorder_sampled()

extern bool switch_recorder_own_filter;
char *switch_recorder_boot_filter(unsigned int events, char *filter);
bool switch_recorder_dumping(void);
bool switch_recorder_owns_ring(void);
int switch_recorder_kdebug_read(void *buf, size_t size, size_t *events);
bool switch_recorder_kperf(bool found, char *config, size_t size);
void switch_recorder_kperf_configured(void);
void switch_recorder_sampled(void);
/* recorder.c and sample.c, between themselves. */
void switch_recorder_start(void);
uint64_t switch_recorder_samples(void);
/*
 * A trigger records one tracepoint and asks for a capture (recorder/recorder.h's switch_recorder_admit decides); it
 * says whether the ask was taken. It never blocks or allocates, and runs in any context but under a thread or
 * scheduler lock: it takes the thread-call group's lock. The kexts call the named ones through weak stubs.
 */
bool switch_recorder_trigger(unsigned int reason, uint64_t tid, uint64_t value);
bool switch_recorder_hang(uint64_t tid, uint64_t ms, uint32_t on_ms, uint32_t blocked_ms, uint32_t runnable_ms);
void switch_recorder_late_frame(uint64_t interval_us);
void switch_recorder_mark(void);
/* sched/stats.c and diag/stall.c: the UI main threads to watch for hangs, and the scheduler tick (watch.c). */
struct thread;
struct task;
void switch_recorder_exec(struct thread *thread);
void switch_recorder_role(struct task *task, int role, struct thread *main);
void switch_recorder_tick(uint64_t now);
#else
#define SWITCH_RECORDER_EVENTS(n)                  (n)
#define SWITCH_RECORDER_FILTER(n, f)               (f)
#define SWITCH_RECORDER_OPTS(n, o)                 (o)
#define SWITCH_RECORDER_QUIET(call)                call
#define SWITCH_RECORDER_COPYOUT(src, dst, size)    copyout((src), (dst), (size))
#define SWITCH_RECORDER_SHUTDOWN_TRACE(enabled)    (enabled)
#define SWITCH_RECORDER_KPERF(found, config, size) ((void)0)
#define SWITCH_RECORDER_KPERF_CONFIGURED()         ((void)0)
#define SWITCH_RECORDER_SAMPLED()                  ((void)0)
#endif

__END_DECLS

#endif
