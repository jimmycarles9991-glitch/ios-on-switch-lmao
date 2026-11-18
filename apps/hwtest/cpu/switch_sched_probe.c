#include "switch_sched_probe.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

bool switch_sched_make_realtime(uint64_t period, uint64_t computation, uint64_t constraint)
{
    thread_time_constraint_policy_data_t policy = {
        .period = (uint32_t)period,
        .computation = (uint32_t)computation,
        .constraint = (uint32_t)constraint,
        .preemptible = 1,
    };

    const thread_t self = pthread_mach_thread_np(pthread_self());
    if (thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
        return false;
    thread_time_constraint_policy_data_t held = {0};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t fallback = FALSE;
    return thread_policy_get(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&held, &count, &fallback) ==
               KERN_SUCCESS &&
           !fallback && held.period == policy.period;
}

void switch_sched_spin(const volatile bool *stop, struct switch_sched_spin *out)
{
    uint64_t last = mach_absolute_time();
    uint64_t passes = 0, longest = 0;

    while (!*stop) {
        const uint64_t now = mach_absolute_time();
        if (now - last > longest)
            longest = now - last;
        last = now;
        passes++;
    }
    out->passes = passes;
    out->longest_stall = longest;
}

void switch_sched_timer_wakes(uint64_t period, unsigned count, uint64_t *late)
{
    uint64_t deadline = mach_absolute_time() + period;

    for (unsigned i = 0; i < count; i++) {
        mach_wait_until(deadline);
        const uint64_t now = mach_absolute_time();
        late[i] = now - deadline;
        deadline = now + period;
    }
}

struct switch_sched_handoff {
    semaphore_t semaphore;
    unsigned count;
    _Atomic uint64_t signaled_at;
    _Atomic unsigned taken;
};

struct switch_sched_handoff *switch_sched_handoff_create(unsigned count)
{
    struct switch_sched_handoff *h = calloc(1, sizeof(*h));

    if (!h)
        return NULL;
    if (semaphore_create(mach_task_self(), &h->semaphore, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        free(h);
        return NULL;
    }
    h->count = count;
    return h;
}

void switch_sched_handoff_destroy(struct switch_sched_handoff *h)
{
    semaphore_destroy(mach_task_self(), h->semaphore);
    free(h);
}

void switch_sched_handoff_signal(struct switch_sched_handoff *h, uint64_t gap)
{
    for (unsigned i = 0; i < h->count; i++) {
        while (atomic_load_explicit(&h->taken, memory_order_acquire) != i) {
        }
        const uint64_t until = mach_absolute_time() + gap;
        while (mach_absolute_time() < until) {
        }
        atomic_store_explicit(&h->signaled_at, mach_absolute_time(), memory_order_release);
        semaphore_signal(h->semaphore);
    }
}

void switch_sched_handoff_wait(struct switch_sched_handoff *h, uint64_t *latency)
{
    for (unsigned i = 0; i < h->count; i++) {
        while (semaphore_wait(h->semaphore) == KERN_ABORTED) {
        }
        const uint64_t now = mach_absolute_time();
        latency[i] = now - atomic_load_explicit(&h->signaled_at, memory_order_acquire);
        atomic_store_explicit(&h->taken, i + 1, memory_order_release);
    }
}
