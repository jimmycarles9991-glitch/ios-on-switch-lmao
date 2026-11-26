/* Stand-ins for what filt_wlupdate (wlupdate.h, cut out of xnu-12377's kern_event.c by BUILD.bazel) calls: threads
 * whose references are counted, a turnstile that records wakes, and the locks it takes, checked for balance. */
#include "xnu_workloop_owner_harness.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wl_flags.h"

#define EV_ADD     0x0001
#define EV_DELETE  0x0002
#define EV_ENABLE  0x0004
#define EV_DISABLE 0x0008

#define THREAD_NULL                         ((thread_t)NULL)
#define TURNSTILE_NULL                      ((struct turnstile *)NULL)
#define THREAD_QOS_UNSPECIFIED              0
#define THREAD_QOS_USER_INTERACTIVE         6
#define MACH_PORT_NULL                      0u
#define THREAD_AWAKENED                     0
#define TURNSTILE_IMMEDIATE_UPDATE          1
#define TURNSTILE_INTERLOCK_NOT_HELD        2
#define PORT_INTRANS_THREAD_IN_CURRENT_TASK 0
#define WORKQ_TR_STATE_IDLE                 0
#define WORKQ_TR_STATE_BOUND                5
#define CAST_USER_ADDR_T(a)                 ((user_addr_t)(a))
#define OS_FALLTHROUGH                      __attribute__((fallthrough))

enum {
    KQWL_UTQ_NONE,
    KQWL_UTQ_SET_QOS_INDEX,
    KQWL_UTQ_REDRIVE_EVENTS
};

typedef uint64_t user_addr_t;
typedef uint8_t kq_index_t;
typedef uint8_t thread_qos_t;
typedef uint32_t mach_port_name_t;

struct thread {
    int refs;
    int overrides;
    bool freed_live;
    mach_port_name_t name;
};
typedef struct thread *thread_t;

struct proc;

struct turnstile {
    int ts_waitq;
};

struct workq_threadreq_s {
    int tr_state;
    kq_index_t tr_kq_qos_index;
    thread_t tr_thread;
};
typedef struct workq_threadreq_s *workq_threadreq_t;

struct kqworkloop {
    struct workq_threadreq_s kqwl_request;
    thread_t kqwl_owner;
    bool kqwl_owner_hint;
    struct turnstile *kqwl_turnstile;
    struct proc *kqwl_p;
    kq_index_t kqwl_wakeup_qos;
};

struct knote {
    uint32_t kn_sfflags;
    thread_t kn_thread;
};

struct kevent_qos_s {
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

static struct thread threads[XNU_WL_THREADS];
static struct kqworkloop workloop;
static struct turnstile turnstile;
static thread_t self;
static thread_qos_t override_qos;
static uint64_t user_word;
static int lock_depth, kq_depth, turnstile_refs, asserts_failed;
static bool woke;

#define assert(e) ((e) ? (void)0 : (void)asserts_failed++)

static thread_t thread_at(int i)
{
    return i >= 0 && i < XNU_WL_THREADS ? &threads[i] : THREAD_NULL;
}

static int index_of(thread_t t)
{
    return t ? (int)(t - threads) : XNU_WL_NONE;
}

static thread_t current_thread(void)
{
    return self;
}

static void release(thread_t t)
{
    if (--t->refs == 0)
        t->freed_live = true;
}

static void thread_deallocate_safe(thread_t t)
{
    release(t);
}

static void filt_wllock(struct kqworkloop *kqwl)
{
    (void)kqwl;
    lock_depth++;
}

static void filt_wlunlock(struct kqworkloop *kqwl)
{
    (void)kqwl;
    lock_depth--;
}

static void kqlock(struct kqworkloop *kqwl)
{
    (void)kqwl;
    kq_depth++;
}

static void kqunlock(struct kqworkloop *kqwl)
{
    (void)kqwl;
    kq_depth--;
}

static int copyin_atomic64(user_addr_t uaddr, uint64_t *out)
{
    (void)uaddr;
    *out = user_word;
    return 0;
}

/* ipc_entry.c's: the roll bits the user word's owner name lends to dispatch's flags come back as ones. */
static mach_port_name_t ipc_entry_name_mask(mach_port_name_t name)
{
    return name | 0x3u;
}

static thread_t port_name_to_thread(mach_port_name_t name, int options)
{
    (void)options;
    for (int i = 0; i < XNU_WL_THREADS; i++) {
        if (threads[i].name == name) {
            threads[i].refs++;
            return &threads[i];
        }
    }
    return THREAD_NULL;
}

static thread_t kqr_thread(workq_threadreq_t kqr)
{
    return kqr->tr_state == WORKQ_TR_STATE_BOUND ? kqr->tr_thread : THREAD_NULL;
}

static bool kqr_thread_requested_pending(workq_threadreq_t kqr)
{
    return kqr->tr_state > WORKQ_TR_STATE_IDLE && kqr->tr_state < WORKQ_TR_STATE_BOUND;
}

static bool kqr_thread_requested(workq_threadreq_t kqr)
{
    return kqr->tr_state != WORKQ_TR_STATE_IDLE;
}

static thread_qos_t kqworkloop_override(struct kqworkloop *kqwl)
{
    (void)kqwl;
    return override_qos;
}

static void thread_add_kevent_override(thread_t t, thread_qos_t qos)
{
    (void)qos;
    t->overrides++;
}

static void thread_drop_kevent_override(thread_t t)
{
    t->overrides--;
}

static void kqworkloop_update_threads_qos(struct kqworkloop *kqwl, int action, kq_index_t qos)
{
    (void)kqwl;
    (void)action;
    (void)qos;
}

static bool filt_wlturnstile_interlock_is_workq(struct kqworkloop *kqwl)
{
    (void)kqwl;
    return false;
}

static void workq_kern_threadreq_lock(struct proc *p)
{
    (void)p;
}

static void workq_kern_threadreq_unlock(struct proc *p)
{
    (void)p;
}

static void workq_kern_threadreq_update_inheritor(struct proc *p, workq_threadreq_t kqr, thread_t owner,
                                                  struct turnstile *ts, int flags)
{
    (void)p;
    (void)kqr;
    (void)owner;
    (void)ts;
    (void)flags;
}

static void filt_wlupdate_inheritor(struct kqworkloop *kqwl, struct turnstile *ts, int flags)
{
    (void)kqwl;
    (void)ts;
    (void)flags;
}

static void turnstile_reference(struct turnstile *ts)
{
    (void)ts;
    turnstile_refs++;
}

static void turnstile_deallocate(struct turnstile *ts)
{
    (void)ts;
    turnstile_refs--;
}

static void turnstile_update_inheritor_complete(struct turnstile *ts, int flags)
{
    (void)ts;
    (void)flags;
}

static uint64_t knote_filt_wev64(struct knote *kn)
{
    return (uint64_t)(uintptr_t)kn;
}

static void waitq_wakeup64_thread(int *waitq, uint64_t event, thread_t t, int result)
{
    (void)waitq;
    (void)event;
    (void)t;
    (void)result;
    woke = true;
}

static void disable_preemption(void)
{}

#include "wlupdate.h"

void xnu_wl_reset(int owner, int servicer, bool override)
{
    for (int i = 0; i < XNU_WL_THREADS; i++)
        threads[i] = (struct thread){.refs = 1, .name = 0x1003u + 0x100u * (mach_port_name_t)i};
    workloop = (struct kqworkloop){.kqwl_turnstile = &turnstile};
    override_qos = override ? THREAD_QOS_USER_INTERACTIVE : THREAD_QOS_UNSPECIFIED;
    if ((workloop.kqwl_owner = thread_at(owner))) {
        workloop.kqwl_owner->refs++;
        if (override)
            workloop.kqwl_owner->overrides++;
    }
    if ((workloop.kqwl_request.tr_thread = thread_at(servicer)))
        workloop.kqwl_request.tr_state = WORKQ_TR_STATE_BOUND;
    lock_depth = kq_depth = turnstile_refs = asserts_failed = 0;
}

struct xnu_wl_result xnu_wl_update(const struct xnu_wl_kevent *k)
{
    const uint64_t word = k->named == XNU_WL_NONE ? 0 : threads[k->named].name;
    struct knote kn = {.kn_sfflags = k->sfflags, .kn_thread = thread_at(k->waiter)};
    struct kevent_qos_s kev = {
        .flags = k->flags,
        .fflags = k->fflags,
        .ext = {[EV_EXTIDX_WL_ADDR] = 0x16f000000, [EV_EXTIDX_WL_MASK] = ~0ull, [EV_EXTIDX_WL_VALUE] = word ^ k->stale},
    };
    user_word = word;
    self = thread_at(k->self);
    woke = false;
    if (kn.kn_thread)
        kn.kn_thread->refs++;
    const int error = filt_wlupdate(&workloop, &kn, &kev, 0, k->op);
    return (struct xnu_wl_result){.error = error, .woke = woke, .sfflags = kn.kn_sfflags};
}

bool xnu_wl_detach(int waiter)
{
    thread_t t = thread_at(waiter);
    release(t);
    return !t->freed_live;
}

int xnu_wl_owner(void)
{
    return index_of(workloop.kqwl_owner);
}

bool xnu_wl_owner_hint(void)
{
    return workloop.kqwl_owner_hint;
}

int xnu_wl_refs(int thread)
{
    return threads[thread].refs;
}

int xnu_wl_overrides(int thread)
{
    return threads[thread].overrides;
}

bool xnu_wl_locks_balanced(void)
{
    return lock_depth == 0 && kq_depth == 0 && turnstile_refs == 0 && asserts_failed == 0;
}

int xnu_wl_no_attach(void)
{
    return EWLNOATTACH;
}

int xnu_wl_preempt_disabled(void)
{
    return EPREEMPTDISABLED;
}
