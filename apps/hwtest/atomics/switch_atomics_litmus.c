/* Store buffering, run from two threads on fresh pairs of words: each stores 1 to its own word through an atomic and
 * then loads the other's plainly. Both loading 0 is forbidden for an acquire-release atomic (LSE's AL forms order
 * their store before every later access) and allowed for LDAXR/STLXR with no barrier after it, which a64x's stubs run
 * for the AL forms today. iOS has no CPU affinity, so the rows rely on many runs instead. */
#include "switch_atomics_internal.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LITMUS_BATCH 4096
/* Spins on the other thread's arrival before yielding, for when it lost its core. */
#define LITMUS_SPINS 4096

typedef uint64_t (*litmus_fn)(uint64_t *mine, uint64_t *other);

/* switch_atomics_harness.S */
uint64_t switch_sb_llsc_plain(uint64_t *mine, uint64_t *other);
uint64_t switch_sb_llsc_dmb(uint64_t *mine, uint64_t *other);
uint64_t switch_sb_swpal(uint64_t *mine, uint64_t *other);
uint64_t switch_sb_cas_llsc_plain(uint64_t *mine, uint64_t *other);
uint64_t switch_sb_cas_llsc_dmb(uint64_t *mine, uint64_t *other);
uint64_t switch_sb_casal(uint64_t *mine, uint64_t *other);
extern const uint32_t *const switch_sb_sites[];

enum litmus_judged {
    LITMUS_INFORMATION, /* the CPU may show the outcome: a count above 0 proves it reorders across the pair */
    LITMUS_ALWAYS,
    LITMUS_LSE, /* an AL instruction only an LSE build holds; built without, it is today's stub, like the first row */
};

static const struct litmus {
    const char *name;
    litmus_fn fn;
    int site;
    enum litmus_judged judged;
} litmus[] = {
    {"sb_llsc_plain", switch_sb_llsc_plain, -1, LITMUS_INFORMATION},
    {"sb_llsc_dmb", switch_sb_llsc_dmb, -1, LITMUS_ALWAYS},
    {"sb_translated_al", switch_sb_swpal, 0, LITMUS_LSE},
    {"sb_cas_llsc_plain", switch_sb_cas_llsc_plain, -1, LITMUS_INFORMATION},
    {"sb_cas_llsc_dmb", switch_sb_cas_llsc_dmb, -1, LITMUS_ALWAYS},
    {"sb_cas_translated_al", switch_sb_casal, 1, LITMUS_LSE},
};

struct litmus_run {
    litmus_fn fn;
    uint64_t batches, forbidden;
    _Alignas(64) uint64_t arrived[2][8]; /* each thread's ticket, a cache line apart */
    _Alignas(64) uint64_t done[2][8];    /* each thread's finished batches */
    _Alignas(64) uint64_t reset;         /* batches thread 0 has counted and cleared */
    uint64_t *x, *y, *seen[2];
};

struct litmus_arg {
    struct litmus_run *run;
    unsigned t;
};

static void wait_at_least(const uint64_t *word, uint64_t value)
{
    for (unsigned spins = 0; switch_atomics_v80_load(word) < value; spins++) {
        if (spins >= LITMUS_SPINS)
            sched_yield();
    }
}

static void count_batch(struct litmus_run *run)
{
    for (unsigned s = 0; s < LITMUS_BATCH; s++)
        run->forbidden += run->seen[0][s] == 0 && run->seen[1][s] == 0;
    memset(run->x, 0, LITMUS_BATCH * sizeof(uint64_t));
    memset(run->y, 0, LITMUS_BATCH * sizeof(uint64_t));
}

/* Each slot starts when both threads have reached it, so their atomics and loads overlap in time. */
static void *litmus_thread(void *raw)
{
    const struct litmus_arg *a = raw;
    struct litmus_run *run = a->run;
    const unsigned t = a->t;
    uint64_t *mine = t ? run->y : run->x, *other = t ? run->x : run->y;
    for (uint64_t b = 0; b < run->batches; b++) {
        wait_at_least(&run->reset, b);
        for (unsigned s = 0; s < LITMUS_BATCH; s++) {
            const uint64_t ticket = b * LITMUS_BATCH + s + 1;
            switch_atomics_v80_store(&run->arrived[t][0], ticket);
            wait_at_least(&run->arrived[1 - t][0], ticket);
            run->seen[t][s] = run->fn(&mine[s], &other[s]);
        }
        switch_atomics_v80_store(&run->done[t][0], b + 1);
        if (t == 0) {
            wait_at_least(&run->done[1][0], b + 1);
            count_batch(run);
            switch_atomics_v80_store(&run->reset, b + 1);
        }
    }
    return NULL;
}

unsigned switch_atomics_litmus_tests(void)
{
    return sizeof(litmus) / sizeof(litmus[0]);
}

bool switch_atomics_litmus(unsigned index, uint64_t runs, struct switch_litmus_row *out)
{
    if (index >= switch_atomics_litmus_tests() || runs < LITMUS_BATCH)
        return false;
    const struct litmus *l = &litmus[index];
    struct litmus_run *run = aligned_alloc(64, sizeof(*run));
    uint64_t *words = calloc(4 * LITMUS_BATCH, sizeof(uint64_t));
    if (!run || !words) {
        free(run);
        free(words);
        return false;
    }
    memset(run, 0, sizeof(*run));
    run->fn = l->fn;
    run->batches = runs / LITMUS_BATCH;
    run->x = words;
    run->y = words + LITMUS_BATCH;
    run->seen[0] = words + 2 * LITMUS_BATCH;
    run->seen[1] = words + 3 * LITMUS_BATCH;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_t thread[2];
    struct litmus_arg arg[2] = {{run, 0}, {run, 1}};
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const bool first = pthread_create(&thread[0], &attr, litmus_thread, &arg[0]) == 0;
    const bool second = first && pthread_create(&thread[1], &attr, litmus_thread, &arg[1]) == 0;
    pthread_attr_destroy(&attr);
    if (first && !second) {
        /* The lone thread waits for a partner that never comes: it is left blocked, its memory with it. */
        return false;
    }
    if (second) {
        pthread_join(thread[0], NULL);
        pthread_join(thread[1], NULL);
    }
    const uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", l->name);
    out->runs = run->batches * LITMUS_BATCH;
    out->forbidden = run->forbidden;
    out->ns = end - start;
    out->judged = l->judged == LITMUS_ALWAYS || (l->judged == LITMUS_LSE && switch_atomics_lse());
    out->has_site = l->site >= 0;
    out->branched = l->site >= 0 && (*switch_sb_sites[l->site] & 0xFC000000u) == 0x14000000u;
    free(words);
    free(run);
    return second;
}
