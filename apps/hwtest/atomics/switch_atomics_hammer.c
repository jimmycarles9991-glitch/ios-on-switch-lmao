/* The invariants: what each thread does with a form, and what the target must hold once every thread is done. A lost
 * update, a doubled one, a wrong old value or a store of the wrong width breaks at least one of them. */
#include "switch_atomics_internal.h"

#include <sched.h>
#include <string.h>

uint64_t atomics_mask(unsigned bits)
{
    return bits >= 64 ? UINT64_MAX : (1ull << bits) - 1;
}

uint64_t atomics_read(const void *p, unsigned bits)
{
    switch (bits) {
    case 8:
        return *(const volatile uint8_t *)p;
    case 16:
        return *(const volatile uint16_t *)p;
    case 32:
        return *(const volatile uint32_t *)p;
    default:
        return *(const volatile uint64_t *)p;
    }
}

void atomics_write(void *p, unsigned bits, uint64_t v)
{
    switch (bits) {
    case 8:
        *(volatile uint8_t *)p = (uint8_t)v;
        break;
    case 16:
        *(volatile uint16_t *)p = (uint16_t)v;
        break;
    case 32:
        *(volatile uint32_t *)p = (uint32_t)v;
        break;
    default:
        *(volatile uint64_t *)p = v;
        break;
    }
}

void atomics_violation(struct atomics_hammer *h)
{
    switch_atomics_v80_add_x(&h->violations, 1);
}

static void full_barrier(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

static void load_barrier(void)
{
    __asm__ volatile("dmb ishld" ::: "memory");
}

static struct switch_atomic_ret call(struct atomics_hammer *h, uint64_t a, uint64_t b)
{
    const struct switch_atomic_ret r = h->form->fn(h->target, a, b);
    if (r.clobbered)
        switch_atomics_v80_add_x(&h->clobbers, 1);
    return r;
}

static uint64_t v80_orr(void *p, unsigned bits, uint64_t v)
{
    switch (bits) {
    case 8:
        return switch_atomics_v80_orr_b(p, v);
    case 16:
        return switch_atomics_v80_orr_h(p, v);
    case 32:
        return switch_atomics_v80_orr_w(p, v);
    default:
        return switch_atomics_v80_orr_x(p, v);
    }
}

static uint64_t v80_bic(void *p, unsigned bits, uint64_t v)
{
    switch (bits) {
    case 8:
        return switch_atomics_v80_bic_b(p, v);
    case 16:
        return switch_atomics_v80_bic_h(p, v);
    case 32:
        return switch_atomics_v80_bic_w(p, v);
    default:
        return switch_atomics_v80_bic_x(p, v);
    }
}

static void barrier_wait(struct atomics_hammer *h)
{
    const uint64_t generation = switch_atomics_v80_load(&h->barrier.generation);
    if (switch_atomics_v80_add_x(&h->barrier.arrived, 1) == h->threads) {
        switch_atomics_v80_store(&h->barrier.arrived, 0);
        switch_atomics_v80_store(&h->barrier.generation, generation + 1);
        return;
    }
    while (switch_atomics_v80_load(&h->barrier.generation) == generation)
        sched_yield();
}

static uint64_t mix(uint64_t v)
{
    v ^= v >> 30;
    v *= 0xBF58476D1CE4E5B9ull;
    v ^= v >> 27;
    v *= 0x94D049BB133111EBull;
    return v ^ (v >> 31);
}

static int64_t signed_in(uint64_t v, unsigned bits)
{
    return bits >= 64 ? (int64_t)v : (int64_t)(v << (64 - bits)) >> (64 - bits);
}

/* a < b in the form's width and signedness. */
static bool below(const struct atomics_hammer *h, uint64_t a, uint64_t b)
{
    const unsigned bits = h->form->bits;
    return h->form->signed_compare ? signed_in(a, bits) < signed_in(b, bits) : a < b;
}

static void count_work(struct atomics_hammer *h)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < h->ops; i++)
        sum += call(h, 1, 0).old;
    switch_atomics_v80_add_x(&h->sum, sum);
}

/* The old values of `total` increments from 0 in `bits`: 0, 1, ... wrapping. */
static uint64_t count_sum(uint64_t total, unsigned bits)
{
    if (bits >= 64)
        return total * (total - 1) / 2;
    const uint64_t span = 1ull << bits, rem = total % span;
    return total / span * (span / 2 * (span - 1)) + rem * (rem - 1) / 2;
}

static void count_check(struct atomics_hammer *h)
{
    const unsigned bits = h->form->bits;
    const uint64_t total = h->ops * h->threads;
    if (atomics_read(h->target, bits) != (total & atomics_mask(bits)))
        atomics_violation(h);
    if (h->form->has_old && h->sum != count_sum(total, bits))
        atomics_violation(h);
}

static void bits_work(struct atomics_hammer *h, unsigned t)
{
    const unsigned bits = h->form->bits;
    const uint64_t own = 1ull << (t % bits);
    for (uint64_t i = 0; i < h->ops; i++) {
        if (h->form->kind == KIND_SET) {
            if (call(h, own, 0).old & own)
                atomics_violation(h);
            v80_bic(h->target, bits, own);
        } else if (h->form->kind == KIND_CLEAR) {
            v80_orr(h->target, bits, own);
            if (!(call(h, own, 0).old & own))
                atomics_violation(h);
        } else if (((call(h, own, 0).old & own) != 0) != ((i & 1) != 0)) {
            atomics_violation(h);
        }
    }
}

static void bits_check(struct atomics_hammer *h)
{
    uint64_t want = 0;
    if (h->form->kind == KIND_TOGGLE && (h->ops & 1)) {
        for (unsigned t = 0; t < h->threads; t++)
            want |= 1ull << (t % h->form->bits);
    }
    if (atomics_read(h->target, h->form->bits) != want)
        atomics_violation(h);
}

/* Rounds of rising (or falling) values, each thread's distinct: the word never moves against the operation, so a
 * thread's next old value is at least (at most) what it last saw and last wrote, and a round ends at its extreme. */
static void extreme_work(struct atomics_hammer *h, unsigned t)
{
    const unsigned bits = h->form->bits;
    const uint64_t mask = atomics_mask(bits);
    const bool max = h->form->kind == KIND_MAX;
    for (uint64_t round = 0; round < h->rounds; round++) {
        if (t == 0)
            atomics_write(h->target, bits, h->start);
        barrier_wait(h);
        uint64_t floor = h->start;
        for (uint64_t i = 0; i < h->per_round; i++) {
            const uint64_t k = i * h->threads + t + 1;
            const uint64_t v = (max ? h->start + k : h->start - k) & mask;
            const uint64_t old = call(h, v, 0).old;
            if (max ? below(h, old, floor) : below(h, floor, old))
                atomics_violation(h);
            floor = max ? (below(h, old, v) ? v : old) : (below(h, v, old) ? v : old);
        }
        barrier_wait(h);
        const uint64_t span = h->per_round * h->threads;
        if (t == 0 && atomics_read(h->target, bits) != ((max ? h->start + span : h->start - span) & mask))
            atomics_violation(h);
    }
}

static void extreme_prepare(struct atomics_hammer *h)
{
    const unsigned bits = h->form->bits;
    const uint64_t mask = atomics_mask(bits);
    h->per_round = bits < 32 && (mask - 2) / h->threads < h->ops ? (mask - 2) / h->threads : h->ops;
    h->rounds = h->ops / h->per_round;
    const uint64_t half_span = h->per_round * h->threads / 2;
    const uint64_t middle = h->form->signed_compare ? 0 : 1ull << (bits - 1);
    const bool max = h->form->kind == KIND_MAX;
    h->start = (max ? middle - half_span : middle + half_span) & mask;
}

static void swap_work(struct atomics_hammer *h, unsigned t)
{
    const uint64_t mask = atomics_mask(h->form->bits);
    uint64_t sum = 0, token = 0;
    for (uint64_t i = 0; i < h->ops; i++) {
        token = (i * h->threads + t + 1) & mask;
        sum += mix(call(h, token, 0).old) - mix(token);
    }
    h->last[t] = token;
    switch_atomics_v80_add_x(&h->sum, sum);
}

/* Every token swapped in is swapped out once or is the last left: the mixed sums of both sides meet. */
static void swap_check(struct atomics_hammer *h)
{
    const uint64_t last = atomics_read(h->target, h->form->bits);
    if (h->form->has_old) {
        if (h->sum + mix(last) - mix(0) != 0)
            atomics_violation(h);
        return;
    }
    for (unsigned t = 0; t < h->threads; t++) {
        if (h->last[t] == last)
            return;
    }
    atomics_violation(h);
}

static bool lock_take(struct atomics_hammer *h, uint64_t token)
{
    if (h->form->take == LOCK_V80)
        return switch_atomics_v80_cas(h->target, 0, token) == 0;
    const struct switch_atomic_ret r = call(h, 0, token);
    if (h->form->has_old)
        return (r.old & atomics_mask(h->form->bits)) == 0;
    return switch_atomics_v80_load(h->target) == token;
}

static uint64_t lock_drop(struct atomics_hammer *h, uint64_t token)
{
    if (h->form->drop == LOCK_V80)
        return switch_atomics_v80_cas(h->target, token, 0);
    return call(h, token, 0).old & atomics_mask(h->form->bits);
}

/* The form's acquire and release orderings guard the counter; a form without one gets the barrier it lacks, so the
 * lock judges the form's atomicity and, where it has them, its orderings. */
static void lock_work(struct atomics_hammer *h, unsigned t)
{
    volatile uint64_t *holder = &h->holder, *counter = &h->counter;
    const uint64_t token = t + 1;
    for (uint64_t i = 0; i < h->ops; i++) {
        while (!lock_take(h, token))
            sched_yield();
        if (!h->form->acquire)
            full_barrier();
        if (*holder != 0)
            atomics_violation(h);
        *holder = token;
        *counter = *counter + 1;
        if (*holder != token)
            atomics_violation(h);
        *holder = 0;
        if (!h->form->release)
            full_barrier();
        if (lock_drop(h, token) != token)
            atomics_violation(h);
    }
}

static void lock_check(struct atomics_hammer *h)
{
    if (h->counter != h->ops * h->threads)
        atomics_violation(h);
}

/* Odd threads add with ARMv8.0 exclusives; even ones read through a compare that stores what it compared. */
static void read_work(struct atomics_hammer *h, unsigned t)
{
    uint64_t last = 0;
    for (uint64_t i = 0; i < h->ops; i++) {
        if (t & 1) {
            switch_atomics_v80_add_x(h->target, 1);
            continue;
        }
        const uint64_t now = call(h, last, 0).old;
        if (now < last)
            atomics_violation(h);
        last = now;
    }
}

static void read_check(struct atomics_hammer *h)
{
    if (atomics_read(h->target, 64) != h->ops * (h->threads / 2))
        atomics_violation(h);
}

/* The stack's head: the top node's index plus one (0: empty) and a version, in a pair of the form's width. */
static void head_read(const struct atomics_hammer *h, uint64_t *top, uint64_t *version)
{
    if (h->form->bits == 32) {
        const uint64_t both = *(const volatile uint64_t *)h->target;
        *top = (uint32_t)both;
        *version = both >> 32;
    } else {
        *top = ((const volatile uint64_t *)h->target)[0];
        *version = ((const volatile uint64_t *)h->target)[1];
    }
}

static bool head_swap(struct atomics_hammer *h, uint64_t top, uint64_t version, uint64_t new_top)
{
    const uint64_t mask = atomics_mask(h->form->bits);
    const uint64_t spec[4] = {top, version, new_top, (version + 1) & mask};
    uint64_t old[2];
    if (h->form->pair(h->target, spec, old).clobbered)
        switch_atomics_v80_add_x(&h->clobbers, 1);
    return (old[0] & mask) == top && (old[1] & mask) == version;
}

static void stack_work(struct atomics_hammer *h)
{
    for (uint64_t i = 0; i < h->ops; i++) {
        uint64_t top, version, next;
        for (;;) {
            head_read(h, &top, &version);
            if (top == 0) {
                sched_yield();
                continue;
            }
            load_barrier();
            next = *(volatile uint64_t *)&h->nodes[top - 1].next;
            if (head_swap(h, top, version, next))
                break;
        }
        if (!h->form->acquire)
            full_barrier();
        struct atomics_node *node = &h->nodes[top - 1];
        if (switch_atomics_v80_swap(&node->owned, 1))
            atomics_violation(h);
        *(volatile uint64_t *)&node->count = node->count + 1;
        switch_atomics_v80_store(&node->owned, 0);
        uint64_t below_top;
        do {
            head_read(h, &below_top, &version);
            *(volatile uint64_t *)&node->next = below_top;
            if (!h->form->release)
                full_barrier();
        } while (!head_swap(h, below_top, version, top));
    }
}

static void stack_prepare(struct atomics_hammer *h)
{
    for (unsigned n = 0; n < ATOMICS_NODES; n++)
        h->nodes[n].next = n + 1 < ATOMICS_NODES ? n + 2 : 0;
    if (h->form->bits == 32)
        *(uint64_t *)h->target = 1;
    else
        ((uint64_t *)h->target)[0] = 1;
}

/* Every node on the stack once, and every pop counted on its node. */
static void stack_check(struct atomics_hammer *h)
{
    bool seen[ATOMICS_NODES] = {false};
    uint64_t top, version, nodes = 0, pops = 0;
    head_read(h, &top, &version);
    while (top != 0 && top <= ATOMICS_NODES && !seen[top - 1] && nodes < ATOMICS_NODES) {
        seen[top - 1] = true;
        nodes++;
        top = h->nodes[top - 1].next;
    }
    for (unsigned n = 0; n < ATOMICS_NODES; n++)
        pops += h->nodes[n].count;
    if (top != 0 || nodes != ATOMICS_NODES || pops != h->ops * h->threads)
        atomics_violation(h);
}

static size_t target_width(const struct atomics_form *form)
{
    return form->kind == KIND_STACK ? 2 * form->bits / 8 : form->bits / 8;
}

void atomics_prepare(struct atomics_hammer *h)
{
    memset(h->cell, ATOMICS_GUARD, sizeof(h->cell));
    h->target = h->cell + ATOMICS_TARGET;
    memset(h->target, 0, target_width(h->form));
    if (h->form->kind == KIND_MAX || h->form->kind == KIND_MIN)
        extreme_prepare(h);
    if (h->form->kind == KIND_STACK)
        stack_prepare(h);
    if (h->form->prepare)
        h->form->prepare(h);
}

void atomics_work(struct atomics_hammer *h, unsigned t)
{
    if (h->form->work) {
        h->form->work(h, t);
        return;
    }
    switch (h->form->kind) {
    case KIND_COUNT:
        count_work(h);
        break;
    case KIND_SET:
    case KIND_CLEAR:
    case KIND_TOGGLE:
        bits_work(h, t);
        break;
    case KIND_MAX:
    case KIND_MIN:
        extreme_work(h, t);
        break;
    case KIND_SWAP:
        swap_work(h, t);
        break;
    case KIND_LOCK:
        lock_work(h, t);
        break;
    case KIND_READ:
        read_work(h, t);
        break;
    case KIND_STACK:
        stack_work(h);
        break;
    }
}

/* The bytes around the target keep their guard: no form stored wider than its width. */
static void guards_check(struct atomics_hammer *h)
{
    const size_t width = target_width(h->form);
    for (size_t i = 0; i < sizeof(h->cell); i++) {
        if ((i < ATOMICS_TARGET || i >= ATOMICS_TARGET + width) && h->cell[i] != ATOMICS_GUARD) {
            atomics_violation(h);
            return;
        }
    }
}

void atomics_check(struct atomics_hammer *h)
{
    if (h->form->check) {
        h->form->check(h);
    } else {
        switch (h->form->kind) {
        case KIND_COUNT:
            count_check(h);
            break;
        case KIND_SET:
        case KIND_CLEAR:
        case KIND_TOGGLE:
            bits_check(h);
            break;
        case KIND_MAX:
        case KIND_MIN:
            break;
        case KIND_SWAP:
            swap_check(h);
            break;
        case KIND_LOCK:
            lock_check(h);
            break;
        case KIND_READ:
            read_check(h);
            break;
        case KIND_STACK:
            stack_check(h);
            break;
        }
    }
    if (h->form->site)
        guards_check(h);
    if (h->form->finish)
        h->form->finish(h);
}
