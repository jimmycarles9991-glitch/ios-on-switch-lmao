// SPDX-License-Identifier: GPL-2.0-only
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <kern/debug.h>
#include <mach/vm_param.h>
#include <arm/cpu_data_internal.h>
#include <vm/vm_kern.h>

#include <switch/diag.h>
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern vm_offset_t kernel_stack_size;

/*
 * Switch: every thread blocked in the middle of a call (it kept its kernel stack), as "T<tid> h<block hint> p<priority>
 * e<wait event> [owner T<tid>]" and its saved return addresses below the context switch (unslid, low 36 bits); for a
 * kernel-mutex wait the owner comes from kdp_lck_mtx_find_owner, and each owner's own stack follows. The only lock it
 * takes is tasks_threads_lock, never OSKext's: a thread stuck in a kext load holds it.
 */
static unsigned switch_thread_frames(thread_t thread, uint64_t *frames, unsigned max)
{
    const vm_offset_t low = thread->kernel_stack;
    const vm_offset_t high = low + kernel_stack_size;
    arm_kernel_saved_state_t *saved = &thread_get_kernel_state(thread)->machine.ss;
    unsigned count = 0;
    uint64_t fp = saved->fp;
    frames[count++] = saved->lr;
    while (count < max && fp >= low && fp + 16 <= high && !(fp & 0xf)) {
        frames[count++] = ((uint64_t *)fp)[1];
        fp = ((uint64_t *)fp)[0];
    }
    return count;
}

static void switch_print_frames(const uint64_t *frames, unsigned count)
{
    printf("   ");
    for (unsigned i = 2; i < count; i++) {
        printf(" %llx", (uint64_t)VM_KERNEL_UNSLIDE(frames[i]) & 0xfffffffffULL);
    }
    printf("\n");
}

void switch_print_blocked_threads(unsigned max_threads)
{
    unsigned printed = 0;
    unsigned blocked = 0;
    unsigned owners = 0;
    uint64_t owner_ids[8];
    thread_t thread = NULL;
    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&threads, thread, thread_t, threads)
    {
        if (thread == current_thread() || !thread->kernel_stack || !((unsigned int)thread->state & TH_WAIT)) {
            continue;
        }
        blocked++;
        if (printed == max_threads) {
            continue;
        }
        printf("T%llu h%d p%d e%llx", thread->thread_id, thread->block_hint, thread->sched_pri,
               (unsigned long long)thread->wait_event);
        if (thread->block_hint == kThreadWaitKernelMutex) {
            thread_waitinfo_t info = {};
            kdp_lck_mtx_find_owner(thread->waitq.wq_q, thread->wait_event, &info);
            printf(" owner T%llu", info.owner);
            bool known = false;
            for (unsigned o = 0; o < owners; o++) {
                known = known || owner_ids[o] == info.owner;
            }
            if (!known && owners < 8) {
                owner_ids[owners++] = info.owner;
            }
        }
        printf("\n");
        uint64_t frames[32];
        const unsigned count = switch_thread_frames(thread, frames, 32);
        if (count > 2) {
            switch_print_frames(frames, count);
        }
        printed++;
    }
    for (unsigned o = 0; o < owners; o++) {
        queue_iterate(&threads, thread, thread_t, threads)
        {
            if (thread->thread_id != owner_ids[o]) {
                continue;
            }
            printf("owner T%llu state=%x h%d run=%d\n", thread->thread_id, (unsigned)thread->state, thread->block_hint,
                   thread->last_processor ? thread->last_processor->cpu_id : -1);
            if (thread->kernel_stack && thread != current_thread() && !((unsigned int)thread->state & TH_RUN)) {
                uint64_t frames[32];
                switch_print_frames(frames, switch_thread_frames(thread, frames, 32));
            }
        }
    }
    lck_mtx_unlock(&tasks_threads_lock);
    printf("blocked threads with a stack: %u, shown: %u\n", blocked, printed);
}

static void switch_print_sched(const char *who, const char *role, thread_t thread)
{
    printf("%s: %sT%llu state %#x h%d pri %d/%d cpu %d runq %d chosen %d event %#llx", who, role, thread->thread_id,
           (unsigned)thread->state, thread->block_hint, thread->base_pri, thread->sched_pri,
           thread->last_processor ? thread->last_processor->cpu_id : -1,
           thread->__runq.runq ? thread->__runq.runq->cpu_id : -1,
           thread->chosen_processor ? thread->chosen_processor->cpu_id : -1, (unsigned long long)thread->wait_event);
}

/*
 * One thread's scheduling state (including the run queue it waits on), what it waits on and, when it is not on a core,
 * its saved return addresses; for a kernel-mutex wait the owner's too: for a driver about to panic because its work
 * loop never ran an action it was sent (an interrupt's, say).
 */
void switch_print_thread(const void *ref, const char *who)
{
    thread_t thread = (thread_t)(uintptr_t)ref;
    if (thread == NULL) {
        printf("%s: no thread\n", who);
        return;
    }
    switch_print_sched(who, "", thread);
    uint64_t owner = 0;
    if (thread->block_hint == kThreadWaitKernelMutex) {
        thread_waitinfo_t info = {};
        kdp_lck_mtx_find_owner(thread->waitq.wq_q, thread->wait_event, &info);
        owner = info.owner;
        printf(" owner T%llu", owner);
    }
    printf("\n");
    if (thread->kernel_stack && thread != current_thread() && !((unsigned int)thread->state & TH_RUN)) {
        uint64_t frames[32];
        switch_print_frames(frames, switch_thread_frames(thread, frames, 32));
    }
    if (!owner) {
        return;
    }
    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&threads, thread, thread_t, threads)
    {
        if (thread->thread_id != owner) {
            continue;
        }
        switch_print_sched(who, "owner ", thread);
        printf("\n");
        if (thread->kernel_stack && thread != current_thread() && !((unsigned int)thread->state & TH_RUN)) {
            uint64_t frames[32];
            switch_print_frames(frames, switch_thread_frames(thread, frames, 32));
        }
    }
    lck_mtx_unlock(&tasks_threads_lock);
}

/* Every processor's scheduler state, what it runs and the IPIs pending at it, beside switch_print_thread's lines. */
void switch_print_processors(const char *who)
{
    for (processor_t processor = processor_list; processor != NULL; processor = processor->processor_list) {
        const cpu_data_t *cdp = cpu_datap(processor->cpu_id);
        thread_t active = processor->active_thread;
        printf("%s: cpu %d state %d active T%llu pri %d signal %#x\n", who, processor->cpu_id, (int)processor->state,
               active ? active->thread_id : 0, active ? active->sched_pri : -1, cdp ? cdp->cpu_signal : 0);
    }
}

/*
 * One line of what the scheduler is doing: `thread`'s state, priority and queue, then each processor's state and its
 * active thread (tid, task pid, priority). Formatting only, no console: a driver takes it at a deadline and prints it
 * only if it panics later.
 */
void switch_sched_describe(char *buffer, size_t size, const void *ref)
{
    thread_t thread = (thread_t)(uintptr_t)ref;
    int used = 0;
    if (thread != NULL) {
        used = snprintf(buffer, size, "T%llu state %#x pri %d/%d runq %d chosen %d |", thread->thread_id,
                        (unsigned)thread->state, thread->base_pri, thread->sched_pri,
                        thread->__runq.runq ? thread->__runq.runq->cpu_id : -1,
                        thread->chosen_processor ? thread->chosen_processor->cpu_id : -1);
    }
    for (processor_t processor = processor_list; processor != NULL && used >= 0 && (size_t)used < size;
         processor = processor->processor_list) {
        thread_t active = processor->active_thread;
        used += snprintf(buffer + used, size - (size_t)used, " cpu%d st%d T%llu/p%d/%d", processor->cpu_id,
                         (int)processor->state, active ? active->thread_id : 0,
                         active ? task_pid(get_threadtask(active)) : -1, active ? active->sched_pri : -1);
    }
}
