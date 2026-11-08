/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: spin locks and the local interrupt mask, kits/SwitchLinux's. The doorbell filter only
 * acknowledges the LIC and mail.c's handler runs on a work loop thread, so nothing here runs in primary interrupt
 * context. */
#ifndef BPXNU_LINUX_SPINLOCK_H
#define BPXNU_LINUX_SPINLOCK_H
#include <bpxnu.h>
typedef switch_linux_spinlock spinlock_t;
typedef switch_linux_spinlock raw_spinlock_t;
#define DEFINE_SPINLOCK(x) spinlock_t x = SWITCH_LINUX_SPINLOCK_INIT
#define DEFINE_RAW_SPINLOCK(x) raw_spinlock_t x = SWITCH_LINUX_SPINLOCK_INIT
#define spin_lock_init(l) do { (l)->holder = 0; } while (0)
#define spin_lock(l) switch_linux_spin_lock(l)
#define spin_unlock(l) switch_linux_spin_unlock(l)
#define spin_lock_irqsave(l, flags) ((flags) = switch_linux_spin_lock_irqsave(l))
#define spin_unlock_irqrestore(l, flags) switch_linux_spin_unlock_irqrestore((l), (flags))
#define raw_spin_lock(l) switch_linux_spin_lock(l)
#define raw_spin_unlock(l) switch_linux_spin_unlock(l)
#define local_irq_save(flags) ((flags) = switch_linux_irq_save())
#define local_irq_restore(flags) switch_linux_irq_restore(flags)
#endif
