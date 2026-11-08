// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch scheduling stats (debug kernels): every 10 s, two os_log lines with how the threads a frame depends on were
 * scheduled in that window: per thread its class as of its last run (base/sched priority, mode, QoS, clutch bucket) and
 * its runnable-to-running latency (p50/p99/max), with the fail-safe and CPU-limit counts; then the kernel threads that
 * used the most CPU, the uptime of System Boot Complete, and the render server's GPU threads; and, when processes
 * asked for new ones, the blocking per-thread CPU limits they asked for (<switch/cpulimit.h> scales or lifts them).
 *
 * The threads are found by the events that make them what they are: RunningBoard giving an app the focal UI role
 * (SpringBoard's main thread, the frontmost app's), backboardd naming its Core Animation and Metal threads (and so its
 * main thread). Latency is the incoming thread's last_made_runnable_time to its dispatch in thread_invoke: a wakeup or a
 * preemption alike. The reports ride the scheduler's maintenance tick, as SwitchStall's do: no timer of their own.
 */

#include <kern/misc_protos.h>
#include <switch/sched.h>
#include <switch/stall.h>
#include <switch/xnu.h>

#include "stats.h"
#include "SwitchPowerTrail.h"

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/recount.h>
#include <kern/sched.h>
#include <kern/simple_lock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <mach/task_policy.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <ptrauth.h>
#include <string.h>
#include <vm/vm_kern.h>

#if DEVELOPMENT || DEBUG

#define REPORT_NS   (10ull * NSEC_PER_SEC)
#define KERNEL_TOP  3
#define KERNEL_SEEN 192
#define LINE        900u /* under the 1 KiB os_log record */
#define LABEL       24
#define ASKED       16

enum slot_id {
    SB_MAIN,
    BB_MAIN,
    CA_RENDER,
    CA_DISPLAY,
    APP_MAIN,
    RAW_SUBMIT,
    RAW_COMPLETION,
    RAW_POOL,
    M2V_SCHEDULER,
    M2V_QUEUE_WAIT,
    M2V_COMPLETION,
    M2V_HANDLERS,
    SLOTS,
};

enum {
    FIRST_GPU = RAW_SUBMIT
};

/* backboardd's threads by the names they give themselves: Core Animation's, and our Metal driver's. */
static const struct {
    const char *name;
    enum slot_id slot;
    const char *label;
} render_server_threads[] = {
    {"com.apple.coreanimation.render-server", CA_RENDER, "render-server"},
    {"com.apple.coreanimation.display.primary", CA_DISPLAY, "display.primary"},
    {"com.brunostjohn.switch.raw.submit", RAW_SUBMIT, "raw.submit"},
    {"com.brunostjohn.switch.raw.completion", RAW_COMPLETION, "raw.completion"},
    {"com.brunostjohn.switch.raw.pool", RAW_POOL, "raw.pool"},
    {"metal2vulkan-scheduler", M2V_SCHEDULER, "m2v.scheduler"},
    {"metal2vulkan-queue-wait-0", M2V_QUEUE_WAIT, "m2v.queue-wait-0"},
    {"metal2vulkan-completion", M2V_COMPLETION, "m2v.completion"},
    {"metal2vulkan-handlers", M2V_HANDLERS, "m2v.handlers"},
};

/* Counters only ever grow (each is written by the thread's own dispatch or run); a report takes their difference. */
struct slot {
    int pid;
    char label[LABEL];
    uint64_t klass; /* struct switch_sched_class, packed, as of the last run */
    uint32_t hist[SWITCH_SCHED_BUCKETS];
    uint32_t runs, failsafe_runs, limit_blocks;
    uint64_t limit_us;
    uint64_t window_max_us;
    struct switch_sched_window reported; /* the counters at the last report */
};

uint32_t switch_sched_watching;
static uint64_t watched[SLOTS]; /* thread ids, 0 for none: what the dispatch hook compares against */
static struct slot slots[SLOTS];
static SIMPLE_LOCK_DECLARE(slots_lock, 0);
static thread_call_t report_call;
static uint64_t report_abs, report_last, boot_complete_ns;

static struct {
    uint64_t tid, cpu;
} kernel_seen[KERNEL_SEEN];

static void switch_sched_report(thread_call_param_t p0, thread_call_param_t p1);

static int slot_for(uint64_t tid)
{
    for (int i = 0; i < SLOTS; i++) {
        if (os_atomic_load(&watched[i], relaxed) == tid) {
            return i;
        }
    }
    return -1;
}

static uint64_t abs_to_us(uint64_t abs)
{
    uint64_t ns = 0;

    absolutetime_to_nanoseconds(abs, &ns);
    return ns / NSEC_PER_USEC;
}

void switch_sched_dispatched(thread_t thread, uint64_t now)
{
    int i = slot_for(thread->thread_id);
    struct slot *s = NULL;
    uint64_t made = thread->last_made_runnable_time;
    uint64_t us = 0;

    if (i < 0) {
        return;
    }
    s = &slots[i];
    if (made != THREAD_NOT_RUNNABLE && now > made) {
        us = abs_to_us(now - made);
    }
    os_atomic_inc(&s->hist[switch_sched_bucket(us)], relaxed);
    os_atomic_inc(&s->runs, relaxed);
    os_atomic_max(&s->window_max_us, us, relaxed);
    if (thread->sched_flags & TH_SFLAG_FAILSAFE) {
        os_atomic_inc(&s->failsafe_runs, relaxed);
    }
    os_atomic_store(&s->klass,
                    (uint64_t)thread->base_pri | (uint64_t)thread->sched_pri << 8 | (uint64_t)thread->sched_mode << 16 |
                        (uint64_t)thread->effective_policy.thep_qos << 24 | (uint64_t)thread->th_sched_bucket << 32,
                    relaxed);
    if (i == CA_DISPLAY) {
        switch_sched_render_on_core(thread, now);
    }
}

static void watch(enum slot_id id, uint64_t tid, int pid, const char *label, bool replace_same_pid)
{
    if (os_atomic_load(&report_call, acquire) == NULL) {
        thread_call_t call = thread_call_allocate_with_options(switch_sched_report, NULL, THREAD_CALL_PRIORITY_KERNEL,
                                                               THREAD_CALL_OPTIONS_ONCE);

        nanoseconds_to_absolutetime(REPORT_NS, &report_abs);
        if (call != NULL && !os_atomic_cmpxchg(&report_call, NULL, call, release)) {
            thread_call_free(call);
        }
    }
    simple_lock(&slots_lock, LCK_GRP_NULL);
    if (os_atomic_load(&watched[id], relaxed) != tid &&
        (replace_same_pid || slots[id].pid != pid || os_atomic_load(&watched[id], relaxed) == 0)) {
        slots[id].pid = pid;
        strlcpy(slots[id].label, label, sizeof(slots[id].label));
        os_atomic_store(&watched[id], tid, relaxed);
    }
    simple_unlock(&slots_lock);
    os_atomic_store(&switch_sched_watching, 1, relaxed);
}

static uint64_t main_thread_id(task_t task)
{
    uint64_t tid = 0;

    task_lock(task);
    if (!queue_empty(&task->threads)) {
        tid = thread_tid((thread_t)queue_first(&task->threads));
    }
    task_unlock(task);
    return tid;
}

/* proc_set_darwin_role (kern_resource.c), after the role is applied: RunningBoard making an app the focal one. */
void switch_sched_role(task_t task, int role)
{
    char name[32];
    char label[LABEL];
    uint64_t tid = 0;

    switch_stall_frame_role(task, role);
    if (role != TASK_FOREGROUND_APPLICATION) {
        return;
    }
    tid = main_thread_id(task);
    if (tid == 0) {
        return;
    }
    switch_task_name(task, name, sizeof(name));
    if (strcmp(name, "SpringBoard") == 0) {
        watch(SB_MAIN, tid, task_pid(task), "SpringBoard.main", true);
        return;
    }
    snprintf(label, sizeof(label), "%s.main", name);
    watch(APP_MAIN, tid, task_pid(task), label, true);
}

/* bsd_setthreadname (proc_info.c): backboardd's threads as they name themselves, and its main thread with them. */
void switch_sched_named(thread_t thread, const char *name)
{
    task_t task = get_threadtask(thread);
    char process[16];
    uint64_t main_tid = 0;

    if (task == kernel_task || name == NULL) {
        return;
    }
    for (unsigned int i = 0; i < sizeof(render_server_threads) / sizeof(render_server_threads[0]); i++) {
        if (strcmp(name, render_server_threads[i].name) != 0) {
            continue;
        }
        switch_task_name(task, process, sizeof(process));
        if (strcmp(process, "backboardd") != 0) {
            return;
        }
        /* A pool's threads share a name: the first of a process stands for them. */
        watch(render_server_threads[i].slot, thread_tid(thread), task_pid(task), render_server_threads[i].label, false);
        if (render_server_threads[i].slot == CA_RENDER) {
            switch_stall_frame_render_server(thread);
        }
        main_tid = main_thread_id(task);
        if (main_tid != 0) {
            watch(BB_MAIN, main_tid, task_pid(task), "backboardd.main", true);
        }
        return;
    }
}

/* ledger_perform_blocking (ledger.c): the thread sleeps until its CPU limit's ledger refills at until. */
void switch_sched_limit_block(thread_t thread, uint64_t until)
{
    int i = slot_for(thread->thread_id);
    uint64_t now = mach_absolute_time();

    if (i < 0) {
        return;
    }
    os_atomic_inc(&slots[i].limit_blocks, relaxed);
    os_atomic_add(&slots[i].limit_us, until > now ? abs_to_us(until - now) : 0, relaxed);
}

/* thread_set_cpulimit: each process's distinct blocking-limit requests (Apple's percentage and refill), with a count,
 * for the report. Keyed by process name, so a relaunch adds to its predecessor's row. */
static struct {
    char process[16];
    uint8_t percentage;
    bool lifted;
    uint32_t interval_ms, count;
} asked[ASKED];

static uint32_t asked_requests, asked_reported, asked_dropped;
static SIMPLE_LOCK_DECLARE(asked_lock, 0);

void switch_sched_limit_asked(uint8_t percentage, uint64_t interval_ns, bool lifted)
{
    char process[16];
    uint32_t interval_ms = (uint32_t)(interval_ns / NSEC_PER_MSEC);
    unsigned int i = 0;

    switch_task_name(current_task(), process, sizeof(process));
    simple_lock(&asked_lock, LCK_GRP_NULL);
    for (i = 0; i < ASKED; i++) {
        if (asked[i].count == 0) {
            strlcpy(asked[i].process, process, sizeof(asked[i].process));
            asked[i].percentage = percentage;
            asked[i].interval_ms = interval_ms;
            asked[i].lifted = lifted;
        } else if (asked[i].percentage != percentage || asked[i].interval_ms != interval_ms ||
                   strcmp(asked[i].process, process) != 0) {
            continue;
        }
        asked[i].count++;
        break;
    }
    if (i == ASKED) {
        asked_dropped++;
    }
    asked_requests++;
    simple_unlock(&asked_lock);
}

/* The requests so far, when there were new ones since the last report. */
static void report_asked(char *line)
{
    size_t at = (size_t)snprintf(line, LINE, "SwitchSched CPU limits asked (process percent/refill ms x requests):");
    bool fresh = false;

    simple_lock(&asked_lock, LCK_GRP_NULL);
    fresh = asked_requests != asked_reported;
    asked_reported = asked_requests;
    for (unsigned int i = 0; fresh && i < ASKED && asked[i].count != 0 && at < LINE - 1; i++) {
        int n = snprintf(line + at, LINE - at, " %s %u%%/%u%s x%u;", asked[i].process, asked[i].percentage,
                         asked[i].interval_ms, asked[i].lifted ? " lifted" : "", asked[i].count);
        at = n < 0 ? at : MIN(at + (size_t)n, LINE - 1);
    }
    if (fresh && asked_dropped != 0 && at < LINE - 1) {
        (void)snprintf(line + at, LINE - at, " %u more in no row", asked_dropped);
    }
    simple_unlock(&asked_lock);
    if (fresh) {
        os_log(OS_LOG_DEFAULT, "%s", line);
    }
}

/* IOPMrootDomain: backboardd set System Boot Complete, about 3 s after SpringBoard dismissed the boot logo. */
void switch_sched_boot_complete(void)
{
    uint64_t ns = 0;

    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    if (os_atomic_cmpxchg(&boot_complete_ns, 0, ns, relaxed)) {
        switch_power_trail_mark(SWITCH_MARK_BOOT_DONE);
    }
}

/* sched_timeshare_maintenance_continue, on the scheduler's maintenance thread. */
void switch_sched_tick(uint64_t now)
{
    thread_call_t call = os_atomic_load(&report_call, acquire);

    if (call == NULL || now - report_last < report_abs) {
        return;
    }
    report_last = now;
    thread_call_enter(call);
}

static struct switch_sched_window take_window(struct slot *s)
{
    struct switch_sched_window now = {0};
    struct switch_sched_window w = {0};

    for (unsigned int b = 0; b < SWITCH_SCHED_BUCKETS; b++) {
        now.hist[b] = os_atomic_load(&s->hist[b], relaxed);
        w.hist[b] = now.hist[b] - s->reported.hist[b];
    }
    now.runs = os_atomic_load(&s->runs, relaxed);
    now.failsafe_runs = os_atomic_load(&s->failsafe_runs, relaxed);
    now.limit_blocks = os_atomic_load(&s->limit_blocks, relaxed);
    now.limit_us = os_atomic_load(&s->limit_us, relaxed);
    w.runs = now.runs - s->reported.runs;
    w.failsafe_runs = now.failsafe_runs - s->reported.failsafe_runs;
    w.limit_blocks = now.limit_blocks - s->reported.limit_blocks;
    w.limit_us = now.limit_us - s->reported.limit_us;
    w.max_us = os_atomic_xchg(&s->window_max_us, 0, relaxed);
    s->reported = now;
    return w;
}

static struct switch_sched_class unpack(uint64_t k)
{
    return (struct switch_sched_class){
        .base_pri = (uint8_t)k,
        .sched_pri = (uint8_t)(k >> 8),
        .mode = (uint8_t)(k >> 16),
        .qos = (uint8_t)(k >> 24),
        .bucket = (uint8_t)(k >> 32),
    };
}

static size_t put(char *line, size_t at, const char *text)
{
    size_t n = strlcpy(line + at, text, LINE - at);

    return MIN(at + n, LINE - 1);
}

static size_t append_slots(char *line, size_t at, int first, int last)
{
    for (int i = first; i < last && at < LINE - 1; i++) {
        struct switch_sched_window w;
        char label[LABEL];
        int pid = 0;

        if (os_atomic_load(&watched[i], relaxed) == 0) {
            continue;
        }
        simple_lock(&slots_lock, LCK_GRP_NULL);
        strlcpy(label, slots[i].label, sizeof(label));
        pid = slots[i].pid;
        simple_unlock(&slots_lock);
        w = take_window(&slots[i]);
        struct switch_sched_class c = unpack(os_atomic_load(&slots[i].klass, relaxed));
        at = put(line, at, "; ");
        at += switch_sched_format(line + at, LINE - at, label, pid, &c, &w);
    }
    return at;
}

/* The kernel threads with the most CPU since the last report, by name, or unnamed by their continuation and wait. */
static size_t append_kernel_top(char *line, size_t at)
{
    struct {
        thread_t thread;
        uint64_t cpu;
    } top[KERNEL_TOP] = {0};

    thread_t t = NULL;

    task_lock(kernel_task);
    queue_iterate(&kernel_task->threads, t, thread_t, task_threads)
    {
        uint64_t cpu = recount_thread_time_mach(t);
        uint64_t delta = 0;
        unsigned int k = 0;

        if (t->base_pri == IDLEPRI) {
            continue;
        }
        for (k = 0; k < KERNEL_SEEN && kernel_seen[k].tid != 0 && kernel_seen[k].tid != t->thread_id; k++) {
        }
        if (k == KERNEL_SEEN) {
            continue;
        }
        delta = kernel_seen[k].tid == 0 ? cpu : cpu - MIN(cpu, kernel_seen[k].cpu);
        kernel_seen[k].tid = t->thread_id;
        kernel_seen[k].cpu = cpu;
        for (unsigned int j = 0; j < KERNEL_TOP; j++) {
            if (delta > top[j].cpu) {
                memmove(&top[j + 1], &top[j], (KERNEL_TOP - 1 - j) * sizeof(top[0]));
                top[j].thread = t;
                top[j].cpu = delta;
                break;
            }
        }
    }
    at = put(line, at, "; kernel:");
    for (unsigned int j = 0; j < KERNEL_TOP && top[j].thread != THREAD_NULL && at < LINE - 1; j++) {
        char name[MAXTHREADNAMESIZE];
        int n = 0;

        t = top[j].thread;
        thread_get_thread_name(t, name);
        if (name[0] != '\0') {
            n = snprintf(line + at, LINE - at, " %s %u %llu ms", name, (unsigned int)t->base_pri,
                         abs_to_us(top[j].cpu) / 1000);
        } else {
            n = snprintf(line + at, LINE - at, " 0x%llx (cont 0x%lx, wait 0x%lx) %u %llu ms", thread_tid(t),
                         (unsigned long)VM_KERNEL_UNSLIDE(
                             (uintptr_t)ptrauth_strip((void *)t->continuation, ptrauth_key_function_pointer)),
                         (unsigned long)VM_KERNEL_UNSLIDE_OR_PERM(t->wait_event), (unsigned int)t->base_pri,
                         abs_to_us(top[j].cpu) / 1000);
        }
        at = n < 0 ? at : MIN(at + (size_t)n, LINE - 1);
    }
    task_unlock(kernel_task);
    return at;
}

static void switch_sched_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    static uint64_t last_ns;
    static char line[LINE];
    uint64_t now_ns = 0;
    uint64_t window_ms = 0;
    uint64_t boot_ns = os_atomic_load(&boot_complete_ns, relaxed);
    size_t at = 0;

    absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
    window_ms = (now_ns - last_ns) / NSEC_PER_MSEC;
    last_ns = now_ns;
    if (boot_ns != 0) {
        at = (size_t)snprintf(line, LINE,
                              "SwitchSched 1/2 (%llu ms; runnable-to-running us, p50/p99 as power-of-two "
                              "bounds, max): System Boot Complete at %llu.%llu s",
                              window_ms, boot_ns / NSEC_PER_SEC, boot_ns % NSEC_PER_SEC / 100000000);
    } else {
        at = (size_t)snprintf(line, LINE,
                              "SwitchSched 1/2 (%llu ms; runnable-to-running us, p50/p99 as power-of-two "
                              "bounds, max): no System Boot Complete yet",
                              window_ms);
    }
    at = append_slots(line, MIN(at, LINE - 1), SB_MAIN, FIRST_GPU);
    append_kernel_top(line, at);
    os_log(OS_LOG_DEFAULT, "%s", line);

    at = (size_t)snprintf(line, LINE, "SwitchSched 2/2 (the render server's GPU threads)");
    append_slots(line, MIN(at, LINE - 1), FIRST_GPU, SLOTS);
    os_log(OS_LOG_DEFAULT, "%s", line);
    report_asked(line);
}

#endif /* DEVELOPMENT || DEBUG */
