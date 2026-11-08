// SPDX-License-Identifier: BSD-2-Clause-Patent
// Linux's spinlock_t and rwlock_t for the Linux-compat shims: a word inside the lock, so a zeroed lock (DEFINE_SPINLOCK)
// is an unlocked one and nothing is allocated or freed. As in Linux, every acquisition turns preemption off and the
// _irq/_irqsave forms mask this CPU's interrupts first; spin_unlock_irq unmasks them whatever they were. nvkm takes
// these inside its interrupt pass with interrupts off, where XNU forbids a sleeping lock (lck_rw, lck_mtx).
//
// A spinlock holds its holder's thread (TPIDR_EL1), so a holder that takes it again, or one that keeps it past XNU's
// spinlock timeout (LockTimeOut, 0.25 s), panics naming the lock and the thread instead of hanging the CPU, as lck_spin
// does. rwlock readers nest and a waiting writer does not hold new readers back: Linux lets a reader in interrupt
// context take a lock its own CPU already reads.
#ifndef SWITCH_LINUX_LOCK_H
#define SWITCH_LINUX_LOCK_H

#include <stdbool.h>
#include <stdint.h>
#include "SwitchLinux.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct switch_linux_spinlock {
    uintptr_t holder;
} switch_linux_spinlock;

typedef struct switch_linux_rwlock {
    uint32_t state;  // the readers inside, or SWITCH_LINUX_RW_WRITER
} switch_linux_rwlock;

#define SWITCH_LINUX_RW_WRITER       0x80000000u
#define SWITCH_LINUX_IRQS_WERE_ON    1ul
#define SWITCH_LINUX_SPINLOCK_INIT   {0}
#define SWITCH_LINUX_RWLOCK_INIT     {0}
#define SWITCH_LINUX_SPIN_TIMEOUT_NS 250000000ull

void _disable_preemption(void);  // NOLINT(bugprone-reserved-identifier): XNU's own name
void _enable_preemption(void);   // NOLINT(bugprone-reserved-identifier): XNU's own name

void switch_linux_spin_contended(switch_linux_spinlock *lock, uintptr_t self);
void switch_linux_read_contended(switch_linux_rwlock *lock);
void switch_linux_write_contended(switch_linux_rwlock *lock);

#ifdef SWITCH_LINUX_HOST_MODEL
uintptr_t switch_linux_model_thread(void);
#endif

static inline uintptr_t switch_linux_thread(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
#ifdef SWITCH_LINUX_HOST_MODEL
    return switch_linux_model_thread();
#else
    // NOLINTNEXTLINE(misc-const-correctness): written by the asm below
    uintptr_t thread = 0;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(thread));
    return thread;
#endif
}

static inline void switch_linux_spin_lock(switch_linux_spinlock *lock)
{
    _disable_preemption();
    const uintptr_t self = switch_linux_thread();
    uintptr_t unlocked = 0;
    if (!__atomic_compare_exchange_n(&lock->holder, &unlocked, self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        switch_linux_spin_contended(lock, self);
}

static inline bool switch_linux_spin_trylock(switch_linux_spinlock *lock)
{
    _disable_preemption();
    uintptr_t unlocked = 0;
    if (__atomic_compare_exchange_n(&lock->holder, &unlocked, switch_linux_thread(), false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
        return true;
    _enable_preemption();
    return false;
}

static inline void switch_linux_spin_unlock(switch_linux_spinlock *lock)
{
    __atomic_store_n(&lock->holder, (uintptr_t)0, __ATOMIC_RELEASE);
    _enable_preemption();
}

static inline bool switch_linux_spin_is_locked(const switch_linux_spinlock *lock)
{
    return __atomic_load_n(&lock->holder, __ATOMIC_RELAXED) != 0;
}

static inline unsigned long switch_linux_spin_lock_irqsave(switch_linux_spinlock *lock)
{
    const unsigned long flags = switch_linux_irq_save();
    switch_linux_spin_lock(lock);
    return flags;
}

static inline void switch_linux_spin_unlock_irqrestore(switch_linux_spinlock *lock, unsigned long flags)
{
    switch_linux_spin_unlock(lock);
    switch_linux_irq_restore(flags);
}

static inline void switch_linux_spin_lock_irq(switch_linux_spinlock *lock)
{
    (void)switch_linux_irq_save();
    switch_linux_spin_lock(lock);
}

static inline void switch_linux_spin_unlock_irq(switch_linux_spinlock *lock)
{
    switch_linux_spin_unlock(lock);
    switch_linux_irq_restore(SWITCH_LINUX_IRQS_WERE_ON);
}

static inline void switch_linux_read_lock(switch_linux_rwlock *lock)
{
    _disable_preemption();
    uint32_t seen = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
    if ((seen & SWITCH_LINUX_RW_WRITER) ||
        !__atomic_compare_exchange_n(&lock->state, &seen, seen + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        switch_linux_read_contended(lock);
}

static inline void switch_linux_read_unlock(switch_linux_rwlock *lock)
{
    __atomic_sub_fetch(&lock->state, 1u, __ATOMIC_RELEASE);
    _enable_preemption();
}

static inline void switch_linux_write_lock(switch_linux_rwlock *lock)
{
    _disable_preemption();
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&lock->state, &idle, SWITCH_LINUX_RW_WRITER, false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        switch_linux_write_contended(lock);
}

static inline void switch_linux_write_unlock(switch_linux_rwlock *lock)
{
    __atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
    _enable_preemption();
}

static inline unsigned long switch_linux_read_lock_irqsave(switch_linux_rwlock *lock)
{
    const unsigned long flags = switch_linux_irq_save();
    switch_linux_read_lock(lock);
    return flags;
}

static inline void switch_linux_read_unlock_irqrestore(switch_linux_rwlock *lock, unsigned long flags)
{
    switch_linux_read_unlock(lock);
    switch_linux_irq_restore(flags);
}

static inline unsigned long switch_linux_write_lock_irqsave(switch_linux_rwlock *lock)
{
    const unsigned long flags = switch_linux_irq_save();
    switch_linux_write_lock(lock);
    return flags;
}

static inline void switch_linux_write_unlock_irqrestore(switch_linux_rwlock *lock, unsigned long flags)
{
    switch_linux_write_unlock(lock);
    switch_linux_irq_restore(flags);
}

static inline void switch_linux_read_lock_irq(switch_linux_rwlock *lock)
{
    (void)switch_linux_irq_save();
    switch_linux_read_lock(lock);
}

static inline void switch_linux_read_unlock_irq(switch_linux_rwlock *lock)
{
    switch_linux_read_unlock(lock);
    switch_linux_irq_restore(SWITCH_LINUX_IRQS_WERE_ON);
}

static inline void switch_linux_write_lock_irq(switch_linux_rwlock *lock)
{
    (void)switch_linux_irq_save();
    switch_linux_write_lock(lock);
}

static inline void switch_linux_write_unlock_irq(switch_linux_rwlock *lock)
{
    switch_linux_write_unlock(lock);
    switch_linux_irq_restore(SWITCH_LINUX_IRQS_WERE_ON);
}

#ifdef __cplusplus
}
#endif

#endif
