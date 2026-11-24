// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <kern/locks.h>
#include <kern/thread.h>

struct turnstile;

/*
 * User-promote turnstile wait primitive (TURNSTILE_EPOCH_USER -> TURNSTILE_USER_PROMOTE).
 * Unlike lck_mtx_sleep_with_inheritor (TURNSTILE_KERNEL_PROMOTE), the inheritor is a
 * userspace thread that returns to user mode while holding the push; XNU raises its
 * base_pri through base_inheritor_queue / user_promotion_basepri, bypassing daemon QoS
 * ceilings and propagating through userspace locks.
 */
struct switch_user_turnstile {
    struct turnstile *turnstile;
};

#ifdef __cplusplus
extern "C" {
#endif

int switch_user_turnstile_sleep(lck_mtx_t *mtx, struct switch_user_turnstile *ut, thread_t inheritor,
                                int interruptible);
void switch_user_turnstile_retarget(struct switch_user_turnstile *ut, thread_t inheritor);
void switch_user_turnstile_wakeup_all(struct switch_user_turnstile *ut, int result);
void switch_user_turnstile_thread_retain(thread_t thread);
void switch_user_turnstile_thread_drop(thread_t thread);

#ifdef __cplusplus
}
#endif
