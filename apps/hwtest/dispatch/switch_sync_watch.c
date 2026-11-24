/* What an unfinished run leaves to read, and the signals caught while it runs. */
#include "switch_sync_internal.h"

#include <dlfcn.h>
#include <mach-o/getsect.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

const char *sync_queue_label(const struct sync_probe *p, unsigned id)
{
    if (id < SYNC_SERIALS)
        return p->serial[id].label;
    if (id == SYNC_CONCURRENT_ID)
        return "concurrent";
    if (id >= SYNC_MOVING_ID && id < SYNC_QUEUE_IDS)
        return p->shape.moving[id - SYNC_MOVING_ID].label;
    return "?";
}

static size_t append(char *out, size_t size, size_t at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static size_t append(char *out, size_t size, size_t at, const char *fmt, ...)
{
    if (at + 1 >= size)
        return at;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + at, size - at, fmt, ap);
    va_end(ap);
    return n < 0 ? at : (at + (size_t)n < size ? at + (size_t)n : size - 1);
}

static size_t threads_at(const struct sync_probe *p, char *out, size_t size, size_t at, unsigned id, bool inside)
{
    const char *sep = inside ? " in " : " wait ";
    for (unsigned s = 0; s < SYNC_SLOTS; s++) {
        const uint64_t state = v80_load(&p->slots[s].state);
        const unsigned phase = (unsigned)(state >> 16), queue = (unsigned)(state >> 8) & 0xff,
                       inner = (unsigned)state & 0xff;
        const bool here = inside ? (phase == SYNC_INSIDE || phase == SYNC_NESTED) && queue == id
                                 : (phase == SYNC_WAITING && queue == id) || (phase == SYNC_NESTED && inner == id);
        if (here) {
            at = append(out, size, at, "%s0x%llx", sep, p->slots[s].tid);
            sep = ",";
        }
    }
    return at;
}

static bool held(const struct sync_probe *p, unsigned id, uint64_t *owner, uint64_t *left)
{
    *owner = *left = 0;
    if (id < SYNC_SERIALS) {
        *owner = v80_load(&p->serial[id].owner);
        *left = p->serial[id].async_sent - p->serial[id].async_ran;
    } else if (id >= SYNC_MOVING_ID && id < SYNC_QUEUE_IDS) {
        *owner = v80_load(&p->shape.moving[id - SYNC_MOVING_ID].owner);
    }
    for (unsigned s = 0; s < SYNC_SLOTS; s++) {
        const uint64_t state = v80_load(&p->slots[s].state);
        const unsigned phase = (unsigned)(state >> 16);
        if ((phase == SYNC_WAITING || phase == SYNC_INSIDE || phase == SYNC_NESTED) &&
            (((state >> 8) & 0xff) == id || (phase == SYNC_NESTED && (state & 0xff) == id)))
            return true;
    }
    return *owner || *left;
}

void sync_describe_wedge(const struct sync_probe *p, char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    for (unsigned id = 0; id < SYNC_QUEUE_IDS; id++) {
        uint64_t owner, left;
        if (!held(p, id, &owner, &left))
            continue;
        at = append(out, size, at, "%s%s:", at ? "; " : "", sync_queue_label(p, id));
        if (owner)
            at = append(out, size, at, " owner 0x%llx", owner);
        at = threads_at(p, out, size, at, id, true);
        at = threads_at(p, out, size, at, id, false);
        if (left)
            at = append(out, size, at, " items_left %llu", left);
    }
    const uint64_t shaping = v80_load(&p->slots[SYNC_SHAPER_SLOT].state);
    if (shaping >> 16 == SYNC_SHAPING)
        at = append(out, size, at, "%sshape %s step %llu", at ? "; " : "", sync_queue_label(p, (shaping >> 8) & 0xff),
                    shaping & 0xff);
    if (p->parked)
        at = append(out, size, at, "%sparked %llu", at ? "; " : "", p->parked);
    if (p->faults)
        append(out, size, at, "%sfault sig %llu pc 0x%llx tid 0x%llx", at ? "; " : "", p->fault_signal, p->fault_pc,
               p->fault_tid);
}

static struct sync_probe *catching;
static uintptr_t dispatch_text, dispatch_text_end;
static const int caught_signals[] = {SIGSEGV, SIGBUS, SIGTRAP, SIGILL};
static struct sigaction previous[sizeof(caught_signals) / sizeof(caught_signals[0])];

static void restore(int sig)
{
    for (size_t i = 0; i < sizeof(caught_signals) / sizeof(caught_signals[0]); i++) {
        if (caught_signals[i] == sig)
            sigaction(sig, &previous[i], NULL);
    }
}

static bool ours(const struct sync_probe *p, uint64_t tid)
{
    for (unsigned s = 0; s < SYNC_SLOTS; s++) {
        if (v80_load(&p->slots[s].tid) == tid)
            return true;
    }
    return false;
}

/* A fault in libdispatch on a thread other than the main one, or anywhere on one of the probe's threads, is the bug
 * the probe looks for: the thread is recorded and parked, so the app lives to report it. Anything else goes back to
 * the handler that was there before, and the faulting instruction runs again into it. */
static void caught(int sig, siginfo_t *info, void *context)
{
    (void)info;
    struct sync_probe *p = catching;
    const ucontext_t *uc = context;
    const uint64_t pc = (uint64_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    const bool in_dispatch = pc >= dispatch_text && pc < dispatch_text_end && !pthread_main_np();
    if (!p || !(in_dispatch || ours(p, tid))) {
        restore(sig);
        return;
    }
    if (v80_add(&p->faults, 1) == 1) {
        p->fault_signal = (uint64_t)sig;
        p->fault_pc = pc;
        p->fault_tid = tid;
    }
    semaphore_signal(p->done);
    for (;;)
        pause();
}

static void find_dispatch_text(void)
{
    const void *fn = (const void *)dispatch_sync_f;
#if __has_feature(ptrauth_calls)
    fn = ptrauth_strip(fn, ptrauth_key_function_pointer);
#endif
    Dl_info info;
    unsigned long size = 0;
    if (!dladdr(fn, &info))
        return;
    const uint8_t *text = getsectiondata((const struct mach_header_64 *)info.dli_fbase, "__TEXT", "__text", &size);
    if (text) {
        dispatch_text = (uintptr_t)text;
        dispatch_text_end = dispatch_text + size;
    }
}

void sync_catch_faults(struct sync_probe *p)
{
    find_dispatch_text();
    catching = p;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = caught;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof(caught_signals) / sizeof(caught_signals[0]); i++)
        sigaction(caught_signals[i], &action, &previous[i]);
}

void sync_release_faults(void)
{
    for (size_t i = 0; i < sizeof(caught_signals) / sizeof(caught_signals[0]); i++)
        sigaction(caught_signals[i], &previous[i], NULL);
    catching = NULL;
}
