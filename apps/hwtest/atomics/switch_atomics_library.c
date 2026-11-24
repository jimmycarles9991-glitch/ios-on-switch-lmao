/* The same operations through Apple's entry points that hold them in the dyld cache: on iOS 27 these run the cache
 * plan's stubs (placed in dead PAC checks, B or BL to a shared copy), on iOS 17 Apple's own ARMv8.0 code. */
#include "switch_atomics_internal.h"

#include <libkern/OSAtomic.h>
#include <os/lock.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static struct switch_atomic_ret old_value(uint64_t old)
{
    return (struct switch_atomic_ret){old, 0};
}

#define ADDER(name, call, type)                                           \
    static struct switch_atomic_ret name(void *p, uint64_t a, uint64_t b) \
    {                                                                     \
        (void)b;                                                          \
        return old_value((u##type)(call((type)a, p) - (type)a));          \
    }

#define INCREMENTER(name, call, type)                                     \
    static struct switch_atomic_ret name(void *p, uint64_t a, uint64_t b) \
    {                                                                     \
        (void)a;                                                          \
        (void)b;                                                          \
        return old_value((u##type)(call(p) - 1));                         \
    }

#define BITWISE(name, call, operand)                                      \
    static struct switch_atomic_ret name(void *p, uint64_t a, uint64_t b) \
    {                                                                     \
        (void)b;                                                          \
        return old_value((uint32_t)call(operand, p));                     \
    }

/* A failed compare answers with a value other than the one compared, as a CAS's old value would. */
#define COMPARER(name, call, type)                                        \
    static struct switch_atomic_ret name(void *p, uint64_t a, uint64_t b) \
    {                                                                     \
        return old_value(call((type)a, (type)b, p) ? a : a + 1);          \
    }

/* A lock's take (a == 0) and drop (a == the holder's token), each answering as the CAS lock's would. */
#define BIT_LOCK(name, take, drop)                                              \
    static struct switch_atomic_ret name(void *p, uint64_t a, uint64_t b)       \
    {                                                                           \
        (void)b;                                                                \
        return old_value(a == 0 ? (take(0, p) ? 1 : 0) : (drop(0, p) ? a : 0)); \
    }

ADDER(add32, OSAtomicAdd32, int32_t)
ADDER(add32_barrier, OSAtomicAdd32Barrier, int32_t)
ADDER(add64, OSAtomicAdd64, int64_t)
ADDER(add64_barrier, OSAtomicAdd64Barrier, int64_t)
INCREMENTER(increment32, OSAtomicIncrement32, int32_t)
INCREMENTER(increment32_barrier, OSAtomicIncrement32Barrier, int32_t)
INCREMENTER(increment64, OSAtomicIncrement64, int64_t)
INCREMENTER(increment64_barrier, OSAtomicIncrement64Barrier, int64_t)
BITWISE(or32_orig, OSAtomicOr32Orig, (uint32_t)a)
BITWISE(or32_orig_barrier, OSAtomicOr32OrigBarrier, (uint32_t)a)
BITWISE(and32_orig, OSAtomicAnd32Orig, ~(uint32_t)a)
BITWISE(and32_orig_barrier, OSAtomicAnd32OrigBarrier, ~(uint32_t)a)
BITWISE(xor32_orig, OSAtomicXor32Orig, (uint32_t)a)
BITWISE(xor32_orig_barrier, OSAtomicXor32OrigBarrier, (uint32_t)a)
COMPARER(cas32, OSAtomicCompareAndSwap32, int32_t)
COMPARER(cas32_barrier, OSAtomicCompareAndSwap32Barrier, int32_t)
COMPARER(cas64, OSAtomicCompareAndSwap64, int64_t)
COMPARER(cas64_barrier, OSAtomicCompareAndSwap64Barrier, int64_t)
COMPARER(cas_ptr, OSAtomicCompareAndSwapPtr, void *)
COMPARER(cas_ptr_barrier, OSAtomicCompareAndSwapPtrBarrier, void *)
BIT_LOCK(test_and_set, OSAtomicTestAndSet, OSAtomicTestAndClear)
BIT_LOCK(test_and_set_barrier, OSAtomicTestAndSetBarrier, OSAtomicTestAndClearBarrier)

static struct switch_atomic_ret unfair(void *p, uint64_t a, uint64_t b)
{
    (void)b;
    if (a == 0)
        os_unfair_lock_lock(p);
    else
        os_unfair_lock_unlock(p);
    return old_value(a);
}

static struct switch_atomic_ret mutex(void *p, uint64_t a, uint64_t b)
{
    (void)b;
    if (a == 0)
        pthread_mutex_lock(p);
    else
        pthread_mutex_unlock(p);
    return old_value(a);
}

static void mutex_prepare(struct atomics_hammer *h)
{
    pthread_mutex_init(h->target, NULL);
}

static void mutex_finish(struct atomics_hammer *h)
{
    pthread_mutex_destroy(h->target);
}

static void queue_prepare(struct atomics_hammer *h)
{
    *(OSQueueHead *)h->target = (OSQueueHead)OS_ATOMIC_QUEUE_INIT;
    for (unsigned n = 0; n < ATOMICS_NODES; n++)
        OSAtomicEnqueue(h->target, &h->nodes[n], offsetof(struct atomics_node, link));
}

static void queue_work(struct atomics_hammer *h, unsigned t)
{
    (void)t;
    for (uint64_t i = 0; i < h->ops; i++) {
        struct atomics_node *node;
        while (!(node = OSAtomicDequeue(h->target, offsetof(struct atomics_node, link))))
            sched_yield();
        if (switch_atomics_v80_swap(&node->owned, 1))
            atomics_violation(h);
        *(volatile uint64_t *)&node->count = node->count + 1;
        switch_atomics_v80_store(&node->owned, 0);
        OSAtomicEnqueue(h->target, node, offsetof(struct atomics_node, link));
    }
}

/* Every node dequeued once more, and every dequeue counted on its node. */
static void queue_check(struct atomics_hammer *h)
{
    bool seen[ATOMICS_NODES] = {false};
    uint64_t nodes = 0, pops = 0;
    struct atomics_node *node;
    while ((node = OSAtomicDequeue(h->target, offsetof(struct atomics_node, link)))) {
        const ptrdiff_t n = node - h->nodes;
        if (n < 0 || n >= ATOMICS_NODES || seen[n]) {
            atomics_violation(h);
            return;
        }
        seen[n] = true;
        nodes++;
        pops += node->count;
    }
    if (nodes != ATOMICS_NODES || pops != h->ops * h->threads)
        atomics_violation(h);
}

#define COUNT(form, call, width) {.name = form, .kind = KIND_COUNT, .bits = width, .has_old = true, .fn = call}
#define OWN_BIT(form, how, call) {.name = form, .kind = how, .bits = 32, .has_old = true, .fn = call}
#define LOCK(form, call, width, ordered) \
    {.name = form,                       \
     .kind = KIND_LOCK,                  \
     .bits = width,                      \
     .acquire = ordered,                 \
     .release = ordered,                 \
     .has_old = true,                    \
     .fn = call}

const struct atomics_form atomics_library[] = {
    COUNT("osatomic_add32", add32, 32),
    COUNT("osatomic_add32_barrier", add32_barrier, 32),
    COUNT("osatomic_add64", add64, 64),
    COUNT("osatomic_add64_barrier", add64_barrier, 64),
    COUNT("osatomic_increment32", increment32, 32),
    COUNT("osatomic_increment32_barrier", increment32_barrier, 32),
    COUNT("osatomic_increment64", increment64, 64),
    COUNT("osatomic_increment64_barrier", increment64_barrier, 64),
    OWN_BIT("osatomic_or32_orig", KIND_SET, or32_orig),
    OWN_BIT("osatomic_or32_orig_barrier", KIND_SET, or32_orig_barrier),
    OWN_BIT("osatomic_and32_orig", KIND_CLEAR, and32_orig),
    OWN_BIT("osatomic_and32_orig_barrier", KIND_CLEAR, and32_orig_barrier),
    OWN_BIT("osatomic_xor32_orig", KIND_TOGGLE, xor32_orig),
    OWN_BIT("osatomic_xor32_orig_barrier", KIND_TOGGLE, xor32_orig_barrier),
    LOCK("osatomic_cas32", cas32, 32, false),
    LOCK("osatomic_cas32_barrier", cas32_barrier, 32, true),
    LOCK("osatomic_cas64", cas64, 64, false),
    LOCK("osatomic_cas64_barrier", cas64_barrier, 64, true),
    LOCK("osatomic_cas_ptr", cas_ptr, 64, false),
    LOCK("osatomic_cas_ptr_barrier", cas_ptr_barrier, 64, true),
    LOCK("osatomic_test_and_set", test_and_set, 8, false),
    LOCK("osatomic_test_and_set_barrier", test_and_set_barrier, 8, true),
    LOCK("os_unfair_lock", unfair, 32, true),
    {.name = "pthread_mutex",
     .kind = KIND_LOCK,
     .bits = 64,
     .acquire = true,
     .release = true,
     .has_old = true,
     .fn = mutex,
     .prepare = mutex_prepare,
     .finish = mutex_finish},
    {.name = "osatomic_enqueue_dequeue",
     .bits = 64,
     .prepare = queue_prepare,
     .work = queue_work,
     .check = queue_check},
};

const unsigned atomics_library_count = sizeof(atomics_library) / sizeof(atomics_library[0]);
