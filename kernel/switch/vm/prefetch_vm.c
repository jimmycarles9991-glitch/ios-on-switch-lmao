// SPDX-License-Identifier: GPL-2.0-only
#define SWITCH_PREFETCH_IMPL 1
#include <switch/prefetch_vm.h>
#include "prefetch_pace.h"

#include <kern/clock.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/vm_param.h>
#if XNU_12377_121_6
#include <vm/memory_object_internal.h>
#include <vm/vm_object_internal.h>
#include <vm/vm_page_internal.h>
#else
#include <vm/memory_object.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#endif

extern struct vnode *vnode_pager_lookup_vnode(memory_object_t pager);

static struct switch_prefetch_pace g_pace;
static thread_t g_replay_thread;
static int g_pace_event;
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
static uint64_t g_replay_abs;
static uint64_t g_demand_upl_abs;
static uint64_t g_replay_decmp_bytes;
static uint64_t g_replay_decmp_tagged_bytes;
#endif

bool switch_vm_prefetch_used_locked(void)
{
    return switch_pace_on_used(&g_pace);
}

/* The file's vnode outlives this call: every caller holds the page's object locked while a mapping, a read's iocount
 * or the shared region holds the vnode, and a vnode object's pager names that vnode until the object terminates. */
void switch_vm_prefetch_first_use(void *object_arg, uint64_t offset)
{
    vm_object_t object = (vm_object_t)object_arg;
    if (object == VM_OBJECT_NULL || object->internal || object->pager == MEMORY_OBJECT_NULL) {
        return;
    }
    struct vnode *vp = vnode_pager_lookup_vnode(object->pager);
    if (vp != NULL) {
        switch_cluster_record_file(vp, (int64_t)(offset + object->paging_offset), PAGE_SIZE, SWITCH_RECORD_FIRST_USE);
    }
}

bool switch_vm_prefetch_reclaimed_locked(void)
{
    return switch_pace_on_reclaimed(&g_pace);
}

void switch_vm_prefetch_wake(void)
{
    thread_wakeup((event_t)&g_pace_event);
}

bool switch_vm_prefetch_is_replay_thread(void *thread)
{
    return thread != NULL && __atomic_load_n(&g_replay_thread, __ATOMIC_RELAXED) == (thread_t)thread;
}

void switch_vm_prefetch_inflight_add(uint32_t bytes)
{
    switch_pace_inflight_add(&g_pace, bytes);
}

void switch_vm_prefetch_committed(uint32_t pages)
{
    switch_pace_committed(&g_pace, pages);
}

void switch_vm_prefetch_inflight_done(uint32_t bytes, uint64_t elapsed_abs)
{
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
    __atomic_fetch_add(&g_replay_abs, elapsed_abs, __ATOMIC_RELAXED);
#else
    (void)elapsed_abs;
#endif
    if (switch_pace_inflight_done(&g_pace, bytes)) {
        thread_wakeup((event_t)&g_pace_event);
    }
}

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
void switch_vm_prefetch_count_replay_decmp(uint64_t bytes, bool tagged)
{
    __atomic_fetch_add(&g_replay_decmp_bytes, bytes, __ATOMIC_RELAXED);
    if (tagged) {
        __atomic_fetch_add(&g_replay_decmp_tagged_bytes, bytes, __ATOMIC_RELAXED);
    }
}

void switch_vm_prefetch_demand_done(uint64_t elapsed_abs)
{
    __atomic_fetch_add(&g_demand_upl_abs, elapsed_abs, __ATOMIC_RELAXED);
}
#endif

void switch_vm_pressure_changed(unsigned int old_level __unused, unsigned int new_level)
{
    /* kVMPressureNormal is 0 in both XNU trees. Runs from vm_pressure_response() beside XNU's own
     * thread_wakeup(&vm_pageout_state.vm_pressure_changed); allocates nothing and takes no lock.
     * Stops replay only so recording keeps running and Boot Complete can overwrite an oversized list. */
    if (new_level == 0u) {
        return;
    }
    uint32_t first = 0u;
    (void)switch_pace_stop_replay(&g_pace, SWITCH_PREFETCH_STOP_PRESSURE, &first);
    thread_wakeup((event_t)&g_pace_event);
}

bool switch_vm_prefetch_start_thread(void (*entry)(void *, int), void *arg)
{
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start_priority((thread_continue_t)entry, arg, BASEPRI_UTILITY, &thread) != KERN_SUCCESS) {
        return false;
    }
    thread_deallocate(thread);
    return true;
}

uint64_t switch_vm_prefetch_begin(uint64_t list_vm_pages, void *replay_thread)
{
    const uint64_t headroom = switch_vm_pressure_headroom_pages();
    switch_pace_init(&g_pace, list_vm_pages, headroom, SWITCH_PREFETCH_INFLIGHT_LIMIT);
    __atomic_store_n(&g_replay_thread, (thread_t)replay_thread, __ATOMIC_RELEASE);
    return __atomic_load_n(&g_pace.lead_bound, __ATOMIC_RELAXED);
}

void switch_vm_prefetch_end_replay(void)
{
#if !((DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO)
    switch_pace_deactivate(&g_pace);
#endif
}

/* Called only after the in-flight drain: XNU's busy-page raise (switch_upl_below_waiter) and the SD queue's replay
 * class recognise the replay's UPLs by this thread until the last of its reads completes. */
void switch_vm_prefetch_forget_replay_thread(void)
{
    __atomic_store_n(&g_replay_thread, THREAD_NULL, __ATOMIC_RELEASE);
}

void switch_vm_prefetch_wait_inflight_drain(void)
{
    for (;;) {
        if (switch_pace_inflight_bytes(&g_pace) == 0u) {
            return;
        }
        assert_wait((event_t)&g_pace_event, THREAD_UNINT);
        __atomic_store_n(&g_pace.waiting, 1u, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (switch_pace_inflight_bytes(&g_pace) == 0u) {
            __atomic_store_n(&g_pace.waiting, 0u, __ATOMIC_RELAXED);
            clear_wait(current_thread(), THREAD_AWAKENED);
            return;
        }
        thread_block(THREAD_CONTINUE_NULL);
        __atomic_store_n(&g_pace.waiting, 0u, __ATOMIC_RELAXED);
    }
}

void switch_vm_prefetch_finish(void)
{
    switch_pace_deactivate(&g_pace);
}

bool switch_vm_prefetch_can_issue_now(void)
{
    return switch_pace_can_issue(&g_pace);
}

bool switch_vm_prefetch_wait_can_issue(void)
{
    for (;;) {
        if (switch_pace_replay_stop_reason(&g_pace) != SWITCH_PREFETCH_STOP_NONE) {
            return false;
        }
        if (switch_pace_can_issue(&g_pace)) {
            return true;
        }
        assert_wait((event_t)&g_pace_event, THREAD_UNINT);
        __atomic_store_n(&g_pace.waiting, 1u, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (switch_pace_replay_stop_reason(&g_pace) != SWITCH_PREFETCH_STOP_NONE || switch_pace_can_issue(&g_pace)) {
            __atomic_store_n(&g_pace.waiting, 0u, __ATOMIC_RELAXED);
            clear_wait(current_thread(), THREAD_AWAKENED);
            continue;
        }
        thread_block(THREAD_CONTINUE_NULL);
        __atomic_store_n(&g_pace.waiting, 0u, __ATOMIC_RELAXED);
    }
}

bool switch_vm_prefetch_wait_until(uint64_t deadline_abs)
{
    for (;;) {
        if (switch_pace_replay_stop_reason(&g_pace) != SWITCH_PREFETCH_STOP_NONE) {
            return false;
        }
        if (mach_absolute_time() >= deadline_abs) {
            return true;
        }
        /* g_pace.waiting stays 0: with it set, every first use or reclaim of a prefetched page would wake this
         * thread from the faulting path while nothing of the replay is in flight. Both stop paths wake it
         * unconditionally, and the recheck below closes the race with a stop between the check above and the wait. */
        assert_wait_deadline((event_t)&g_pace_event, THREAD_UNINT, deadline_abs);
        if (switch_pace_replay_stop_reason(&g_pace) != SWITCH_PREFETCH_STOP_NONE) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            continue;
        }
        thread_block(THREAD_CONTINUE_NULL);
    }
}

uint32_t switch_vm_prefetch_wait_stop(void)
{
    for (;;) {
        const uint32_t reason = switch_pace_boot_stop_reason(&g_pace);
        if (reason != SWITCH_PREFETCH_STOP_NONE) {
            return reason;
        }
        assert_wait((event_t)&g_pace_event, THREAD_UNINT);
        __atomic_store_n(&g_pace.waiting, 1u, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (switch_pace_boot_stop_reason(&g_pace) != SWITCH_PREFETCH_STOP_NONE) {
            __atomic_store_n(&g_pace.waiting, 0u, __ATOMIC_RELAXED);
            clear_wait(current_thread(), THREAD_AWAKENED);
            continue;
        }
        thread_block(THREAD_CONTINUE_NULL);
        __atomic_store_n(&g_pace.waiting, 0u, __ATOMIC_RELAXED);
    }
}

uint32_t switch_vm_prefetch_stop(uint32_t reason)
{
    uint32_t first = 0u;
    (void)switch_pace_stop_boot(&g_pace, reason, &first);
    thread_wakeup((event_t)&g_pace_event);
    return first;
}

uint32_t switch_vm_prefetch_replay_stop_reason(void)
{
    return switch_pace_replay_stop_reason(&g_pace);
}

uint32_t switch_vm_prefetch_boot_stop_reason(void)
{
    return switch_pace_boot_stop_reason(&g_pace);
}

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
void switch_vm_prefetch_pace_snapshot(struct switch_prefetch_pace_snapshot *out)
{
    out->replayed_pages = __atomic_load_n(&g_pace.replayed_pages, __ATOMIC_RELAXED);
    out->used_pages = __atomic_load_n(&g_pace.used_pages, __ATOMIC_RELAXED);
    out->reclaimed_pages = __atomic_load_n(&g_pace.reclaimed_pages, __ATOMIC_RELAXED);
    out->lead_pages = switch_pace_lead(&g_pace);
    out->lead_bound = __atomic_load_n(&g_pace.lead_bound, __ATOMIC_RELAXED);
    out->replay_abs = __atomic_load_n(&g_replay_abs, __ATOMIC_RELAXED);
    out->demand_upl_abs = __atomic_load_n(&g_demand_upl_abs, __ATOMIC_RELAXED);
    out->replay_decmp_bytes = __atomic_load_n(&g_replay_decmp_bytes, __ATOMIC_RELAXED);
    out->replay_decmp_tagged_bytes = __atomic_load_n(&g_replay_decmp_tagged_bytes, __ATOMIC_RELAXED);
}
#endif
