// xnu_workloop_owner_harness.c: xnu-12377's filt_wlupdate, cut out of kern_event.c and compiled as the kernel compiles
// it (C) over a workloop, a knote and four threads that count their references, for xnu_workloop_owner_test.cpp.
#ifndef SWITCH_XNU_WORKLOOP_OWNER_HARNESS_H
#define SWITCH_XNU_WORKLOOP_OWNER_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XNU_WL_THREADS 4
#define XNU_WL_NONE    (-1)

// One kevent: op is kern_event.c's FILT_WLATTACH (0), FILT_WLTOUCH (1) or FILT_WLDROP (2). `named` is the thread whose
// port name the user word holds (XNU_WL_NONE: MACH_PORT_NULL); a stale word differs from the kevent's value. The
// knote's waiter (kn_thread) holds a reference of its own while the kevent runs.
struct xnu_wl_kevent {
    int op;
    uint32_t fflags;
    uint16_t flags;
    int named;
    bool stale;
    int self;
    int waiter;
    uint32_t sfflags;
};

struct xnu_wl_result {
    int error;
    bool woke;
    uint32_t sfflags;
};

// A workloop whose owner and bound servicer are these threads (or XNU_WL_NONE); each thread holds its own reference
// and the owner one more, plus a kevent override when `override` is set.
void xnu_wl_reset(int owner, int servicer, bool override);
struct xnu_wl_result xnu_wl_update(const struct xnu_wl_kevent *k);
// The knote's drop after the kevent (filt_wldetach): its waiter's reference goes. False when that freed a thread
// that was never terminated (thread.c's "thread not properly terminated" panic).
bool xnu_wl_detach(int waiter);

int xnu_wl_owner(void);
bool xnu_wl_owner_hint(void);
int xnu_wl_refs(int thread);
int xnu_wl_overrides(int thread);
bool xnu_wl_locks_balanced(void);
int xnu_wl_no_attach(void);
int xnu_wl_preempt_disabled(void);

#ifdef __cplusplus
}
#endif

#endif
