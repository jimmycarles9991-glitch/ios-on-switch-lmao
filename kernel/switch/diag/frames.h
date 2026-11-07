// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch frame diagnostics (frames.c, debug kernels): what reaches the render server between two presents. This half is
 * plain data and text, free of kernel types, so kernel/tests runs it on the host; the including file declares snprintf.
 *
 * Commit cadence. Core Animation's client commits a transaction as one mach message per context it changed, sent back to
 * back, so a client's frame is a burst of messages to the render server: a gap of more than SWITCH_FRAMES_BURST_US
 * starts the next one. The gaps between a client's bursts, in refresh periods, say whether it drew every frame.
 *
 * Touch delivery. backboardd routes each touch report to the app under the finger on its event threads (fixed
 * priority 63: its "event router thread" and IOHIDService's compatibility thread). The time from a report's interrupt
 * to backboardd's next message to another process, from such a thread, is the report's delivery.
 */
#ifndef SWITCH_DIAG_FRAMES_H
#define SWITCH_DIAG_FRAMES_H

#include <stddef.h>
#include <stdint.h>

#include "../sched/stats.h"

#define SWITCH_FRAMES_CLIENTS   8
#define SWITCH_FRAMES_NAME      17
#define SWITCH_FRAMES_BINS      5 /* burst-to-burst gaps in refresh periods: under one, 1, 2, 3, 4 or more */
#define SWITCH_FRAMES_BURST_US  2000
#define SWITCH_FRAMES_STILL_US  250000 /* a longer gap is a client that went still, not a late one */
#define SWITCH_FRAMES_EVENT_PRI 60

struct switch_frames_client {
    int32_t pid;
    char name[SWITCH_FRAMES_NAME];
    uint64_t last_us;  /* its last message */
    uint64_t start_us; /* the first message of its last burst */
    uint32_t commits;
    uint32_t gaps[SWITCH_FRAMES_BINS];
};

struct switch_frames_cadence {
    struct switch_frames_client client[SWITCH_FRAMES_CLIENTS];
    uint32_t used;
};

struct switch_frames_delivery {
    uint64_t pending_us; /* the oldest report no delivery followed yet; 0: none */
    uint32_t reports;
    struct switch_sched_window latency; /* report to delivery, us */
    int32_t to_pid;
    char to[SWITCH_FRAMES_NAME]; /* the last delivery's receiver */
};

static inline unsigned int switch_frames_bin(uint64_t gap_us, uint64_t period_us)
{
    uint64_t periods = period_us ? (gap_us + period_us / 2) / period_us : 0;

    return periods >= SWITCH_FRAMES_BINS - 1 ? SWITCH_FRAMES_BINS - 1 : (unsigned int)periods;
}

/* A new client's entry: a free one, else the one whose last message is the oldest (a client gone idle). */
static inline struct switch_frames_client *switch_frames_new_client(struct switch_frames_cadence *c)
{
    uint32_t oldest = 0;

    if (c->used < SWITCH_FRAMES_CLIENTS) {
        return &c->client[c->used++];
    }
    for (uint32_t i = 1; i < SWITCH_FRAMES_CLIENTS; i++) {
        if (c->client[i].last_us < c->client[oldest].last_us) {
            oldest = i;
        }
    }
    struct switch_frames_client *e = &c->client[oldest];
    e->last_us = e->start_us = 0;
    e->commits = 0;
    for (unsigned int b = 0; b < SWITCH_FRAMES_BINS; b++) {
        e->gaps[b] = 0;
    }
    return e;
}

/* A message from pid at now_us (microseconds of mach time). */
static inline void switch_frames_note_message(struct switch_frames_cadence *c, int32_t pid, const char *name,
                                              uint64_t now_us, uint64_t period_us)
{
    struct switch_frames_client *e = NULL;
    uint32_t i;

    for (i = 0; i < c->used; i++) {
        if (c->client[i].pid == pid) {
            e = &c->client[i];
            break;
        }
    }
    if (e == NULL) {
        e = switch_frames_new_client(c);
        e->pid = pid;
        for (i = 0; i + 1 < SWITCH_FRAMES_NAME && name && name[i]; i++) {
            e->name[i] = name[i];
        }
        e->name[i] = 0;
    } else if (now_us - e->last_us <= SWITCH_FRAMES_BURST_US) {
        e->last_us = now_us;
        return;
    }
    if (e->start_us != 0 && now_us - e->start_us < SWITCH_FRAMES_STILL_US) {
        e->gaps[switch_frames_bin(now_us - e->start_us, period_us)]++;
    }
    e->commits++;
    e->start_us = e->last_us = now_us;
}

/* A new window: the counts start over, each client's last burst stays so the next gap is measured across the edge. */
static inline void switch_frames_cadence_restart(struct switch_frames_cadence *c)
{
    for (uint32_t i = 0; i < c->used; i++) {
        c->client[i].commits = 0;
        for (unsigned int b = 0; b < SWITCH_FRAMES_BINS; b++) {
            c->client[i].gaps[b] = 0;
        }
    }
}

static inline void switch_frames_note_report(struct switch_frames_delivery *d, uint64_t stamp_us)
{
    d->reports++;
    if (d->pending_us == 0) {
        d->pending_us = stamp_us;
    }
}

static inline void switch_frames_note_delivery(struct switch_frames_delivery *d, int32_t to_pid, const char *to,
                                               uint64_t now_us)
{
    uint32_t i;

    if (d->pending_us == 0 || now_us < d->pending_us) {
        return;
    }
    switch_sched_note(&d->latency, now_us - d->pending_us);
    d->pending_us = 0;
    d->to_pid = to_pid;
    for (i = 0; i + 1 < SWITCH_FRAMES_NAME && to && to[i]; i++) {
        d->to[i] = to[i];
    }
    d->to[i] = 0;
}

static inline size_t switch_frames_advance(size_t at, int n, size_t size)
{
    return n < 0 ? at : (at + (size_t)n < size ? at + (size_t)n : size - 1);
}

/*
 * The `top` clients by commits, most first: "SpringBoard[27] 118 commits <1:2 1:101 2:11 3:3 4+:0", then the
 * delivery: "; delivery 212/215 reports p50/p99/max 128/512/950 us to SpringBoard[27]". Returns the length written.
 */
static inline size_t switch_frames_format(const struct switch_frames_cadence *c, const struct switch_frames_delivery *d,
                                          unsigned int top, char *out, size_t size)
{
    uint8_t shown[SWITCH_FRAMES_CLIENTS] = {0};
    size_t at = 0;
    int n;

    if (size == 0) {
        return 0;
    }
    out[0] = 0;
    for (unsigned int k = 0; k < top; k++) {
        uint32_t best = SWITCH_FRAMES_CLIENTS;
        for (uint32_t i = 0; i < c->used; i++) {
            if (!shown[i] && c->client[i].commits &&
                (best == SWITCH_FRAMES_CLIENTS || c->client[i].commits > c->client[best].commits)) {
                best = i;
            }
        }
        if (best == SWITCH_FRAMES_CLIENTS) {
            break;
        }
        shown[best] = 1;
        const struct switch_frames_client *e = &c->client[best];
        n = snprintf(out + at, size - at, "%s%s[%d] %u commits <1:%u 1:%u 2:%u 3:%u 4+:%u", at ? ", " : "", e->name,
                     e->pid, e->commits, e->gaps[0], e->gaps[1], e->gaps[2], e->gaps[3], e->gaps[4]);
        at = switch_frames_advance(at, n, size);
    }
    if (d != NULL && d->reports != 0 && at < size - 1) {
        n = snprintf(out + at, size - at, "%sdelivery %u/%u reports p50/p99/max %llu/%llu/%llu us to %s[%d]",
                     at ? "; " : "", d->latency.runs, d->reports,
                     (unsigned long long)switch_sched_percentile(&d->latency, 50),
                     (unsigned long long)switch_sched_percentile(&d->latency, 99),
                     (unsigned long long)d->latency.max_us, d->to, d->to_pid);
        at = switch_frames_advance(at, n, size);
    }
    return at;
}

#endif /* SWITCH_DIAG_FRAMES_H */
