// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel as TegraSE.cpp sees it, for a host build: locks and sleeps on std::mutex/condition_variable, a wait that
// is woken only by a wakeup posted on the same thread (the register model completes an operation inside the write
// that starts it), and empty corecrypto types (no CPU reference: every CPU path answers "cannot").
#pragma once
#include <sys/types.h>
#include <time.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#define SWITCH_DCACHE_OP(op, address) ((void)(op), (void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)

using IOReturn = int32_t;
using IOOptionBits = uint32_t;
using vm_offset_t = uintptr_t;
using boolean_t = int;
using kern_return_t = int;
using wait_result_t = int;
using wait_interrupt_t = int;
using event_t = void *;
using task_t = void *;
struct thread;
using thread_t = struct thread *;
using thread_continue_t = void (*)(void *, wait_result_t);

constexpr IOReturn kIOReturnSuccess = 0;
constexpr kern_return_t KERN_SUCCESS = 0, KERN_FAILURE = 5;
constexpr wait_interrupt_t THREAD_UNINT = 0;
constexpr wait_result_t THREAD_AWAKENED = 0, THREAD_TIMED_OUT = 1;
#define THREAD_CONTINUE_NULL ((thread_continue_t) nullptr)
constexpr uint32_t kMillisecondScale = 1000000;
#ifndef NSEC_PER_USEC
#define NSEC_PER_USEC 1000ull
#endif
#ifndef NSEC_PER_MSEC
#define NSEC_PER_MSEC 1000000ull
#endif

[[noreturn]] void panic(const char *format, ...) __attribute__((format(printf, 1, 2)));
void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
void IOSleep(unsigned ms);

inline void IODelay(unsigned)
{}

inline void *IOMalloc(size_t size)
{
    return malloc(size);
}

inline void IOFree(void *address, size_t)
{
    free(address);
}

// Absolute time is nanoseconds here.
inline uint64_t mach_absolute_time()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs)
{
    *abs = ns;
}

inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs;
}

inline void clock_interval_to_deadline(uint32_t interval, uint64_t scale, uint64_t *deadline)
{
    *deadline = mach_absolute_time() + interval * scale;
}

thread_t current_thread();
void thread_terminate(thread_t thread);
wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t interruptible, uint64_t deadline);
wait_result_t thread_block(thread_continue_t continuation);
void thread_wakeup(event_t event);

extern "C" {
boolean_t ml_at_interrupt_context(void);
boolean_t ml_get_interrupts_enabled(void);
vm_offset_t ml_vtophys(vm_offset_t va);
}

struct IOLock {
    std::mutex mutex;
    std::condition_variable changed;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->mutex.lock();
}

inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
    lock->mutex.unlock();
}

inline void IOLockSleep(IOLock *lock, void *, int)
{
    std::unique_lock held(lock->mutex, std::adopt_lock);
    lock->changed.wait(held);
    held.release();
}

inline int IOLockSleepDeadline(IOLock *lock, void *, uint64_t deadline, int)
{
    const uint64_t now = mach_absolute_time();
    std::unique_lock held(lock->mutex, std::adopt_lock);
    bool woke = false;
    if (deadline > now)
        woke = lock->changed.wait_for(held, std::chrono::nanoseconds(deadline - now)) == std::cv_status::no_timeout;
    held.release();
    return woke ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}

inline void IOLockWakeup(IOLock *lock, void *, bool)
{
    lock->changed.notify_all();
}

class OSObject {
public:
    virtual ~OSObject() = default;
};

class IOService;
using IOInterruptAction = void (*)(OSObject *target, void *refCon, IOService *nub, int source);

class IOService : public OSObject {
public:
    IOReturn registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon = nullptr)
    {
        if (source != 0 || !handler)
            return -1;
        this->target = target;
        this->handler = handler;
        this->refCon = refCon;
        return kIOReturnSuccess;
    }

    IOReturn unregisterInterrupt(int source)
    {
        if (source != 0)
            return -1;
        target = nullptr;
        handler = nullptr;
        refCon = nullptr;
        enabled = false;
        return kIOReturnSuccess;
    }

    IOReturn enableInterrupt(int source)
    {
        if (source != 0 || !handler)
            return -1;
        enabled = true;
        return kIOReturnSuccess;
    }

    IOReturn disableInterrupt(int source)
    {
        if (source != 0)
            return -1;
        enabled = false;
        return kIOReturnSuccess;
    }

    void fire()
    {
        if (enabled && handler)
            handler(target, refCon, this, 0);
    }

    OSObject *target = nullptr;
    IOInterruptAction handler = nullptr;
    void *refCon = nullptr;
    bool enabled = false;
};

class IOBufferMemoryDescriptor : public OSObject {};

#define OS_LOG_DEFAULT   nullptr
#define os_log(log, ...) IOLog(__VA_ARGS__)

// corecrypto and libkern/crypto as TegraSE.cpp names them; g_crypto_funcs stays null.
typedef struct ccdigest_state *ccdigest_state_t;

struct ccdigest_info {
    size_t output_size, state_size, block_size, oid_size;
    const unsigned char *oid;
    const void *initial_state;
    void (*compress)(ccdigest_state_t state, size_t nblocks, const void *data);
};
struct ccctr_ctx;
struct ccecb_ctx;

struct ccmode_ctr {
    size_t size;
    int (*init)(const ccmode_ctr *mode, ccctr_ctx *ctx, size_t keyBytes, const void *key, const void *iv);
    int (*ctr)(ccctr_ctx *ctx, size_t bytes, const void *in, void *out);
};

struct ccmode_ecb {
    size_t size;
    int (*init)(const ccmode_ecb *mode, ccecb_ctx *ctx, size_t keyBytes, const void *key);
    int (*ecb)(const ccecb_ctx *ctx, size_t nblocks, const void *in, void *out);
};

struct crypto_functions {
    const ccmode_ctr *ccaes_ctr_crypt;
    const ccmode_ecb *ccaes_ecb_encrypt, *ccaes_ecb_decrypt;
};

inline const crypto_functions *g_crypto_funcs = nullptr;

struct aes_encrypt_ctx {
    uint8_t opaque[256];
};

struct aes_decrypt_ctx {
    uint8_t opaque[256];
};

inline int aes_encrypt_key(const uint8_t *, int, aes_encrypt_ctx *)
{
    return -1;
}

inline int aes_decrypt_key(const uint8_t *, int, aes_decrypt_ctx *)
{
    return -1;
}

inline int aes_encrypt_cbc(const uint8_t *, const uint8_t *, unsigned, uint8_t *, aes_encrypt_ctx *)
{
    return -1;
}

inline int aes_decrypt_cbc(const uint8_t *, const uint8_t *, unsigned, uint8_t *, aes_decrypt_ctx *)
{
    return -1;
}
