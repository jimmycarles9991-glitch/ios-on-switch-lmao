#include "switch_ipi_probe.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdlib.h>

struct switch_ipi_pingpong {
    semaphore_t semaphore[2];
    unsigned rounds;
    uint64_t signalled[2]; /* when each side last signalled the other: written before the signal, read after the wake */
};

struct switch_ipi_pingpong *switch_ipi_pingpong_create(unsigned rounds)
{
    struct switch_ipi_pingpong *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->rounds = rounds;
    if (semaphore_create(mach_task_self(), &p->semaphore[0], SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        free(p);
        return NULL;
    }
    if (semaphore_create(mach_task_self(), &p->semaphore[1], SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        semaphore_destroy(mach_task_self(), p->semaphore[0]);
        free(p);
        return NULL;
    }
    return p;
}

void switch_ipi_pingpong_destroy(struct switch_ipi_pingpong *p)
{
    if (!p)
        return;
    semaphore_destroy(mach_task_self(), p->semaphore[0]);
    semaphore_destroy(mach_task_self(), p->semaphore[1]);
    free(p);
}

static void spin(uint64_t ticks)
{
    const uint64_t until = mach_absolute_time() + ticks;
    while (mach_absolute_time() < until) {
    }
}

static bool wait_for(struct switch_ipi_pingpong *p, unsigned me, mach_timespec_t limit)
{
    kern_return_t kr = KERN_ABORTED;
    while (kr == KERN_ABORTED)
        kr = semaphore_timedwait(p->semaphore[me], limit);
    return kr == KERN_SUCCESS;
}

static void wake(struct switch_ipi_pingpong *p, unsigned me, uint64_t hold)
{
    __atomic_store_n(&p->signalled[me], mach_absolute_time(), __ATOMIC_RELAXED);
    semaphore_signal(p->semaphore[1 - me]);
    spin(hold);
}

unsigned switch_ipi_pingpong_side(struct switch_ipi_pingpong *p, bool first, uint64_t hold, uint64_t timeout,
                                  uint64_t *latency)
{
    const unsigned me = first ? 0 : 1;
    mach_timebase_info_data_t base = {0, 0};
    mach_timebase_info(&base);
    const uint64_t ns = timeout * base.numer / base.denom;
    const mach_timespec_t limit = {(unsigned)(ns / 1000000000u), (clock_res_t)(ns % 1000000000u)};
    for (unsigned i = 0; i < p->rounds; ++i) {
        if (first)
            wake(p, me, hold);
        if (!wait_for(p, me, limit))
            return p->rounds - i;
        latency[i] = mach_absolute_time() - __atomic_load_n(&p->signalled[1 - me], __ATOMIC_RELAXED);
        if (!first)
            wake(p, me, hold);
    }
    return 0;
}
