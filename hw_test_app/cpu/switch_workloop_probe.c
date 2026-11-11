#include "switch_workloop_probe.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/event.h>
#include <time.h>

/* xnu bsd/sys/event_private.h: not in the SDK, the same on xnu-10063 and xnu-12377. */
#define SWITCH_EVFILT_WORKLOOP        (-17)
#define SWITCH_KEVENT_FLAG_WORKLOOP   0x400u
#define SWITCH_NOTE_WL_SYNC_WAIT      0x4u
#define SWITCH_NOTE_WL_SYNC_WAKE      0x8u
#define SWITCH_NOTE_WL_END_OWNERSHIP  0x20u
#define SWITCH_NOTE_WL_DISCOVER_OWNER 0x80u
#define SWITCH_EV_EXTIDX_WL_ADDR      1
#define SWITCH_EV_EXTIDX_WL_VALUE     3
#define SWITCH_SYS_KEVENT_ID          375
/* QOS_CLASS_DEFAULT as a pthread_priority_t: libdispatch's fallback for a sync waiter's qos. */
#define SWITCH_PRIORITY_DEFAULT 0x8ff

#define SWITCH_WORKLOOP_MAX_WAITERS 16u
#define SWITCH_WORKLOOP_DEADLINE_NS 10000000000ull
/* A SYNC_WAIT this long was a park in the kernel, not a wake that was already posted. */
#define SWITCH_WORKLOOP_PARKED_NS 20000ull

struct switch_kevent_qos {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    int32_t qos;
    uint64_t udata;
    uint32_t fflags;
    uint32_t xflags;
    int64_t data;
    uint64_t ext[4];
};

struct waiter_account {
    uint32_t early, parked, interrupted, errors;
    int32_t first_error;
};

struct probe {
    /* The workloop's id is this word's address. Its low 32 bits name the thread that owns the workloop. */
    uint64_t state;
    uint32_t rounds;
    uint32_t abort;
    bool keep_knotes;
    uint32_t port[SWITCH_WORKLOOP_MAX_WAITERS];
    uint32_t ready[SWITCH_WORKLOOP_MAX_WAITERS];   /* the round a waiter is about to wait for */
    uint32_t granted[SWITCH_WORKLOOP_MAX_WAITERS]; /* the last round the waker handed it */
    uint32_t done[SWITCH_WORKLOOP_MAX_WAITERS];    /* the last round it gave back */
    struct waiter_account account[SWITCH_WORKLOOP_MAX_WAITERS];
};

struct waiter_arg {
    struct probe *probe;
    unsigned index;
};

static uint32_t load_acquire(const uint32_t *p)
{
    uint32_t v;
    __asm__ volatile("ldar %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static void store_release(uint32_t *p, uint32_t v)
{
    __asm__ volatile("stlr %w0, [%1]" : : "r"(v), "r"(p) : "memory");
}

static uint64_t load_acquire64(const uint64_t *p)
{
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static void store_release64(uint64_t *p, uint64_t v)
{
    __asm__ volatile("stlr %0, [%1]" : : "r"(v), "r"(p) : "memory");
}

/* kevent_id(2) as a trap of its own: the number of events (errors, here) or -errno. */
static int kevent_id_trap(uint64_t id, struct switch_kevent_qos *ke, uint32_t flags)
{
    register uint64_t x0 __asm__("x0") = id;
    register uint64_t x1 __asm__("x1") = (uint64_t)ke;
    register uint64_t x2 __asm__("x2") = 1;
    register uint64_t x3 __asm__("x3") = (uint64_t)ke;
    register uint64_t x4 __asm__("x4") = 1;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    register uint64_t x7 __asm__("x7") = flags;
    register uint64_t x16 __asm__("x16") = SWITCH_SYS_KEVENT_ID;
    uint64_t failed;
    __asm__ volatile("svc #0x80\n\tcset %[failed], cs"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7),
                       "+r"(x16), [failed] "=&r"(failed)
                     :
                     : "memory", "cc", "x17");
    return failed ? -(int)x0 : (int)x0;
}

/* One change to the probe's workloop; 0, or the errno it failed with. */
static int workloop_change(struct probe *p, uint32_t ident, uint16_t flags, uint32_t fflags, bool discover)
{
    struct switch_kevent_qos ke = {
        .ident = ident,
        .filter = SWITCH_EVFILT_WORKLOOP,
        .flags = flags,
        .qos = SWITCH_PRIORITY_DEFAULT,
        .udata = (uint64_t)&p->state,
        .fflags = fflags,
    };
    ke.ext[SWITCH_EV_EXTIDX_WL_VALUE] = load_acquire64(&p->state);
    if (discover)
        ke.ext[SWITCH_EV_EXTIDX_WL_ADDR] = (uint64_t)&p->state;
    const int n = kevent_id_trap((uint64_t)&p->state, &ke,
                                 SWITCH_KEVENT_FLAG_WORKLOOP | KEVENT_FLAG_IMMEDIATE | KEVENT_FLAG_ERROR_EVENTS);
    if (n < 0)
        return -n;
    return n ? (int)ke.data : 0;
}

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void note_error(struct waiter_account *a, int error)
{
    if (!a->errors++)
        a->first_error = error;
}

static void *waiter(void *raw)
{
    const struct waiter_arg *arg = raw;
    struct probe *p = arg->probe;
    const unsigned i = arg->index;
    struct waiter_account *a = &p->account[i];
    const uint32_t me = pthread_mach_thread_np(pthread_self());

    store_release(&p->port[i], me);
    for (uint32_t round = 1; round <= p->rounds && !load_acquire(&p->abort); round++) {
        store_release(&p->ready[i], round);
        const uint64_t start = now_ns();
        int error;
        while ((error = workloop_change(p, me, EV_ADD | EV_DISABLE, SWITCH_NOTE_WL_SYNC_WAIT, false)) == EINTR)
            a->interrupted++;
        if (error) {
            note_error(a, error);
            break;
        }
        if (now_ns() - start >= SWITCH_WORKLOOP_PARKED_NS)
            a->parked++;
        if (load_acquire(&p->granted[i]) != round && !load_acquire(&p->abort))
            a->early++;
        if (!p->keep_knotes) {
            error = workloop_change(p, me, EV_DELETE | EV_ENABLE,
                                    SWITCH_NOTE_WL_SYNC_WAKE | SWITCH_NOTE_WL_END_OWNERSHIP, false);
            if (error)
                note_error(a, error);
        }
        store_release(&p->done[i], round);
    }
    /* A waiter that stops early must not leave the waker waiting for it. */
    store_release(&p->ready[i], UINT32_MAX);
    store_release(&p->done[i], UINT32_MAX);
    return NULL;
}

/* Yields until *word reaches `value`; false past the deadline. */
static bool wait_for(const uint32_t *word, uint32_t value)
{
    const uint64_t deadline = now_ns() + SWITCH_WORKLOOP_DEADLINE_NS;
    while (load_acquire(word) < value) {
        if (now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}

/* Every waiter that is still parked is let go, its handoff marked as granted so it does not count as early. */
static void release_all(struct probe *p, unsigned waiters, struct switch_workloop_result *out)
{
    store_release(&p->abort, 1);
    for (unsigned i = 0; i < waiters; i++) {
        store_release(&p->granted[i], load_acquire(&p->ready[i]));
        const int error =
            workloop_change(p, load_acquire(&p->port[i]), EV_ADD | EV_DISABLE, SWITCH_NOTE_WL_SYNC_WAKE, false);
        if (error && !out->errors++)
            out->first_error = error;
    }
}

/* The waker's side of one generation: every round, each waiter in turn (a rotating order) gets the workloop and gives
 * it back. */
static void hand_out(struct probe *p, unsigned waiters, uint32_t me, struct switch_workloop_result *out)
{
    for (uint32_t round = 1; round <= p->rounds; round++) {
        for (unsigned i = 0; i < waiters; i++) {
            if (!wait_for(&p->ready[i], round)) {
                out->stuck++;
                return;
            }
        }
        for (unsigned k = 0; k < waiters; k++) {
            const unsigned i = (k + round) % waiters;
            if (load_acquire(&p->done[i]) == round)
                out->early++;
            store_release64(&p->state, load_acquire(&p->port[i]));
            store_release(&p->granted[i], round);
            const int error = workloop_change(p, load_acquire(&p->port[i]), EV_ADD | EV_DISABLE,
                                              SWITCH_NOTE_WL_SYNC_WAKE | SWITCH_NOTE_WL_DISCOVER_OWNER, true);
            out->handoffs++;
            if (error && !out->errors++)
                out->first_error = error;
            if (!wait_for(&p->done[i], round)) {
                out->stuck++;
                return;
            }
            store_release64(&p->state, me);
        }
    }
}

static bool generation(unsigned waiters, unsigned rounds, bool keep_knotes, uint32_t me,
                       struct switch_workloop_result *out)
{
    struct probe *p = calloc(1, sizeof(*p));
    if (!p)
        return false;
    p->rounds = rounds;
    p->keep_knotes = keep_knotes;
    store_release64(&p->state, me);

    pthread_t threads[SWITCH_WORKLOOP_MAX_WAITERS];
    struct waiter_arg args[SWITCH_WORKLOOP_MAX_WAITERS];
    unsigned started = 0;
    for (; started < waiters; started++) {
        args[started] = (struct waiter_arg){.probe = p, .index = started};
        if (pthread_create(&threads[started], NULL, waiter, &args[started]))
            break;
    }
    for (unsigned i = 0; i < started; i++) {
        if (!wait_for(&p->ready[i], 1))
            out->stuck++;
    }
    if (started == waiters && !out->stuck)
        hand_out(p, waiters, me, out);
    if (started < waiters || out->stuck)
        release_all(p, started, out);
    for (unsigned i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    for (unsigned i = 0; i < started; i++) {
        const struct waiter_account *a = &p->account[i];
        out->early += a->early;
        out->parked += a->parked;
        out->interrupted += a->interrupted;
        if (a->errors && !out->errors)
            out->first_error = a->first_error;
        out->errors += a->errors;
    }
    free(p);
    return started == waiters;
}

bool switch_workloop_probe(unsigned generations, unsigned waiters, unsigned rounds, bool keep_knotes,
                           struct switch_workloop_result *out)
{
    *out = (struct switch_workloop_result){0};
    if (waiters == 0 || waiters > SWITCH_WORKLOOP_MAX_WAITERS || rounds == 0)
        return false;
    const uint32_t me = pthread_mach_thread_np(pthread_self());
    for (unsigned g = 0; g < generations && !out->stuck; g++) {
        if (!generation(waiters, rounds, keep_knotes, me, out))
            return false;
    }
    return true;
}
