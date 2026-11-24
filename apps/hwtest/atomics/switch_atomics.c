#include "switch_atomics_internal.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum atomics_op {
    OP_add,
    OP_clr,
    OP_eor,
    OP_set,
    OP_smax,
    OP_smin,
    OP_umax,
    OP_umin,
    OP_swp,
    OP_cas,
    OP_casp,
};

enum atomics_shape {
    SHAPE_plain,
    SHAPE_rt_zr,
    SHAPE_rt_rs,
    SHAPE_rt_rn,
    SHAPE_rs_zr,
    SHAPE_lr_base,
    SHAPE_lr_source,
    SHAPE_lr_result,
    SHAPE_spill,
};

#define DECLARE_ONE(name)  struct switch_atomic_ret switch_atomic_##name(void *, uint64_t, uint64_t);
#define DECLARE_PAIR(name) struct switch_atomic_ret switch_atomic_##name(void *, const uint64_t *, uint64_t *);
#define DECLARE_add        DECLARE_ONE
#define DECLARE_clr        DECLARE_ONE
#define DECLARE_eor        DECLARE_ONE
#define DECLARE_set        DECLARE_ONE
#define DECLARE_smax       DECLARE_ONE
#define DECLARE_smin       DECLARE_ONE
#define DECLARE_umax       DECLARE_ONE
#define DECLARE_umin       DECLARE_ONE
#define DECLARE_swp        DECLARE_ONE
#define DECLARE_cas        DECLARE_ONE
#define DECLARE_casp       DECLARE_PAIR
#define SWITCH_ATOMIC(name, op, size, order, shape, insn, ldx, stx, r) DECLARE_##op(name)
#include "switch_atomics_list.h"
#undef SWITCH_ATOMIC

/* switch_atomics_forms.S: each form's site, in the list's order. */
extern const uint32_t *const switch_atomic_sites[];

#define ONE_add(name)    switch_atomic_##name
#define ONE_clr(name)    switch_atomic_##name
#define ONE_eor(name)    switch_atomic_##name
#define ONE_set(name)    switch_atomic_##name
#define ONE_smax(name)   switch_atomic_##name
#define ONE_smin(name)   switch_atomic_##name
#define ONE_umax(name)   switch_atomic_##name
#define ONE_umin(name)   switch_atomic_##name
#define ONE_swp(name)    switch_atomic_##name
#define ONE_cas(name)    switch_atomic_##name
#define ONE_casp(name)   NULL
#define PAIR_casp(name)  switch_atomic_##name
#define PAIR_OTHER(name) NULL
#define PAIR_add         PAIR_OTHER
#define PAIR_clr         PAIR_OTHER
#define PAIR_eor         PAIR_OTHER
#define PAIR_set         PAIR_OTHER
#define PAIR_smax        PAIR_OTHER
#define PAIR_smin        PAIR_OTHER
#define PAIR_umax        PAIR_OTHER
#define PAIR_umin        PAIR_OTHER
#define PAIR_swp         PAIR_OTHER
#define PAIR_cas         PAIR_OTHER
#define BITS_b           8
#define BITS_h           16
#define BITS_w           32
#define BITS_x           64
#define ACQUIRE_none     false
#define ACQUIRE_a        true
#define ACQUIRE_l        false
#define ACQUIRE_al       true
#define RELEASE_none     false
#define RELEASE_a        false
#define RELEASE_l        true
#define RELEASE_al       true

struct atomics_entry {
    const char *name;
    enum atomics_op op;
    unsigned bits;
    bool acquire, release;
    enum atomics_shape shape;
    atomic_fn fn;
    atomic_pair_fn pair;
};

static const struct atomics_entry entries[] = {
#define SWITCH_ATOMIC(name, op, size, order, shape, insn, ldx, stx, r) \
    {#name, OP_##op, BITS_##size, ACQUIRE_##order, RELEASE_##order, SHAPE_##shape, ONE_##op(name), PAIR_##op(name)},
#include "switch_atomics_list.h"
#undef SWITCH_ATOMIC
};

static enum atomics_kind kind_of(const struct atomics_entry *e)
{
    switch (e->op) {
    case OP_add:
        return KIND_COUNT;
    case OP_clr:
        return KIND_CLEAR;
    case OP_eor:
        return KIND_TOGGLE;
    case OP_set:
        return KIND_SET;
    case OP_smax:
    case OP_umax:
        return KIND_MAX;
    case OP_smin:
    case OP_umin:
        return KIND_MIN;
    case OP_swp:
        return KIND_SWAP;
    case OP_cas:
        return e->shape == SHAPE_rt_rs ? KIND_READ : KIND_LOCK;
    case OP_casp:
        return KIND_STACK;
    }
    return KIND_COUNT;
}

static struct atomics_form form_at(unsigned index)
{
    const struct atomics_entry *e = &entries[index];
    const bool cas = e->op == OP_cas;
    return (struct atomics_form){
        .name = e->name,
        .kind = kind_of(e),
        .bits = e->bits,
        .signed_compare = e->op == OP_smax || e->op == OP_smin,
        .acquire = e->acquire,
        .release = e->release,
        .has_old = cas ? e->shape != SHAPE_rs_zr : e->shape != SHAPE_rt_zr,
        .take = cas && e->shape == SHAPE_rt_zr ? LOCK_V80 : LOCK_FORM,
        .drop = cas && e->shape == SHAPE_rs_zr ? LOCK_V80 : LOCK_FORM,
        .fn = e->fn,
        .pair = e->pair,
        .site = switch_atomic_sites[index],
    };
}

bool switch_atomics_lse(void)
{
#if defined(__ARM_FEATURE_ATOMICS)
    return true;
#else
    return false;
#endif
}

unsigned switch_atomics_forms(void)
{
    return sizeof(entries) / sizeof(entries[0]);
}

unsigned switch_atomics_library_forms(void)
{
    return atomics_library_count;
}

struct worker_arg {
    struct atomics_hammer *h;
    unsigned t;
};

static void *worker(void *raw)
{
    const struct worker_arg *a = raw;
    struct atomics_hammer *h = a->h;
    switch_atomics_v80_add_x(&h->gate, 1);
    for (unsigned spins = 0; switch_atomics_v80_load(&h->gate) <= h->threads; spins++) {
        if ((spins & 16383) == 16383)
            sched_yield();
    }
    atomics_work(h, a->t);
    return NULL;
}

/* The operations a hammer made: rounds of a min/max form are whole. */
static uint64_t made(const struct atomics_hammer *h)
{
    const bool rounds = h->form->kind == KIND_MAX || h->form->kind == KIND_MIN;
    return (rounds ? h->rounds * h->per_round : h->ops) * h->threads;
}

bool atomics_run(const struct atomics_form *form, unsigned threads, unsigned ops, struct switch_atomics_row *out)
{
    const bool own_bits = form->kind == KIND_SET || form->kind == KIND_CLEAR || form->kind == KIND_TOGGLE;
    if (threads < 2 || threads > ATOMICS_MAX_THREADS || ops == 0 || (own_bits && threads > form->bits))
        return false;
    struct atomics_hammer *h = aligned_alloc(64, sizeof(*h));
    if (!h)
        return false;
    memset(h, 0, sizeof(*h));
    h->form = form;
    h->threads = threads;
    h->ops = ops;
    atomics_prepare(h);

    pthread_t thread[ATOMICS_MAX_THREADS];
    struct worker_arg arg[ATOMICS_MAX_THREADS];
    unsigned started = 0;
    for (; started < threads; started++) {
        arg[started] = (struct worker_arg){h, started};
        if (pthread_create(&thread[started], NULL, worker, &arg[started]))
            break;
    }
    for (unsigned spins = 0; started == threads && switch_atomics_v80_load(&h->gate) < threads; spins++) {
        if ((spins & 16383) == 16383)
            sched_yield();
    }
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    switch_atomics_v80_add_x(&h->gate, started == threads ? 1 : threads + 1);
    for (unsigned t = 0; t < started; t++)
        pthread_join(thread[t], NULL);
    const uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

    if (started == threads)
        atomics_check(h);
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", form->name);
    out->ops = made(h);
    out->ns = end - start;
    out->violations = h->violations;
    out->clobbers = h->clobbers;
    out->branched = form->site && (*form->site & 0xFC000000u) == 0x14000000u;
    free(h);
    return started == threads;
}

bool switch_atomics_form(unsigned index, unsigned threads, unsigned ops, struct switch_atomics_row *out)
{
    if (index >= switch_atomics_forms())
        return false;
    const struct atomics_form form = form_at(index);
    return atomics_run(&form, threads, ops, out);
}

bool switch_atomics_library_form(unsigned index, unsigned threads, unsigned ops, struct switch_atomics_row *out)
{
    return index < atomics_library_count && atomics_run(&atomics_library[index], threads, ops, out);
}
