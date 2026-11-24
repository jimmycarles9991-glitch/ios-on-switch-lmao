#ifndef SWITCH_ATOMICS_INTERNAL_H
#define SWITCH_ATOMICS_INTERNAL_H

#include "switch_atomics.h"

#include <stddef.h>

#define ATOMICS_MAX_THREADS 16
#define ATOMICS_NODES       64
/* The target's offset in the hammer's cell: guard bytes on both sides catch a store of the wrong width. */
#define ATOMICS_TARGET 16
#define ATOMICS_GUARD  0xA5u

struct switch_atomic_ret {
    uint64_t old, clobbered;
};

typedef struct switch_atomic_ret (*atomic_fn)(void *address, uint64_t a, uint64_t b);
typedef struct switch_atomic_ret (*atomic_pair_fn)(void *pair, const uint64_t spec[4], uint64_t old[2]);

enum atomics_kind {
    KIND_COUNT,  /* add 1 each call: the final value and the sum of old values */
    KIND_SET,    /* set the thread's own bit, which must have been clear; cleared again by an ARMv8.0 bic */
    KIND_CLEAR,  /* the inverse, after an ARMv8.0 orr */
    KIND_TOGGLE, /* flip the thread's own bit: its old state alternates */
    KIND_MAX,    /* rising values: the word never falls and ends at the round's highest */
    KIND_MIN,    /* falling values, the mirror */
    KIND_SWAP,   /* unique tokens passed on: every value swapped in is swapped out once, or is the last */
    KIND_LOCK,   /* a lock from the operation guarding a plain counter */
    KIND_READ,   /* a compare that stores what it compared, racing ARMv8.0 increments: reads rise, none is lost */
    KIND_STACK,  /* a version-tagged lock-free stack of counted nodes */
};

/* How a lock form takes and drops the lock: through the form, or through the harness's ARMv8.0 CAS (the other half of
 * a form that discards a register). */
enum atomics_lock_half {
    LOCK_FORM,
    LOCK_V80,
};

struct atomics_hammer;

struct atomics_form {
    const char *name;
    enum atomics_kind kind;
    unsigned bits;
    bool signed_compare, acquire, release, has_old;
    enum atomics_lock_half take, drop;
    atomic_fn fn;
    atomic_pair_fn pair;
    const uint32_t *site;
    /* Library forms: the target, made ready before the threads start and torn down after (pthread_mutex_t), and a
     * worker and check of their own where the kind's do not fit (OSAtomicEnqueue's queue). */
    void (*prepare)(struct atomics_hammer *h);
    void (*finish)(struct atomics_hammer *h);
    void (*work)(struct atomics_hammer *h, unsigned t);
    void (*check)(struct atomics_hammer *h);
};

struct atomics_barrier {
    uint64_t arrived, generation;
};

struct atomics_node {
    uint64_t next, count, owned, link;
};

struct atomics_hammer {
    const struct atomics_form *form;
    unsigned threads;
    uint64_t ops, rounds, per_round;
    _Alignas(64) uint8_t cell[128];
    void *target;
    uint64_t counter, holder; /* plain, under the lock under test */
    uint64_t start;           /* KIND_MAX/MIN: the value each round starts from */
    uint64_t violations, clobbers, sum, gate;
    uint64_t last[ATOMICS_MAX_THREADS]; /* KIND_SWAP: each thread's last token */
    struct atomics_barrier barrier;
    struct atomics_node nodes[ATOMICS_NODES];
};

/* switch_atomics_forms.S: the harness's own atomics, ARMv8.0 on every build. */
uint64_t switch_atomics_v80_orr_b(void *p, uint64_t v);
uint64_t switch_atomics_v80_orr_h(void *p, uint64_t v);
uint64_t switch_atomics_v80_orr_w(void *p, uint64_t v);
uint64_t switch_atomics_v80_orr_x(void *p, uint64_t v);
uint64_t switch_atomics_v80_bic_b(void *p, uint64_t v);
uint64_t switch_atomics_v80_bic_h(void *p, uint64_t v);
uint64_t switch_atomics_v80_bic_w(void *p, uint64_t v);
uint64_t switch_atomics_v80_bic_x(void *p, uint64_t v);
uint64_t switch_atomics_v80_add_x(void *p, uint64_t v);
uint64_t switch_atomics_v80_swap(uint64_t *p, uint64_t v);
uint64_t switch_atomics_v80_cas(uint64_t *p, uint64_t expected, uint64_t desired);
uint64_t switch_atomics_v80_load(const uint64_t *p);
void switch_atomics_v80_store(uint64_t *p, uint64_t v);

/* switch_atomics_hammer.c */
uint64_t atomics_mask(unsigned bits);
uint64_t atomics_read(const void *p, unsigned bits);
void atomics_write(void *p, unsigned bits, uint64_t v);
void atomics_violation(struct atomics_hammer *h);
void atomics_prepare(struct atomics_hammer *h);
void atomics_work(struct atomics_hammer *h, unsigned t);
void atomics_check(struct atomics_hammer *h);

/* switch_atomics.c */
bool atomics_run(const struct atomics_form *form, unsigned threads, unsigned ops, struct switch_atomics_row *out);

/* switch_atomics_library.c */
extern const struct atomics_form atomics_library[];
extern const unsigned atomics_library_count;

#endif
