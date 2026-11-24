// SPDX-License-Identifier: GPL-2.0-only
/*
 * Debug kernels: one line for a boot function's steps before launchd, each with the milliseconds since the step before
 * it (mach time), where the log has no other line to split the stretch by. Release kernels compile it to nothing.
 */
#ifndef SWITCH_BOOT_STEPS_H
#define SWITCH_BOOT_STEPS_H

#include <stddef.h>
#include <stdint.h>

#if DEVELOPMENT || DEBUG

#include <kern/clock.h>
#include <libkern/libkern.h>

struct switch_boot_steps {
    uint64_t last;
    size_t at;
    char text[384];
};

static inline void switch_boot_steps_begin(struct switch_boot_steps *s)
{
    s->last = mach_absolute_time();
    s->at = 0;
    s->text[0] = '\0';
}

static inline void switch_boot_steps_mark(struct switch_boot_steps *s, const char *step)
{
    const uint64_t now = mach_absolute_time();
    uint64_t ns = 0;
    int n;

    absolutetime_to_nanoseconds(now - s->last, &ns);
    s->last = now;
    if (s->at >= sizeof(s->text)) {
        return;
    }
    n = snprintf(s->text + s->at, sizeof(s->text) - s->at, "%s%s %llu", s->at ? ", " : "", step,
                 (unsigned long long)(ns / 1000000ull));
    if (n > 0) {
        s->at += (size_t)n;
    }
}

/* "SwitchBootSteps: <what> (ms): <step> <ms>, ..." */
static inline void switch_boot_steps_log(const struct switch_boot_steps *s, const char *what)
{
    printf("SwitchBootSteps: %s (ms): %s\n", what, s->text);
}

#else /* DEVELOPMENT || DEBUG */

struct switch_boot_steps {
    char unused;
};

static inline void switch_boot_steps_begin(struct switch_boot_steps *s)
{
    (void)s;
}

static inline void switch_boot_steps_mark(struct switch_boot_steps *s, const char *step)
{
    (void)s;
    (void)step;
}

static inline void switch_boot_steps_log(const struct switch_boot_steps *s, const char *what)
{
    (void)s;
    (void)what;
}

#endif /* DEVELOPMENT || DEBUG */

#endif /* SWITCH_BOOT_STEPS_H */
