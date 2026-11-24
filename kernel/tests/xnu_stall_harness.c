#include "xnu_stall_harness.h"

#include <stdlib.h>

#include "switch_stall.h"

static struct switch_stall_episode episode;

static struct switch_stall_sample convert(const struct xnu_stall_sample *s, uint64_t threshold)
{
    struct switch_stall_sample out = {
        .waiting = s->waiting,
        .runnable = s->runnable,
        .running = s->running,
        .run_loop_idle = s->run_loop_idle,
        .now = s->now,
        .last_run = s->last_run,
        .made_runnable = s->made_runnable,
        .threshold = threshold,
    };
    return out;
}

uint64_t xnu_stall_threshold_ns(void)
{
    return SWITCH_STALL_NS;
}

uint64_t xnu_stall_not_runnable(void)
{
    return SWITCH_STALL_NOT_RUNNABLE;
}

int xnu_stall_kind(const struct xnu_stall_sample *s, uint64_t threshold)
{
    const struct switch_stall_sample sample = convert(s, threshold);
    return (int)switch_stall_kind(&sample);
}

void xnu_stall_reset(void)
{
    const struct switch_stall_episode none = {0};
    episode = none;
}

int xnu_stall_step(const struct xnu_stall_sample *s, uint64_t threshold)
{
    const struct switch_stall_sample sample = convert(s, threshold);
    return (int)switch_stall_step(&episode, &sample);
}

uint64_t xnu_stall_since(void)
{
    return episode.since;
}

int xnu_stall_episode_kind(void)
{
    return (int)episode.kind;
}

const char *xnu_stall_wait_name(unsigned hint)
{
    return switch_stall_wait_name(hint);
}

unsigned xnu_stall_wait_names(void)
{
    return SWITCH_STALL_WAIT_NAMES;
}

bool xnu_stall_owner_is_thread(int pid, uint64_t tid)
{
    const struct switch_stall_owner owner = {.pid = pid, .tid = tid};

    return switch_stall_owner_is_thread(&owner);
}

bool xnu_stall_owner_known(int pid, uint64_t tid)
{
    const struct switch_stall_owner owner = {.pid = pid, .tid = tid};

    return switch_stall_owner_known(&owner);
}

bool xnu_stall_hop_present(int pid, uint64_t tid, bool locked)
{
    const struct switch_stall_owner owner = {.pid = pid, .tid = tid};

    return switch_stall_hop_present(&owner, locked);
}

bool xnu_stall_rwlock_wait(unsigned hint)
{
    return switch_stall_rwlock_wait(hint);
}

uint64_t xnu_stall_rwlock_of(uint64_t event, unsigned hint, bool allow_typed)
{
    return (uint64_t)switch_stall_rwlock_of(event, hint, allow_typed);
}

unsigned xnu_stall_most(const uint64_t *tids, const uint64_t *cpus, unsigned n, bool complete, const uint64_t *now_tids,
                        const uint64_t *now_cpus, unsigned now_n, unsigned k, uint64_t *top_tids, uint64_t *top_gains)
{
    struct switch_stall_used seen[64];
    struct switch_stall_top top[8] = {{0}};
    unsigned entered = 0;

    for (unsigned i = 0; i < n; i++) {
        seen[i].tid = tids[i];
        seen[i].cpu = cpus[i];
    }
    qsort(seen, n, sizeof(seen[0]), switch_stall_used_cmp);
    for (unsigned i = 0; i < now_n; i++) {
        const struct switch_stall_top e = {
            .tid = now_tids[i],
            .cpu = switch_stall_gained(seen, n, complete, now_tids[i], now_cpus[i]),
            .ref = &seen[0],
        };
        switch_stall_rank(top, k, e);
    }
    for (unsigned i = 0; i < k; i++) {
        top_tids[i] = top[i].ref != NULL ? top[i].tid : 0;
        top_gains[i] = top[i].ref != NULL ? top[i].cpu : 0;
        if (top[i].ref != NULL) {
            entered++;
        }
    }
    return entered;
}

int xnu_stall_user_where(uint64_t pc, uint64_t sr_base, uint64_t sr_size, uint64_t sr_slide, uint64_t header,
                         uint64_t *value)
{
    return (int)switch_stall_user_where(pc, sr_base, sr_size, sr_slide, header, value);
}
