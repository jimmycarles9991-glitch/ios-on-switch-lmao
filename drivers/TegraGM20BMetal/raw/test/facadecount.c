// SPDX-License-Identifier: MIT
// The raw facade's own Objective-C messages, locks and allocations, on every thread (raw/test/facadecost.mm): a dylib
// whose interposers count a call when the facade's image made it. A message counts where objc_msgSend returns into the
// facade (its link register); a lock or an allocation where a return address of its first frames lies in the facade,
// so std::mutex::lock and operator new count for the facade code that called them, unless the mock kernel's image lies
// between (its own locks and allocations, the kernel's on a device: facadecount_mock). The facade is its own image.
// Each count's facade return address also goes into a ring (facadecount_sites), which facadecost names by symbol.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <malloc/malloc.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                 \
    __attribute__((used)) static struct {                                       \
        const void *replacement;                                                \
        const void *replacee;                                                   \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee};

struct facadecount {
    uint64_t sends, locks, mallocs, frees;
};

// [start, end) of the facade's __TEXT: read by the message trampoline below, so its layout is fixed
uintptr_t facadecount_text[2];
_Atomic uint64_t facadecount_sends;
static _Atomic uint64_t locks, mallocs, frees, mockLocks, mockMallocs;
// [start, end) of the mock kernel's __TEXT (facadecount_mock_image): what it does under a facade call is the kernel's
static uintptr_t mockText[2];
static _Atomic int counting;

// the last kSites counted calls' facade return addresses, each tagged with its kind in the top byte
enum {
    kSites = 1 << 16,
    kSend = 1,
    kLock = 2,
    kMalloc = 3
};

uintptr_t facadecount_ring[kSites];
_Atomic uint64_t facadecount_ringAt;

static void site(uintptr_t lr, uintptr_t kind)
{
    const uint64_t at = atomic_fetch_add_explicit(&facadecount_ringAt, 1, memory_order_relaxed);
    facadecount_ring[at & (kSites - 1)] = lr | kind << 56;
}

// The facade return address of the first frames, and whether the mock kernel's image is between it and the call
static uintptr_t fromFacade(const void *frame, int depth, bool *mock)
{
    const uintptr_t *fp = frame;
    *mock = false;
    for (int i = 0; i < depth && fp && !((uintptr_t)fp & 7); i++) {
        const uintptr_t lr = fp[1];
        if (lr >= facadecount_text[0] && lr < facadecount_text[1])
            return lr;
        *mock |= lr >= mockText[0] && lr < mockText[1];
        fp = (const uintptr_t *)fp[0];
    }
    return 0;
}

// objc_msgSend with the argument registers untouched: x9-x17 are free at a call
__asm__(".globl _facadecount_msgSend\n"
        ".p2align 2\n"
        "_facadecount_msgSend:\n"
        "    adrp x16, _facadecount_text@PAGE\n"
        "    add x16, x16, _facadecount_text@PAGEOFF\n"
        "    ldp x9, x10, [x16]\n"
        "    cmp x30, x9\n"
        "    b.lo 1f\n"
        "    cmp x30, x10\n"
        "    b.hs 1f\n"
        "    adrp x16, _facadecount_sends@PAGE\n"
        "    add x16, x16, _facadecount_sends@PAGEOFF\n"
        "    mov x17, #1\n"
        "    stadd x17, [x16]\n"
        "    adrp x16, _facadecount_ringAt@PAGE\n"
        "    add x16, x16, _facadecount_ringAt@PAGEOFF\n"
        "    ldadd x17, x9, [x16]\n"
        "    and x9, x9, #0xffff\n"
        "    adrp x16, _facadecount_ring@PAGE\n"
        "    add x16, x16, _facadecount_ring@PAGEOFF\n"
        "    orr x10, x30, #(1 << 56)\n"
        "    str x10, [x16, x9, lsl #3]\n"
        "1:\n"
        "    b _objc_msgSend\n");

extern void objc_msgSend(void);
extern void facadecount_msgSend(void);
DYLD_INTERPOSE(facadecount_msgSend, objc_msgSend)

static void noteLock(void)
{
    if (!atomic_load_explicit(&counting, memory_order_relaxed))
        return;
    bool mock;
    const uintptr_t lr = fromFacade(__builtin_frame_address(0), 4, &mock);
    if (lr && mock)
        atomic_fetch_add_explicit(&mockLocks, 1, memory_order_relaxed);
    else if (lr) {
        atomic_fetch_add_explicit(&locks, 1, memory_order_relaxed);
        site(lr, kLock);
    }
}

static int countedMutexLock(pthread_mutex_t *m)
{
    noteLock();
    return pthread_mutex_lock(m);
}
DYLD_INTERPOSE(countedMutexLock, pthread_mutex_lock)

static int countedMutexTryLock(pthread_mutex_t *m)
{
    noteLock();
    return pthread_mutex_trylock(m);
}
DYLD_INTERPOSE(countedMutexTryLock, pthread_mutex_trylock)

static void countedUnfairLock(os_unfair_lock_t l)
{
    noteLock();
    os_unfair_lock_lock(l);
}
DYLD_INTERPOSE(countedUnfairLock, os_unfair_lock_lock)

static bool countedUnfairTryLock(os_unfair_lock_t l)
{
    noteLock();
    return os_unfair_lock_trylock(l);
}
DYLD_INTERPOSE(countedUnfairTryLock, os_unfair_lock_trylock)

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

    bool mock = false;
    const uintptr_t lr = type & (kAllocate | kDeallocate) ? fromFacade(__builtin_frame_address(0), 24, &mock) : 0;
    if (!lr)
        return;
    if (mock) {
        if (type & kAllocate)
            atomic_fetch_add_explicit(&mockMallocs, 1, memory_order_relaxed);
        return;
    }
    if (type & kAllocate) {
        atomic_fetch_add_explicit(&mallocs, 1, memory_order_relaxed);
        site(lr, kMalloc);
    }
    if (type & kDeallocate)
        atomic_fetch_add_explicit(&frees, 1, memory_order_relaxed);
}

// The facade's image: the one that holds `inside`
void facadecount_image(const void *inside)
{
    Dl_info info;
    if (!dladdr(inside, &info))
        return;
    unsigned long size = 0;
    const uint8_t *text = getsegmentdata((const struct mach_header_64 *)info.dli_fbase, "__TEXT", &size);
    facadecount_text[0] = (uintptr_t)info.dli_fbase;
    facadecount_text[1] = (uintptr_t)(text ? text : (const uint8_t *)info.dli_fbase) + size;
}

// The mock kernel's image: the one that holds `inside`
void facadecount_mock_image(const void *inside)
{
    Dl_info info;
    if (!dladdr(inside, &info))
        return;
    unsigned long size = 0;
    const uint8_t *text = getsegmentdata((const struct mach_header_64 *)info.dli_fbase, "__TEXT", &size);
    mockText[0] = (uintptr_t)info.dli_fbase;
    mockText[1] = (uintptr_t)(text ? text : (const uint8_t *)info.dli_fbase) + size;
}

// The locks and allocations the mock kernel made under facade calls since facadecount_start
void facadecount_mock(uint64_t *mockLocksOut, uint64_t *mockMallocsOut)
{
    *mockLocksOut = atomic_load(&mockLocks);
    *mockMallocsOut = atomic_load(&mockMallocs);
}

void facadecount_start(void)
{
    atomic_store(&facadecount_sends, 0);
    atomic_store(&facadecount_ringAt, 0);
    atomic_store(&locks, 0);
    atomic_store(&mallocs, 0);
    atomic_store(&frees, 0);
    atomic_store(&mockLocks, 0);
    atomic_store(&mockMallocs, 0);
    malloc_logger = countAllocations;
    atomic_store(&counting, 1);
}

void facadecount_stop(struct facadecount *out)
{
    atomic_store(&counting, 0);
    malloc_logger = 0;
    out->sends = atomic_load(&facadecount_sends);
    out->locks = atomic_load(&locks);
    out->mallocs = atomic_load(&mallocs);
    out->frees = atomic_load(&frees);
}

// The ring's counted calls of `kind` (1 messages, 2 locks, 3 allocations) by facade return address, most first: `out`
// holds address and count pairs, the call returns how many pairs it wrote
size_t facadecount_sites(unsigned kind, uintptr_t *out, size_t pairs)
{
    const uint64_t taken = atomic_load(&facadecount_ringAt);
    const size_t live = taken < kSites ? (size_t)taken : kSites;
    size_t n = 0;
    for (size_t i = 0; i < live; i++) {
        const uintptr_t entry = facadecount_ring[i];
        if ((entry >> 56) != kind)
            continue;
        const uintptr_t lr = entry & ((1ull << 56) - 1);
        size_t k = 0;
        while (k < n && out[2 * k] != lr)
            k++;
        if (k == n) {
            if (n == pairs)
                continue;
            out[2 * n] = lr;
            out[2 * n + 1] = 0;
            n++;
        }
        out[2 * k + 1]++;
    }
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && out[2 * j + 1] > out[2 * j - 1]; j--) {
            const uintptr_t a = out[2 * j], c = out[2 * j + 1];
            out[2 * j] = out[2 * j - 2];
            out[2 * j + 1] = out[2 * j - 1];
            out[2 * j - 2] = a;
            out[2 * j - 1] = c;
        }
    return n;
}
