/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: counting semaphore (shim/linux.c). `count` is read by mail.c's WARN_ON. */
#ifndef BPXNU_LINUX_SEMAPHORE_H
#define BPXNU_LINUX_SEMAPHORE_H
/* The GM20B shim (same kernel) defines Linux names too: every function here is bpmp_linux_-prefixed. */
#define sema_init(a0, a1) bpmp_linux_sema_init(a0, a1)
#define down_timeout(a0, a1) bpmp_linux_down_timeout(a0, a1)
#define up(a0) bpmp_linux_up(a0)
#include <linux/spinlock.h>
struct semaphore {
	spinlock_t lock;
	int count;
	unsigned int waiters;
};
void sema_init(struct semaphore *s, int count);
int down_timeout(struct semaphore *s, long timeout);
void up(struct semaphore *s);
#endif
