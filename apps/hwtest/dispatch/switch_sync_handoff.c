#include "switch_sync_internal.h"

#include <os/object.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Every class of waiter at once, so a sync waiter's push meets owners of other classes (overrides, turnstile
 * hand-offs). */
static const qos_class_t sync_qos[] = {QOS_CLASS_UTILITY, QOS_CLASS_DEFAULT, QOS_CLASS_USER_INITIATED,
                                       QOS_CLASS_USER_INTERACTIVE};

static __thread uint64_t own_tid;

uint64_t sync_self_tid(void)
{
    if (!own_tid)
        pthread_threadid_np(NULL, &own_tid);
    return own_tid;
}

void sync_slot_set(struct sync_probe *p, unsigned slot, enum sync_phase phase, unsigned queue, unsigned inner)
{
    if (slot < SYNC_SLOTS)
        v80_store(&p->slots[slot].state, (uint64_t)phase << 16 | (uint64_t)queue << 8 | inner);
}

void sync_finish(struct sync_probe *p)
{
    if (v80_add(&p->remaining, UINT64_MAX) == 0)
        semaphore_signal(p->done);
}

static uint64_t next_random(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* A block that found another thread inside its queue stays inside for good: if it is a waiter that left its wait
 * early, the frame libdispatch still links stays alive, and the queue wedges instead of a drainer reading a reused
 * stack. */
static void park(struct sync_probe *p)
{
    v80_add(&p->parked, 1);
    semaphore_signal(p->done);
    for (;;)
        pause();
}

static void overlapped(struct sync_probe *p, unsigned id)
{
    if (v80_add(&p->overlaps, 1) == 1)
        v80_store(&p->first_overlap, id + 1);
    park(p);
}

void sync_enter(struct sync_probe *p, struct sync_queue *q, uint64_t me)
{
    bool clash = v80_swap(&q->owner, me) != 0;
    if (q->hierarchy && v80_swap(q->hierarchy, me) != 0)
        clash = true;
    if (clash)
        overlapped(p, q->id);
}

void sync_leave(struct sync_probe *p, struct sync_queue *q, uint64_t me)
{
    bool clash = q->hierarchy && !v80_cas(q->hierarchy, me, 0);
    if (!v80_cas(&q->owner, me, 0))
        clash = true;
    if (clash)
        overlapped(p, q->id);
}

static void check_activity(struct sync_probe *p, uint64_t want)
{
    if (os_activity_get_identifier(OS_ACTIVITY_CURRENT, NULL) != want)
        v80_add(&p->activity_mismatches, 1);
}

void sync_activity_enter(struct sync_probe *p, struct sync_activity *a, bool counted)
{
    a->activity =
        os_activity_create("com.brunostjohn.switch.hwtest.sync_handoff", OS_ACTIVITY_CURRENT, OS_ACTIVITY_FLAG_DEFAULT);
    os_activity_scope_enter(a->activity, &a->scope);
    a->id = os_activity_get_identifier(a->activity, NULL);
    if (counted && a->id)
        v80_add(&p->activities, 1);
}

void sync_activity_leave(struct sync_activity *a)
{
    os_activity_scope_leave(&a->scope);
    os_release(a->activity);
}

static void inject_once(struct sync_probe *p, struct sync_queue *q, uint64_t me)
{
    if (p->config.inject == SWITCH_SYNC_BREAK_OVERLAP && v80_cas(&p->injected, 0, 1))
        sync_enter(p, q, me);
    if (p->config.inject == SWITCH_SYNC_BREAK_WEDGE && v80_cas(&p->injected, 0, 1)) {
        for (;;)
            pause();
    }
}

void sync_serial_block(void *raw)
{
    struct sync_call *c = raw;
    struct sync_probe *p = c->p;
    struct sync_queue *q = c->q;
    const uint64_t me = sync_self_tid();
    check_activity(p, c->activity);
    sync_enter(p, q, me);
    sync_slot_set(p, c->slot, SYNC_INSIDE, q->id, 0);
    q->ran++;
    if (c->slot == 0)
        inject_once(p, q, me);
    if (c->inner) {
        struct sync_call inner = {p, c->inner, NULL, c->activity, c->slot};
        sync_slot_set(p, c->slot, SYNC_NESTED, q->id, c->inner->id);
        dispatch_sync_f(c->inner->queue, &inner, sync_serial_block);
        v80_add(&c->inner->made, 1);
        sync_slot_set(p, c->slot, SYNC_INSIDE, q->id, 0);
    }
    sync_leave(p, q, me);
}

static void reader_block(void *raw)
{
    struct sync_call *c = raw;
    struct sync_concurrent *cq = &c->p->concurrent;
    check_activity(c->p, c->activity);
    if (v80_add(&cq->state, 1) & SYNC_BARRIER)
        overlapped(c->p, SYNC_CONCURRENT_ID);
    v80_add(&cq->reader_ran, 1);
    if (v80_add(&cq->state, UINT64_MAX) & SYNC_BARRIER)
        overlapped(c->p, SYNC_CONCURRENT_ID);
}

static void barrier_block(void *raw)
{
    struct sync_call *c = raw;
    struct sync_concurrent *cq = &c->p->concurrent;
    check_activity(c->p, c->activity);
    if (v80_or(&cq->state, SYNC_BARRIER) != 0)
        overlapped(c->p, SYNC_CONCURRENT_ID);
    cq->barrier_ran++;
    if (!v80_cas(&cq->state, SYNC_BARRIER, 0))
        overlapped(c->p, SYNC_CONCURRENT_ID);
}

/* A queue of a higher rank than `q` from a random start, NULL when there is none. */
static struct sync_queue *higher(struct sync_probe *p, const struct sync_queue *q, uint64_t r)
{
    for (unsigned i = 0; i < SYNC_SERIALS; i++) {
        struct sync_queue *o = &p->serial[(r + i) % SYNC_SERIALS];
        if (o->rank > q->rank)
            return o;
    }
    return NULL;
}

static void concurrent_once(struct sync_probe *p, struct sync_call *call, uint64_t r)
{
    struct sync_concurrent *cq = &p->concurrent;
    sync_slot_set(p, call->slot, SYNC_WAITING, SYNC_CONCURRENT_ID, 0);
    if (r & 1) {
        dispatch_barrier_sync_f(cq->queue, call, barrier_block);
        v80_add(&cq->barrier_made, 1);
        v80_add(&p->barriers, 1);
    } else if (r & 2) {
        dispatch_async_and_wait_f(cq->queue, call, reader_block);
        v80_add(&cq->reader_made, 1);
        v80_add(&p->async_and_waits, 1);
    } else {
        dispatch_sync_f(cq->queue, call, reader_block);
        v80_add(&cq->reader_made, 1);
    }
}

/* One call: a quarter nested one level into a queue of a higher rank, an eighth on the concurrent queue, an eighth
 * async-and-wait, the rest a plain dispatch_sync. */
static void sync_once(struct sync_probe *p, unsigned slot, uint64_t activity, uint64_t r)
{
    struct sync_queue *q = &p->serial[(r >> 8) % SYNC_SERIALS];
    const unsigned kind = (unsigned)(r % 100);
    struct sync_call call = {p, q, NULL, activity, slot};
    if (kind >= 25 && kind < 38) {
        concurrent_once(p, &call, r >> 32);
    } else {
        if (kind < 25 && (call.inner = higher(p, q, r >> 24)))
            v80_add(&p->nested, 1);
        sync_slot_set(p, slot, SYNC_WAITING, q->id, 0);
        if (kind >= 38 && kind < 50) {
            dispatch_async_and_wait_f(q->queue, &call, sync_serial_block);
            v80_add(&p->async_and_waits, 1);
        } else {
            dispatch_sync_f(q->queue, &call, sync_serial_block);
        }
        v80_add(&q->made, 1);
    }
    sync_slot_set(p, slot, SYNC_IDLE, 0, 0);
}

static void *sync_thread(void *raw)
{
    const struct sync_arg *a = raw;
    struct sync_probe *p = a->p;
    v80_store(&p->slots[a->slot].tid, sync_self_tid());
    struct sync_activity activity;
    sync_activity_enter(p, &activity, true);
    uint64_t random = 0x9E3779B97F4A7C15ull * (a->slot + 1);
    for (uint32_t i = 0; i < p->config.syncs_per_thread; i++)
        sync_once(p, a->slot, activity.id, next_random(&random));
    sync_activity_leave(&activity);
    sync_slot_set(p, a->slot, SYNC_DONE, 0, 0);
    sync_finish(p);
    return NULL;
}

static void async_item(void *raw)
{
    struct sync_item *it = raw;
    struct sync_probe *p = it->p;
    struct sync_queue *q = it->q;
    const uint64_t me = sync_self_tid();
    check_activity(p, it->activity);
    sync_enter(p, q, me);
    q->async_ran++;
    if (it->seq + 1 <= q->last_seq[it->producer])
        v80_add(&p->out_of_order, 1);
    q->last_seq[it->producer] = it->seq + 1;
    if (v80_swap(&it->received, 1))
        v80_add(&p->duplicates, 1);
    if (it->inner) {
        struct sync_call call = {p, it->inner, NULL, it->activity, SYNC_SLOTS};
        dispatch_sync_f(it->inner->queue, &call, sync_serial_block);
        v80_add(&it->inner->made, 1);
        v80_add(&p->servicer_syncs, 1);
    }
    sync_leave(p, q, me);
    if (v80_add(&p->async_received, 1) == p->async_runs)
        sync_finish(p);
}

static void send(struct sync_item *it)
{
    v80_add(&it->q->async_sent, 1);
    dispatch_async_f(it->q->queue, it, async_item);
}

/* Numbered items onto random serial queues; a quarter of those on a workloop's queue sync from its servicer onto a
 * queue of a higher rank. */
static void *producer(void *raw)
{
    const struct sync_arg *a = raw;
    struct sync_probe *p = a->p;
    const unsigned index = a->slot - SYNC_PRODUCER_SLOT;
    const uint32_t n = p->config.items_per_producer;
    v80_store(&p->slots[a->slot].tid, sync_self_tid());
    struct sync_activity activity;
    sync_activity_enter(p, &activity, true);
    sync_slot_set(p, a->slot, SYNC_SENDING, 0, 0);
    uint64_t random = 0xD1B54A32D192ED03ull * (a->slot + 1);
    for (uint32_t seq = 0; seq < n; seq++) {
        struct sync_item *it = &p->items[index * n + seq];
        const uint64_t r = next_random(&random);
        const bool first = index == 0 && seq == 0;
        it->p = p;
        it->producer = index;
        it->seq = seq;
        it->q = &p->serial[r % SYNC_SERIALS];
        it->inner = it->q->rank < 2 && (r >> 32) % 4 == 0 ? higher(p, it->q, r >> 40) : NULL;
        it->activity = activity.id ^ (first && p->config.inject == SWITCH_SYNC_BREAK_ACTIVITY);
        send(it);
        if (first && p->config.inject == SWITCH_SYNC_BREAK_DUPLICATE)
            send(it);
    }
    sync_activity_leave(&activity);
    sync_slot_set(p, a->slot, SYNC_DONE, 0, 0);
    sync_finish(p);
    return NULL;
}

bool sync_start(struct sync_probe *p, unsigned slot, void *(*body)(void *))
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, sync_qos[slot % (sizeof(sync_qos) / sizeof(sync_qos[0]))], 0);
    p->started[slot] = pthread_create(&p->threads[slot], &attr, body, &p->args[slot]) == 0;
    pthread_attr_destroy(&attr);
    return p->started[slot];
}

static void name_queue(struct sync_queue *q, unsigned id, unsigned rank, const char *fmt, unsigned n)
{
    q->id = id;
    q->rank = rank;
    snprintf(q->label, sizeof(q->label), fmt, n);
}

static void make_queues(struct sync_probe *p)
{
    char label[96];
    for (unsigned w = 0; w < SYNC_WORKLOOPS; w++) {
        snprintf(label, sizeof(label), "com.brunostjohn.switch.hwtest.handoff.wl%u", w);
        p->workloop[w] = dispatch_workloop_create(label);
    }
    for (unsigned i = 0; i < SYNC_PLAIN_BASE; i++) {
        struct sync_queue *q = &p->serial[i];
        const unsigned w = i / SYNC_PER_WORKLOOP;
        name_queue(q, i, w, w ? "wl1.q%u" : "wl0.q%u", i % SYNC_PER_WORKLOOP);
        snprintf(label, sizeof(label), "com.brunostjohn.switch.hwtest.handoff.%s", q->label);
        q->queue = dispatch_queue_create_with_target(label, DISPATCH_QUEUE_SERIAL, p->workloop[w]);
        q->hierarchy = &p->hierarchy[w];
    }
    for (unsigned i = 0; i < SYNC_PLAIN; i++) {
        struct sync_queue *q = &p->serial[SYNC_PLAIN_BASE + i];
        name_queue(q, SYNC_PLAIN_BASE + i, SYNC_WORKLOOPS + i, "plain%u", i);
        snprintf(label, sizeof(label), "com.brunostjohn.switch.hwtest.handoff.%s", q->label);
        q->queue = dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL);
    }
    for (unsigned i = 0; i < SYNC_MOVED; i++)
        name_queue(&p->serial[SYNC_MOVED_BASE + i], SYNC_MOVED_BASE + i, (i + 1) % SYNC_WORKLOOPS, "moved%u", i);
    sync_make_moved(p);
    for (unsigned i = 0; i < SYNC_CHILDREN; i++) {
        struct sync_queue *q = &p->serial[SYNC_CHILD_BASE + i], *parent = &p->serial[SYNC_PLAIN_BASE + i];
        name_queue(q, SYNC_CHILD_BASE + i, parent->rank, "child%u", i);
        snprintf(label, sizeof(label), "com.brunostjohn.switch.hwtest.handoff.%s", q->label);
        q->queue = dispatch_queue_create_with_target(label, DISPATCH_QUEUE_SERIAL, parent->queue);
        q->hierarchy = &parent->owner;
    }
    p->concurrent.queue =
        dispatch_queue_create("com.brunostjohn.switch.hwtest.handoff.concurrent", DISPATCH_QUEUE_CONCURRENT);
    sync_make_shape(p);
}

static void release_queues(struct sync_probe *p)
{
    for (unsigned i = 0; i < SYNC_SERIALS; i++)
        dispatch_release(p->serial[i].queue);
    dispatch_release(p->concurrent.queue);
    for (unsigned w = 0; w < SYNC_WORKLOOPS; w++)
        dispatch_release(p->workloop[w]);
    sync_release_shape(p);
}

/* Whether `done` was signalled before the deadline: by the last participant, a parked thread or a caught fault. */
static bool wait_done(struct sync_probe *p, uint64_t deadline)
{
    for (;;) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now >= deadline)
            return false;
        const uint64_t left = deadline - now;
        const mach_timespec_t wait = {(unsigned int)(left / NSEC_PER_SEC), (clock_res_t)(left % NSEC_PER_SEC)};
        const kern_return_t kr = semaphore_timedwait(p->done, wait);
        if (kr != KERN_ABORTED)
            return kr == KERN_SUCCESS;
    }
}

static uint32_t clamp32(uint64_t v)
{
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

static void judge(const struct sync_probe *p, struct switch_sync_result *out)
{
    uint32_t mismatches = 0;
    uint64_t syncs = 0, sent = 0;
    for (unsigned i = 0; i < SYNC_SERIALS; i++) {
        const struct sync_queue *q = &p->serial[i];
        mismatches += (q->ran != q->made) + (q->async_ran != q->async_sent);
        syncs += q->made;
        sent += q->async_sent;
    }
    for (unsigned r = 0; r < p->config.retarget_rounds; r++) {
        const struct sync_queue *q = &p->shape.moving[r];
        mismatches += q->ran != q->made;
        syncs += q->made;
    }
    const struct sync_concurrent *cq = &p->concurrent;
    mismatches += (cq->barrier_ran != cq->barrier_made) + (cq->reader_ran != cq->reader_made);
    out->syncs = syncs + cq->barrier_made + cq->reader_made;
    out->async_sent = sent;
    out->async_received = p->async_received;
    out->counter_mismatches = mismatches;
    out->overlaps = clamp32(p->overlaps);
    out->duplicates = clamp32(p->duplicates);
    out->out_of_order = clamp32(p->out_of_order);
    out->activity_mismatches = clamp32(p->activity_mismatches);
    out->activities = clamp32(p->activities);
    out->nested = clamp32(p->nested);
    out->barriers = clamp32(p->barriers);
    out->async_and_waits = clamp32(p->async_and_waits);
    out->servicer_syncs = clamp32(p->servicer_syncs);
    out->inactive_waiters = clamp32(p->inactive_waiters);
    out->retarget_rounds = clamp32(p->retarget_rounds);
    out->retarget_parked = (uint32_t)__builtin_popcountll(p->shape.parked_rounds);
    out->retarget_crossed = (uint32_t)__builtin_popcountll(p->shape.parked_rounds & p->shape.woken_on_second);
    out->parked = clamp32(p->parked);
    out->faults = clamp32(p->faults);
    out->fault_signal = (int32_t)p->fault_signal;
    out->fault_pc = p->fault_pc;
    out->fault_tid = p->fault_tid;
    if (p->first_overlap)
        snprintf(out->first_overlap, sizeof(out->first_overlap), "%s",
                 sync_queue_label(p, (unsigned)p->first_overlap - 1));
}

static bool start_all(struct sync_probe *p)
{
    if (!sync_start_early_waiters(p))
        return false;
    sync_activate_moved(p);
    for (unsigned t = 0; t < p->config.sync_threads; t++) {
        if (!sync_start(p, t, sync_thread))
            return false;
    }
    for (unsigned t = 0; t < p->config.producers; t++) {
        if (!sync_start(p, SYNC_PRODUCER_SLOT + t, producer))
            return false;
    }
    return sync_start_shape(p);
}

static void tear_down(struct sync_probe *p)
{
    for (unsigned s = 0; s < SYNC_SLOTS; s++) {
        if (p->started[s])
            pthread_join(p->threads[s], NULL);
    }
    release_queues(p);
    semaphore_destroy(mach_task_self(), p->done);
    free(p->items);
    free(p);
}

static bool valid(const struct switch_sync_config *c)
{
    return c->sync_threads >= 1 && c->sync_threads <= SYNC_MAX_THREADS && c->producers >= 1 &&
           c->producers <= SYNC_MAX_PRODUCERS && c->items_per_producer >= 1 && c->retarget_rounds >= 1 &&
           c->retarget_rounds <= SYNC_MAX_ROUNDS && c->deadline_ms > 0;
}

bool switch_sync_handoff(const struct switch_sync_config *config, struct switch_sync_result *out)
{
    memset(out, 0, sizeof(*out));
    if (!valid(config))
        return false;
    struct sync_probe *p = calloc(1, sizeof(*p));
    const uint64_t items = (uint64_t)config->producers * config->items_per_producer;
    if (!p)
        return false;
    p->config = *config;
    p->items = calloc(items, sizeof(*p->items));
    if (!p->items || semaphore_create(mach_task_self(), &p->done, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) {
        free(p->items);
        free(p);
        return false;
    }
    p->async_runs = items + (config->inject == SWITCH_SYNC_BREAK_DUPLICATE);
    p->remaining = config->sync_threads + config->producers + SYNC_MOVED * SYNC_EARLY_WAITERS + 2 + 1;
    for (unsigned s = 0; s < SYNC_SLOTS; s++)
        p->args[s] = (struct sync_arg){p, s};
    make_queues(p);
    sync_catch_faults(p);

    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const bool started = start_all(p);
    const bool signalled = started && wait_done(p, start + (uint64_t)config->deadline_ms * NSEC_PER_MSEC);
    out->elapsed_ms = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1e6;
    const bool settled = signalled && v80_load(&p->remaining) == 0;
    sync_release_faults();
    judge(p, out);
    out->finished = settled && !out->parked && !out->faults;
    if (!out->finished)
        sync_describe_wedge(p, out->wedge, sizeof(out->wedge));
    /* Otherwise threads may still run, park or wait inside the probe's queues: it stays allocated. */
    if (out->finished)
        tear_down(p);
    return started;
}
