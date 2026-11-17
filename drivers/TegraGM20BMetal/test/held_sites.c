// SPDX-License-Identifier: MIT
// Every allocation and free of every thread between held_sites_start and held_sites_stop, each allocation with the
// return addresses of its frames, logged through libmalloc's malloc_logger into storage made before the first start, so
// the hook never allocates. held_sites.h reads the log back as the blocks still held and where each was allocated.
#include "held_sites_log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/mman.h>

typedef void(malloc_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
                              uint32_t num_hot_frames_to_skip);
extern malloc_logger_t *malloc_logger;

enum {
    kAllocate = 2,
    kDeallocate = 4,
    kEvents = 1 << 18
};

static struct held_sites_event *events;
static _Atomic uint64_t used;
static _Atomic int overflowed;

static uint32_t walk(uintptr_t *out)
{
    pthread_t self = pthread_self();
    const uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    const uintptr_t bottom = top - pthread_get_stacksize_np(self);
    const uintptr_t *fp = __builtin_frame_address(0);
    uint32_t n = 0;
    while (n < kHeldSitesFrames && (uintptr_t)fp >= bottom && (uintptr_t)fp + 16 <= top && !((uintptr_t)fp & 7)) {
        // a return address arm64e code saved is signed above the 47 bits of the user address space
        const uintptr_t lr = fp[1] & 0x00007fffffffffffull;
        if (!lr)
            break;
        out[n++] = lr;
        const uintptr_t *next = (const uintptr_t *)fp[0];
        if (next <= fp)
            break;
        fp = next;
    }
    return n;
}

// malloc: (zone, size, 0, block); calloc: (zone, count * size, 0, block); free: (zone, block, 0, 0); realloc:
// (zone, old block, new size, new block)
static void logBlocks(uint32_t type, uintptr_t zone, uintptr_t a2, uintptr_t a3, uintptr_t result, uint32_t skip)
{
    (void)zone, (void)skip;
    if (!(type & (kAllocate | kDeallocate)))
        return;
    const uint64_t at = atomic_fetch_add_explicit(&used, 1, memory_order_relaxed);
    if (at >= kEvents) {
        atomic_store_explicit(&overflowed, 1, memory_order_relaxed);
        return;
    }
    struct held_sites_event *e = &events[at];
    e->freed = type & kDeallocate ? a2 : 0;
    e->allocated = type & kAllocate ? result : 0;
    e->size = !(type & kAllocate) ? 0 : type & kDeallocate ? a3 : a2;
    e->frames = type & kAllocate ? walk(e->frame) : 0;
    __atomic_store_n(&e->ready, 1, __ATOMIC_RELEASE);
}

void held_sites_start(void)
{
    if (!events)
        events = mmap(NULL, sizeof(struct held_sites_event) * kEvents, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
                      -1, 0);
    for (uint64_t i = 0, n = atomic_load(&used); i < n && i < kEvents; i++)
        __atomic_store_n(&events[i].ready, 0, __ATOMIC_RELAXED);
    atomic_store(&used, 0);
    atomic_store(&overflowed, 0);
    atomic_thread_fence(memory_order_seq_cst);
    malloc_logger = logBlocks;
}

const struct held_sites_event *held_sites_stop(size_t *count, bool *overflow)
{
    malloc_logger = 0;
    atomic_thread_fence(memory_order_seq_cst);
    const uint64_t n = atomic_load(&used);
    *count = n < kEvents ? (size_t)n : kEvents;
    *overflow = atomic_load(&overflowed) != 0;
    return events;
}
