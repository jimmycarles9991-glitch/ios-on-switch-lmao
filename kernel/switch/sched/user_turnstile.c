// SPDX-License-Identifier: GPL-2.0-only
#include <switch/user_turnstile.h>

#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/turnstile.h>
#include <kern/waitq.h>

static LCK_GRP_DECLARE(user_turnstile_grp, "switch_user_turnstile");
static LCK_SPIN_DECLARE(user_turnstile_lock, &user_turnstile_grp);

int switch_user_turnstile_sleep(lck_mtx_t *mtx, struct switch_user_turnstile *ut, thread_t inheritor, int interruptible)
{
    struct turnstile *ts;
    wait_result_t wr;

    LCK_MTX_ASSERT(mtx, LCK_MTX_ASSERT_OWNED);

    if (inheritor == current_thread()) {
        inheritor = THREAD_NULL;
    }

    /* Acquire interlock before dropping mtx so retarget/wakeup cannot overtake waitq registration. */
    lck_spin_lock(&user_turnstile_lock);
    lck_mtx_unlock(mtx);

    /* Leave block_hint at kThreadWaitNone: XNU's kdp_sleep_with_inheritor_find_owner asserts
     * TURNSTILE_INHERITOR_THREAD, which is cleared whenever inheritor is THREAD_NULL. */
    ts = turnstile_prepare((uintptr_t)ut, &ut->turnstile, TURNSTILE_NULL, TURNSTILE_EPOCH_USER);
    turnstile_update_inheritor(ts, inheritor, TURNSTILE_DELAYED_UPDATE | TURNSTILE_INHERITOR_THREAD);
    wr = waitq_assert_wait64(&ts->ts_waitq, CAST_EVENT64_T(ut), (wait_interrupt_t)interruptible, TIMEOUT_WAIT_FOREVER);

    lck_spin_unlock(&user_turnstile_lock);

    turnstile_update_inheritor_complete(ts, TURNSTILE_INTERLOCK_NOT_HELD);

    if (wr == THREAD_WAITING) {
        wr = thread_block(THREAD_CONTINUE_NULL);
    }

    lck_spin_lock(&user_turnstile_lock);
    turnstile_complete((uintptr_t)ut, &ut->turnstile, NULL, TURNSTILE_EPOCH_USER);
    lck_spin_unlock(&user_turnstile_lock);

    turnstile_cleanup();
    lck_mtx_lock(mtx);
    return (int)wr;
}

void switch_user_turnstile_retarget(struct switch_user_turnstile *ut, thread_t inheritor)
{
    struct turnstile *ts;

    lck_spin_lock(&user_turnstile_lock);
    if (ut->turnstile == TURNSTILE_NULL) {
        lck_spin_unlock(&user_turnstile_lock);
        return;
    }
    ts = turnstile_prepare((uintptr_t)ut, &ut->turnstile, TURNSTILE_NULL, TURNSTILE_EPOCH_USER);
    turnstile_update_inheritor(ts, inheritor, TURNSTILE_IMMEDIATE_UPDATE | TURNSTILE_INHERITOR_THREAD);
    lck_spin_unlock(&user_turnstile_lock);

    turnstile_update_inheritor_complete(ts, TURNSTILE_INTERLOCK_NOT_HELD);

    lck_spin_lock(&user_turnstile_lock);
    turnstile_complete((uintptr_t)ut, &ut->turnstile, NULL, TURNSTILE_EPOCH_USER);
    lck_spin_unlock(&user_turnstile_lock);

    turnstile_cleanup();
}

void switch_user_turnstile_wakeup_all(struct switch_user_turnstile *ut, int result)
{
    struct turnstile *ts;

    lck_spin_lock(&user_turnstile_lock);
    if (ut->turnstile == TURNSTILE_NULL) {
        lck_spin_unlock(&user_turnstile_lock);
        return;
    }
    ts = turnstile_prepare((uintptr_t)ut, &ut->turnstile, TURNSTILE_NULL, TURNSTILE_EPOCH_USER);
    waitq_wakeup64_all(&ts->ts_waitq, CAST_EVENT64_T(ut), (wait_result_t)result, WAITQ_UPDATE_INHERITOR);
    lck_spin_unlock(&user_turnstile_lock);

    turnstile_update_inheritor_complete(ts, TURNSTILE_INTERLOCK_NOT_HELD);

    lck_spin_lock(&user_turnstile_lock);
    turnstile_complete((uintptr_t)ut, &ut->turnstile, NULL, TURNSTILE_EPOCH_USER);
    lck_spin_unlock(&user_turnstile_lock);

    turnstile_cleanup();
}

void switch_user_turnstile_thread_retain(thread_t thread)
{
    if (thread != THREAD_NULL) {
        thread_reference(thread);
    }
}

void switch_user_turnstile_thread_drop(thread_t thread)
{
    if (thread != THREAD_NULL) {
        thread_deallocate_safe(thread);
    }
}
