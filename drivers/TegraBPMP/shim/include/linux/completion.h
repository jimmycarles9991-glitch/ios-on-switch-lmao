/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: completions (shim/linux.c), woken by event, never polled. */
#ifndef BPXNU_LINUX_COMPLETION_H
#define BPXNU_LINUX_COMPLETION_H
/* The GM20B shim (same kernel) defines Linux names too: every function here is bpmp_linux_-prefixed. */
#define init_completion(a0) bpmp_linux_init_completion(a0)
#define complete(a0) bpmp_linux_complete(a0)
#define wait_for_completion_timeout(a0, a1) bpmp_linux_wait_for_completion_timeout(a0, a1)
#include <linux/spinlock.h>
struct completion {
	spinlock_t lock;
	unsigned int done;
	unsigned int waiters;
};
void init_completion(struct completion *c);
void complete(struct completion *c);
unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout);
#endif
