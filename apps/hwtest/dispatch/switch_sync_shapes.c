/* The two retarget shapes. Queues made inactive on one workloop, retargeted to the other and activated while threads
 * already wait in them. And a queue retargeted from one serial hierarchy to another while a dispatch_sync waits on it:
 * the waiter computed its wait on the first hierarchy's workloop, the queue's drainer now serves the second, so the
 * hand-off wakes the waiter by deleting its knote on the first (_dispatch_event_loop_cancel_waiter, the kernel's
 * EV_DELETE of an unwoken NOTE_WL_SYNC_WAIT). */
#include "switch_sync_internal.h"

#include <sched.h>
#include <stdio.h>
#include <time.h>

/* How long the shaper looks for the waiter blocked in its wait before it lets the queue go on regardless. */
#define SYNC_SHAPE_LOOK_NS (50 * NSEC_PER_MSEC)

static char sync_home_key;

void sync_make_moved(struct sync_probe *p)
{
    const dispatch_queue_attr_t inactive = dispatch_queue_attr_make_initially_inactive(DISPATCH_QUEUE_SERIAL);
    char label[96];
    for (unsigned i = 0; i < SYNC_MOVED; i++) {
        struct sync_queue *q = &p->serial[SYNC_MOVED_BASE + i];
        snprintf(label, sizeof(label), "com.brunostjohn.switch.hwtest.handoff.%s", q->label);
        q->queue = dispatch_queue_create_with_target(label, inactive, p->workloop[i % SYNC_WORKLOOPS]);
        dispatch_set_target_queue(q->queue, p->workloop[q->rank]);
        q->hierarchy = &p->hierarchy[q->rank];
    }
}

static void *early_waiter(void *raw)
{
    const struct sync_arg *a = raw;
    struct sync_probe *p = a->p;
    struct sync_queue *q = &p->serial[SYNC_MOVED_BASE + (a->slot - SYNC_EARLY_SLOT) / SYNC_EARLY_WAITERS];
    v80_store(&p->slots[a->slot].tid, sync_self_tid());
    struct sync_activity activity;
    sync_activity_enter(p, &activity, false);
    struct sync_call call = {p, q, NULL, activity.id, a->slot};
    sync_slot_set(p, a->slot, SYNC_WAITING, q->id, 0);
    dispatch_sync_f(q->queue, &call, sync_serial_block);
    v80_add(&q->made, 1);
    v80_add(&p->inactive_waiters, 1);
    sync_activity_leave(&activity);
    sync_slot_set(p, a->slot, SYNC_DONE, 0, 0);
    sync_finish(p);
    return NULL;
}

bool sync_start_early_waiters(struct sync_probe *p)
{
    for (unsigned slot = SYNC_EARLY_SLOT; slot < SYNC_SHAPER_SLOT; slot++) {
        if (!sync_start(p, slot, early_waiter))
            return false;
    }
    return true;
}

void sync_activate_moved(struct sync_probe *p)
{
    for (unsigned i = 0; i < SYNC_MOVED; i++)
        dispatch_activate(p->serial[SYNC_MOVED_BASE + i].queue);
}

void sync_make_shape(struct sync_probe *p)
{
    struct sync_shape *s = &p->shape;
    char label[96];
    for (unsigned h = 0; h < 2; h++) {
        snprintf(label, sizeof(label), "com.brunostjohn.switch.hwtest.handoff.home%u", h);
        s->home[h] = dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(s->home[h], &sync_home_key, (void *)(uintptr_t)(h + 1), NULL);
    }
    s->started = dispatch_semaphore_create(0);
    s->release = dispatch_semaphore_create(0);
    s->go = dispatch_semaphore_create(0);
    s->finished = dispatch_semaphore_create(0);
    for (unsigned r = 0; r < p->config.retarget_rounds; r++) {
        struct sync_queue *q = &s->moving[r];
        q->id = SYNC_MOVING_ID + r;
        snprintf(q->label, sizeof(q->label), "moving%u", r);
        snprintf(label, sizeof(label), "com.brunostjohn.switch.hwtest.handoff.%s", q->label);
        q->queue = dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL);
    }
}

void sync_release_shape(struct sync_probe *p)
{
    struct sync_shape *s = &p->shape;
    for (unsigned r = 0; r < p->config.retarget_rounds; r++)
        dispatch_release(s->moving[r].queue);
    for (unsigned h = 0; h < 2; h++)
        dispatch_release(s->home[h]);
    dispatch_release(s->started);
    dispatch_release(s->release);
    dispatch_release(s->go);
    dispatch_release(s->finished);
}

struct sync_hold {
    struct sync_probe *p;
    struct sync_queue *q;
};

static void hold_block(void *raw)
{
    struct sync_hold *h = raw;
    struct sync_shape *s = &h->p->shape;
    const uint64_t me = sync_self_tid();
    sync_enter(h->p, h->q, me);
    dispatch_semaphore_signal(s->started);
    dispatch_semaphore_wait(s->release, DISPATCH_TIME_FOREVER);
    sync_leave(h->p, h->q, me);
}

struct sync_handed_call {
    struct sync_call call;
    unsigned round;
};

static void handed_block(void *raw)
{
    struct sync_handed_call *h = raw;
    if (dispatch_get_specific(&sync_home_key) == (void *)(uintptr_t)2)
        v80_or(&h->call.p->shape.woken_on_second, 1ull << h->round);
    sync_serial_block(&h->call);
}

static void *handed_waiter(void *raw)
{
    const struct sync_arg *a = raw;
    struct sync_probe *p = a->p;
    struct sync_shape *s = &p->shape;
    v80_store(&p->slots[a->slot].tid, sync_self_tid());
    struct sync_activity activity;
    sync_activity_enter(p, &activity, false);
    for (unsigned r = 0; r < p->config.retarget_rounds; r++) {
        dispatch_semaphore_wait(s->go, DISPATCH_TIME_FOREVER);
        struct sync_queue *q = &s->moving[r];
        struct sync_handed_call h = {{p, q, NULL, activity.id, a->slot}, r};
        sync_slot_set(p, a->slot, SYNC_WAITING, q->id, 0);
        dispatch_sync_f(q->queue, &h, handed_block);
        v80_add(&q->made, 1);
        sync_slot_set(p, a->slot, SYNC_IDLE, 0, 0);
        dispatch_semaphore_signal(s->finished);
    }
    sync_activity_leave(&activity);
    sync_slot_set(p, a->slot, SYNC_DONE, 0, 0);
    sync_finish(p);
    return NULL;
}

/* Whether the handed waiter is blocked in its dispatch_sync on `queue` within SYNC_SHAPE_LOOK_NS. A look, not a
 * judgement: a round whose waiter was not seen blocked still runs, and only counts as unshaped. */
static bool seen_waiting(struct sync_probe *p, unsigned queue)
{
    const mach_port_t port = pthread_mach_thread_np(p->threads[SYNC_HANDED_SLOT]);
    const uint64_t waiting = (uint64_t)SYNC_WAITING << 16 | (uint64_t)queue << 8;
    const uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + SYNC_SHAPE_LOOK_NS;
    do {
        thread_basic_info_data_t info;
        mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
        if (v80_load(&p->slots[SYNC_HANDED_SLOT].state) == waiting &&
            thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &n) == KERN_SUCCESS &&
            info.run_state == TH_STATE_WAITING)
            return true;
        sched_yield();
    } while (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) < until);
    return false;
}

/* Each round: the queue targets the first home and a block holds it; the retarget to the second home queues behind
 * that block as a barrier; the waiter's dispatch_sync queues behind the barrier, waiting on the first home's workloop;
 * then the block lets go. */
static void *shaper(void *raw)
{
    const struct sync_arg *a = raw;
    struct sync_probe *p = a->p;
    struct sync_shape *s = &p->shape;
    v80_store(&p->slots[a->slot].tid, sync_self_tid());
    for (unsigned r = 0; r < p->config.retarget_rounds; r++) {
        struct sync_queue *q = &s->moving[r];
        struct sync_hold hold = {p, q};
        sync_slot_set(p, a->slot, SYNC_SHAPING, q->id, 0);
        dispatch_set_target_queue(q->queue, s->home[0]);
        dispatch_async_f(q->queue, &hold, hold_block);
        dispatch_semaphore_wait(s->started, DISPATCH_TIME_FOREVER);
        sync_slot_set(p, a->slot, SYNC_SHAPING, q->id, 1);
        dispatch_set_target_queue(q->queue, s->home[1]);
        dispatch_semaphore_signal(s->go);
        if (seen_waiting(p, q->id))
            v80_or(&s->parked_rounds, 1ull << r);
        dispatch_semaphore_signal(s->release);
        sync_slot_set(p, a->slot, SYNC_SHAPING, q->id, 2);
        dispatch_semaphore_wait(s->finished, DISPATCH_TIME_FOREVER);
        v80_add(&p->retarget_rounds, 1);
    }
    sync_slot_set(p, a->slot, SYNC_DONE, 0, 0);
    sync_finish(p);
    return NULL;
}

bool sync_start_shape(struct sync_probe *p)
{
    return sync_start(p, SYNC_HANDED_SLOT, handed_waiter) && sync_start(p, SYNC_SHAPER_SLOT, shaper);
}
