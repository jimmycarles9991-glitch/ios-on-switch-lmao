// SPDX-License-Identifier: GPL-2.0-only
/*
 * A client's commit through the render server (commit.c, debug kernels), from what the scheduler sees of backboardd's
 * two Core Animation threads: the commit sent (the newest commit message from any client), received (the render-server
 * thread made runnable after it, or the send itself when that thread was already on a core and took it without
 * waiting), applied (that thread back in a message receive: the kernel's hint is the same for any receive, MIG reply
 * waits included, so each applied stamp is counted by the receive's kind, a port set or a single port), and the display
 * thread's next wake, where the frame that shows it begins. Each stage follows the newest commit sent before it, named
 * by its send time, so a frame that shows several commits counts the newest. The render-server thread's hooks alone
 * write `struct switch_commit_server`, the display thread's alone `struct switch_commit_shown`. Plain data, so
 * kernel/tests runs it on the host. Mach ticks.
 */
#ifndef SWITCH_SCHED_COMMIT_H
#define SWITCH_SCHED_COMMIT_H

#include <stdbool.h>
#include <stdint.h>

#include "stats.h"

struct switch_commit_applied {
    uint64_t sent, received, applied;
    bool on_core;  /* taken while on a core: received is the send */
    bool port_set; /* applied at a port-set receive (else a single port's) */
};

struct switch_commit_server {
    uint64_t received_sent, received;
    bool on_core;
    /* to the display thread: applied[published % 2] is whole once `published` is stored (release) */
    struct switch_commit_applied applied[2];
    uint64_t published;
};

/* The render-server thread is dispatched at `now`, made runnable at `made` (0: not known); the newest commit was sent
 * at `sent` (0: none yet). */
static inline void switch_commit_dispatched(struct switch_commit_server *s, uint64_t sent, uint64_t made, uint64_t now)
{
    if (sent == 0 || sent == s->received_sent || sent > now) {
        return;
    }
    s->received_sent = sent;
    s->on_core = made == 0 || made < sent;
    s->received = s->on_core ? sent : made;
}

/* The render-server thread leaves a core at `now`, into a message receive (`receiving`, on a port set: `port_set`) or
 * not. True when the newest commit it took is newly applied, in applied[(published + 1) % 2]: the caller then
 * publishes it. */
static inline bool switch_commit_departed(struct switch_commit_server *s, uint64_t sent, bool receiving, bool port_set,
                                          uint64_t now)
{
    struct switch_commit_applied *a = NULL;

    if (!receiving || sent == 0 || sent > now) {
        return false;
    }
    if (sent != s->received_sent) {
        s->received_sent = sent;
        s->received = sent;
        s->on_core = true;
    }
    if (s->received_sent == s->applied[s->published % 2].sent) {
        return false;
    }
    a = &s->applied[(s->published + 1) % 2];
    a->sent = s->received_sent;
    a->received = s->received;
    a->applied = now;
    a->on_core = s->on_core;
    a->port_set = port_set;
    return true;
}

/* The display thread's side: each stage's length, cumulative (the report takes differences). */
struct switch_commit_shown {
    uint64_t sent; /* the last applied commit counted */
    struct switch_sched_window receive, apply, wait, total;
    uint32_t on_core, skipped; /* taken while on a core; publications that moved under the read */
    uint32_t port_set;         /* applied at a port-set receive (the rest at a single port's) */
};

/* The display thread is dispatched at `now`; `a` was copied from the publication `published`, which read `after` once
 * the copy was done. us() turns ticks into microseconds. */
static inline void switch_commit_woke(struct switch_commit_shown *d, const struct switch_commit_applied *a,
                                      uint64_t published, uint64_t after, uint64_t now, uint64_t (*us)(uint64_t))
{
    if (published == 0 || a->sent == d->sent) {
        return;
    }
    if (after != published || now < a->applied || a->applied < a->received || a->received < a->sent) {
        d->skipped++;
        return;
    }
    d->sent = a->sent;
    switch_sched_note(&d->receive, us(a->received - a->sent));
    switch_sched_note(&d->apply, us(a->applied - a->received));
    switch_sched_note(&d->wait, us(now - a->applied));
    switch_sched_note(&d->total, us(now - a->sent));
    if (a->on_core) {
        d->on_core++;
    }
    if (a->port_set) {
        d->port_set++;
    }
}

/* A window of a cumulative histogram; its largest is bounded by its top bucket's upper edge and the largest ever. */
static inline void switch_commit_window(struct switch_sched_window *w, const struct switch_sched_window *now,
                                        const struct switch_sched_window *was)
{
    w->runs = now->runs - was->runs;
    w->max_us = 0;
    for (unsigned int b = 0; b < SWITCH_SCHED_BUCKETS; b++) {
        w->hist[b] = now->hist[b] - was->hist[b];
        if (w->hist[b] != 0) {
            w->max_us = b + 1 < SWITCH_SCHED_BUCKETS && (1ull << b) < now->max_us ? 1ull << b : now->max_us;
        }
    }
}

/* "812 commits to the frame's wake, p50/p99 us: received 16/64, applied 128/2048, waited 8192/16384, all
 * 8192/32768 (31 taken on core, 0 skipped; applied at 800 port-set receives, 12 single-port)"; percentiles are
 * power-of-two bounds. */
static inline size_t switch_commit_format(char *out, size_t size, const struct switch_commit_shown *now,
                                          const struct switch_commit_shown *was)
{
    struct switch_sched_window w[4];
    const struct switch_sched_window *n[4] = {&now->receive, &now->apply, &now->wait, &now->total};
    const struct switch_sched_window *o[4] = {&was->receive, &was->apply, &was->wait, &was->total};
    int written = 0;

    if (size == 0) {
        return 0;
    }
    for (unsigned int i = 0; i < 4; i++) {
        switch_commit_window(&w[i], n[i], o[i]);
    }
    written = snprintf(
        out, size,
        "%u commits to the frame's wake, p50/p99 us: received %llu/%llu, applied %llu/%llu, waited "
        "%llu/%llu, all %llu/%llu (%u taken on core, %u skipped; applied at %u port-set receives, %u single-port)",
        w[3].runs, (unsigned long long)switch_sched_percentile(&w[0], 50),
        (unsigned long long)switch_sched_percentile(&w[0], 99), (unsigned long long)switch_sched_percentile(&w[1], 50),
        (unsigned long long)switch_sched_percentile(&w[1], 99), (unsigned long long)switch_sched_percentile(&w[2], 50),
        (unsigned long long)switch_sched_percentile(&w[2], 99), (unsigned long long)switch_sched_percentile(&w[3], 50),
        (unsigned long long)switch_sched_percentile(&w[3], 99), now->on_core - was->on_core,
        now->skipped - was->skipped, now->port_set - was->port_set, w[3].runs - (now->port_set - was->port_set));
    return switch_sched_appended(0, written, size);
}

#endif /* SWITCH_SCHED_COMMIT_H */
