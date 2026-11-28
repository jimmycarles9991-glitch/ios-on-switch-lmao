// SPDX-License-Identifier: GPL-2.0-only
/*
 * User threads' kernel time by what brought each thread into the kernel (systime.c, debug kernels). sleh_synchronous
 * marks a thread's synchronous exception from EL0 in thread->machine.exception_trace_code (ARM64_KDBG_CODE_USER | the
 * ESR class); recount hands every slice of kernel-level time to the thread it belongs to. sleh clears the mark before
 * the trap's last slice is handed over (recount_enter_user, on the way back to EL0), so the bucket is kept in the
 * thread's machine.switch_systime_trap from the mark until recount_enter_user has handed that slice over. A slice with
 * no trap kept (an interrupt or an AST taken from EL0) is "outside a trap". Plain C, free of kernel types, so
 * kernel/tests runs it on the host; the including file declares snprintf. Times are mach ticks.
 */
#ifndef SWITCH_SCHED_SYSTIME_H
#define SWITCH_SCHED_SYSTIME_H

#include <stddef.h>
#include <stdint.h>

enum switch_systime_bucket {
    SWITCH_SYSTIME_UNIX,
    SWITCH_SYSTIME_MACH_MSG,
    SWITCH_SYSTIME_MACH,
    SWITCH_SYSTIME_DATA_ABORT,
    SWITCH_SYSTIME_INSTRUCTION_ABORT,
    SWITCH_SYSTIME_UNDEFINED,
    SWITCH_SYSTIME_OTHER,
    SWITCH_SYSTIME_OUTSIDE,
    SWITCH_SYSTIME_BUCKETS
};

/* ESR_ELx.EC values (osfmk/arm64/proc_reg.h esr_exception_class_t). */
#define SWITCH_SYSTIME_EC_UNCATEGORIZED 0x00u
#define SWITCH_SYSTIME_EC_SVC_64        0x15u
#define SWITCH_SYSTIME_EC_IABORT_EL0    0x20u
#define SWITCH_SYSTIME_EC_DABORT_EL0    0x24u

/* Mach trap numbers are negative in x16: mach_msg_trap -31, mach_msg_overwrite_trap -32, mach_msg2_trap -47. */
static inline enum switch_systime_bucket switch_systime_bucket(uint32_t ec, int64_t svc)
{
    switch (ec) {
    case SWITCH_SYSTIME_EC_SVC_64:
        if (svc >= 0) {
            return SWITCH_SYSTIME_UNIX;
        }
        return svc == -31 || svc == -32 || svc == -47 ? SWITCH_SYSTIME_MACH_MSG : SWITCH_SYSTIME_MACH;
    case SWITCH_SYSTIME_EC_DABORT_EL0:
        return SWITCH_SYSTIME_DATA_ABORT;
    case SWITCH_SYSTIME_EC_IABORT_EL0:
        return SWITCH_SYSTIME_INSTRUCTION_ABORT;
    case SWITCH_SYSTIME_EC_UNCATEGORIZED:
        return SWITCH_SYSTIME_UNDEFINED;
    default:
        return SWITCH_SYSTIME_OTHER;
    }
}

/* sleh.c's ARM64_KDBG_CODE_USER, set in exception_trace_code beside the ESR class while a user exception runs. */
#define SWITCH_SYSTIME_TRACE_USER 0x100u
#define SWITCH_SYSTIME_TRACE_EC   0xffu

static inline enum switch_systime_bucket switch_systime_trace_bucket(uint16_t trace_code, int64_t svc)
{
    if ((trace_code & SWITCH_SYSTIME_TRACE_USER) == 0) {
        return SWITCH_SYSTIME_OUTSIDE;
    }
    return switch_systime_bucket(trace_code & SWITCH_SYSTIME_TRACE_EC, svc);
}

struct switch_systime {
    uint64_t time[SWITCH_SYSTIME_BUCKETS];
    uint64_t entries[SWITCH_SYSTIME_BUCKETS];
};

static inline size_t switch_systime_put(size_t size, size_t at, int n)
{
    if (n < 0 || at >= size) {
        return at;
    }
    return at + (size_t)n < size ? at + (size_t)n : size - 1;
}

/* "unix 3200 ms (412000), mach_msg 2100 ms (95000), ..., outside a trap 1800 ms" for one window's differences. */
static inline size_t switch_systime_format(char *out, size_t size, const struct switch_systime *w, uint32_t numer,
                                           uint32_t denom)
{
    static const char *const names[SWITCH_SYSTIME_BUCKETS] = {
        "unix",      "mach_msg",   "other mach",     "data abort", "instruction abort",
        "undefined", "other sync", "outside a trap",
    };
    size_t at = 0;

    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    for (uint32_t b = 0; b < SWITCH_SYSTIME_BUCKETS; b++) {
        const unsigned long long ms = (unsigned long long)(w->time[b] * numer / denom / 1000000u);

        if (b == SWITCH_SYSTIME_OUTSIDE) {
            at = switch_systime_put(size, at,
                                    snprintf(out + at, size - at, "%s%s %llu ms", b ? ", " : "", names[b], ms));
        } else {
            at = switch_systime_put(size, at,
                                    snprintf(out + at, size - at, "%s%s %llu ms (%llu)", b ? ", " : "", names[b], ms,
                                             (unsigned long long)w->entries[b]));
        }
    }
    return at;
}

#endif /* SWITCH_SCHED_SYSTIME_H */
