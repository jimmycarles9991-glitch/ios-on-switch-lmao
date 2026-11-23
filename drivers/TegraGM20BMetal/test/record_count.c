// SPDX-License-Identifier: MIT
// What one thread does while it records (record_cost_test): its Objective-C messages, locks, allocations, weak
// reference calls (each takes the runtime's side-table lock, which the lock interposers cannot see), retains and
// releases, and work it hands to a dispatch queue, counted by interposers in this library, which the test links so dyld applies them at launch. Only the thread
// that called record_count_start counts, so a completion or a queue thread of the facade never adds to it.
#include <dispatch/dispatch.h>
#include <malloc/malloc.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct objc_object *id;

#define DYLD_INTERPOSE(_replacement, _replacee)                                 \
    __attribute__((used)) static struct {                                       \
        const void *replacement;                                                \
        const void *replacee;                                                   \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee};

struct record_count {
    uint64_t sends, locks, mallocs, frees, weaks, retains, dispatches;
};

// the counting thread's TPIDRRO_EL0 with its low bits clear (0: none), which the message trampoline compares
uintptr_t record_count_thread;
uint64_t record_count_sends;
static uint64_t locks, mallocs, frees, weaks, retains, dispatches;

// each counted retain's and release's caller (a release's with the top bit set), the first kRetainSites of a window
enum {
    kRetainSites = 1 << 14
};

static uintptr_t retainSites[kRetainSites];

static void countRetain(uintptr_t caller)
{
    if (retains < kRetainSites)
        retainSites[retains] = caller;
    retains++;
}

static uintptr_t threadWord(void)
{
    uintptr_t word;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(word));
    return word & ~(uintptr_t)7;
}

// Every other thread calls the interposers too, so a counter is only written behind this test: an `x += counting()` on
// another thread writes back what it read and loses the counting thread's increments in between
static bool counting(void)
{
    return record_count_thread && threadWord() == record_count_thread;
}

__asm__(".globl _record_count_msgSend\n"
        ".p2align 2\n"
        "_record_count_msgSend:\n"
        "    adrp x16, _record_count_thread@PAGE\n"
        "    ldr x9, [x16, _record_count_thread@PAGEOFF]\n"
        "    cbz x9, 1f\n"
        "    mrs x10, TPIDRRO_EL0\n"
        "    and x10, x10, #~7\n"
        "    cmp x9, x10\n"
        "    b.ne 1f\n"
        "    adrp x16, _record_count_sends@PAGE\n"
        "    add x16, x16, _record_count_sends@PAGEOFF\n"
        "    ldr x9, [x16]\n"
        "    add x9, x9, #1\n"
        "    str x9, [x16]\n"
        "1:\n"
        "    b _objc_msgSend\n");

extern void objc_msgSend(void);
extern void record_count_msgSend(void);
DYLD_INTERPOSE(record_count_msgSend, objc_msgSend)

static int countedMutexLock(pthread_mutex_t *m)
{
    if (counting())
        locks++;
    return pthread_mutex_lock(m);
}
DYLD_INTERPOSE(countedMutexLock, pthread_mutex_lock)

static int countedMutexTryLock(pthread_mutex_t *m)
{
    if (counting())
        locks++;
    return pthread_mutex_trylock(m);
}
DYLD_INTERPOSE(countedMutexTryLock, pthread_mutex_trylock)

static void countedUnfairLock(os_unfair_lock_t l)
{
    if (counting())
        locks++;
    os_unfair_lock_lock(l);
}
DYLD_INTERPOSE(countedUnfairLock, os_unfair_lock_lock)

static bool countedUnfairTryLock(os_unfair_lock_t l)
{
    if (counting())
        locks++;
    return os_unfair_lock_trylock(l);
}
DYLD_INTERPOSE(countedUnfairTryLock, os_unfair_lock_trylock)

extern id objc_loadWeakRetained(id *location);
extern id objc_loadWeak(id *location);
extern id objc_storeWeak(id *location, id object);
extern id objc_initWeak(id *location, id object);
extern void objc_destroyWeak(id *location);
extern void objc_copyWeak(id *to, id *from);
extern void objc_moveWeak(id *to, id *from);
extern id objc_retain(id object);
extern void objc_release(id object);

static id countedLoadWeakRetained(id *location)
{
    if (counting())
        weaks++;
    return objc_loadWeakRetained(location);
}
DYLD_INTERPOSE(countedLoadWeakRetained, objc_loadWeakRetained)

static id countedLoadWeak(id *location)
{
    if (counting())
        weaks++;
    return objc_loadWeak(location);
}
DYLD_INTERPOSE(countedLoadWeak, objc_loadWeak)

static id countedStoreWeak(id *location, id object)
{
    if (counting())
        weaks++;
    return objc_storeWeak(location, object);
}
DYLD_INTERPOSE(countedStoreWeak, objc_storeWeak)

static id countedInitWeak(id *location, id object)
{
    if (counting())
        weaks++;
    return objc_initWeak(location, object);
}
DYLD_INTERPOSE(countedInitWeak, objc_initWeak)

static void countedDestroyWeak(id *location)
{
    if (counting())
        weaks++;
    objc_destroyWeak(location);
}
DYLD_INTERPOSE(countedDestroyWeak, objc_destroyWeak)

static void countedCopyWeak(id *to, id *from)
{
    if (counting())
        weaks++;
    objc_copyWeak(to, from);
}
DYLD_INTERPOSE(countedCopyWeak, objc_copyWeak)

static void countedMoveWeak(id *to, id *from)
{
    if (counting())
        weaks++;
    objc_moveWeak(to, from);
}
DYLD_INTERPOSE(countedMoveWeak, objc_moveWeak)

static id countedRetain(id object)
{
    if (counting())
        countRetain((uintptr_t)__builtin_return_address(0));
    return objc_retain(object);
}
DYLD_INTERPOSE(countedRetain, objc_retain)

static void countedRelease(id object)
{
    if (counting())
        countRetain((uintptr_t)__builtin_return_address(0) | (uintptr_t)1 << 63);
    objc_release(object);
}
DYLD_INTERPOSE(countedRelease, objc_release)

static void countedDispatchAsyncF(dispatch_queue_t queue, void *context, dispatch_function_t work)
{
    if (counting())
        dispatches++;
    dispatch_async_f(queue, context, work);
}
DYLD_INTERPOSE(countedDispatchAsyncF, dispatch_async_f)

static void countedDispatchAsync(dispatch_queue_t queue, dispatch_block_t block)
{
    if (counting())
        dispatches++;
    dispatch_async(queue, block);
}
DYLD_INTERPOSE(countedDispatchAsync, dispatch_async)

typedef void(malloc_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
                              uint32_t num_hot_frames_to_skip);
extern malloc_logger_t *malloc_logger;

static void countAllocations(uint32_t type, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uint32_t skip)
{
    (void)a, (void)b, (void)c, (void)d, (void)skip;

    enum {
        kAllocate = 2,
        kDeallocate = 4
    };

    if (!counting())
        return;
    mallocs += (type & kAllocate) != 0;
    frees += (type & kDeallocate) != 0;
}

void record_count_start(void)
{
    record_count_sends = locks = mallocs = frees = weaks = retains = dispatches = 0;
    malloc_logger = countAllocations;
    atomic_thread_fence(memory_order_seq_cst);
    record_count_thread = threadWord();
}

void record_count_stop(struct record_count *out)
{
    record_count_thread = 0;
    atomic_thread_fence(memory_order_seq_cst);
    malloc_logger = 0;
    out->sends = record_count_sends;
    out->locks = locks;
    out->mallocs = mallocs;
    out->frees = frees;
    out->weaks = weaks;
    out->retains = retains;
    out->dispatches = dispatches;
}

// The last window's retain and release callers (record_count_stop's `retains` of them, at most kRetainSites)
const uintptr_t *record_count_retain_sites(size_t *count)
{
    *count = retains < kRetainSites ? (size_t)retains : kRetainSites;
    return retainSites;
}
