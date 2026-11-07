// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch frame diagnostics (debug kernels): the render server's clients by commit cadence, and how long backboardd takes
 * to route each touch report, for NativeLegacyDisplay's stats window and touch interaction lines (frames.h has the
 * definitions). Two sets of accumulators, one per kind of window; each message and report feeds both. The same events,
 * with the display's presents, carry one touch at a time to the panel for the interaction's latency line (latency.h).
 */

#include <kern/misc_protos.h>
#include <switch/frames.h>
#include <switch/xnu.h>

#include "frames.h"
#include "latency.h"

#include <ipc/ipc_space.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <string.h>

#if DEVELOPMENT || DEBUG

#define PERIOD_US 16667

enum {
    STATS_WINDOW,
    INTERACTION_WINDOW,
    WINDOWS
};

static struct {
    struct switch_frames_cadence cadence[WINDOWS];
    struct switch_frames_delivery delivery[WINDOWS];
} acc;

/* backboardd sends a report to SpringBoard, then to the app under the finger, back to back: the sends of its event
 * threads until this long after a delivery go to the same report's other receivers. */
#define CO_RECEIVERS_US 2000

static struct switch_latency latency;
static uint64_t touch_began, touch_last_down;
static uint32_t report_pending;
static uint64_t co_receivers_until; /* mach */

static uint64_t abs_to_us(uint64_t abs)
{
    uint64_t ns;

    absolutetime_to_nanoseconds(abs, &ns);
    return ns / NSEC_PER_USEC;
}

static LCK_GRP_DECLARE(frames_grp, "switch_frames");
static LCK_SPIN_DECLARE(frames_lock, &frames_grp);

void switch_frames_render_message(task_t sender, mach_msg_id_t id, bool own)
{
    char name[SWITCH_FRAMES_NAME];

    if (own || sender == kernel_task) {
        return;
    }
    const uint64_t now = abs_to_us(mach_absolute_time());
    switch_task_name(sender, name, sizeof(name));
    lck_spin_lock(&frames_lock);
    for (int w = 0; w < WINDOWS; w++) {
        switch_frames_note_message(&acc.cadence[w], task_pid(sender), name, now, PERIOD_US);
    }
    switch_latency_message(&latency, task_pid(sender), id, now);
    lck_spin_unlock(&frames_lock);
}

void switch_frames_render_server_sent(ipc_port_t port)
{
    char name[SWITCH_FRAMES_NAME];
    ipc_space_t space;
    task_t receiver;

    if (current_thread()->base_pri < SWITCH_FRAMES_EVENT_PRI) {
        return;
    }
    const bool report = os_atomic_load(&report_pending, relaxed) != 0;
    const uint64_t abs = mach_absolute_time();
    if (!report && abs > os_atomic_load(&co_receivers_until, relaxed)) {
        return;
    }
    space = ip_get_receiver(port);
    receiver = space != NULL ? space->is_task : TASK_NULL;
    if (receiver == TASK_NULL || receiver == kernel_task) {
        return;
    }
    if (!report) {
        lck_spin_lock(&frames_lock);
        switch_latency_also_delivered(&latency, task_pid(receiver));
        lck_spin_unlock(&frames_lock);
        return;
    }
    switch_task_name(receiver, name, sizeof(name));
    const uint64_t now = abs_to_us(abs);
    uint64_t window;
    nanoseconds_to_absolutetime((uint64_t)CO_RECEIVERS_US * NSEC_PER_USEC, &window);
    lck_spin_lock(&frames_lock);
    os_atomic_store(&co_receivers_until, abs + window, relaxed);
    switch_latency_delivered(&latency, acc.delivery[STATS_WINDOW].pending_us, task_pid(receiver), now);
    for (int w = 0; w < WINDOWS; w++) {
        switch_frames_note_delivery(&acc.delivery[w], task_pid(receiver), name, now);
    }
    os_atomic_store(&report_pending, 0, relaxed);
    lck_spin_unlock(&frames_lock);
}

void switch_frames_touch(uint64_t stamp, bool down, unsigned int changes)
{
    uint64_t gap;

    nanoseconds_to_absolutetime((uint64_t)SWITCH_FRAMES_RUN_GAP_MS * NSEC_PER_MSEC, &gap);
    lck_spin_lock(&frames_lock);
    const bool running = touch_last_down != 0 && stamp - touch_last_down <= gap;
    if (down && !running) {
        touch_began = stamp;
    }
    if (down || running) {
        touch_last_down = stamp;
    }
    for (int w = 0; w < WINDOWS; w++) {
        switch_frames_note_report(&acc.delivery[w], abs_to_us(stamp));
    }
    switch_latency_report(&latency, changes);
    os_atomic_store(&report_pending, 1, relaxed);
    lck_spin_unlock(&frames_lock);
}

void switch_frames_touch_activity(uint64_t *began, uint64_t *last_down)
{
    lck_spin_lock(&frames_lock);
    *began = touch_began;
    *last_down = touch_last_down;
    lck_spin_unlock(&frames_lock);
}

size_t switch_frames_line(bool interaction, char *out, size_t size)
{
    struct switch_frames_cadence cadence;
    struct switch_frames_delivery delivery;
    const int w = interaction ? INTERACTION_WINDOW : STATS_WINDOW;

    lck_spin_lock(&frames_lock);
    cadence = acc.cadence[w];
    delivery = acc.delivery[w];
    switch_frames_cadence_restart(&acc.cadence[w]);
    acc.delivery[w] = (struct switch_frames_delivery){.pending_us = acc.delivery[w].pending_us};
    lck_spin_unlock(&frames_lock);
    return switch_frames_format(&cadence, &delivery, 4, out, size);
}

void switch_frames_present(uint64_t vsync, uint64_t submit, uint64_t latch)
{
    const uint64_t vsync_us = vsync != 0 ? abs_to_us(vsync) : 0, submit_us = abs_to_us(submit),
                   latch_us = abs_to_us(latch);

    lck_spin_lock(&frames_lock);
    switch_latency_present(&latency, vsync_us, submit_us, latch_us);
    lck_spin_unlock(&frames_lock);
}

size_t switch_frames_latency_line(char *out, size_t size)
{
    struct switch_latency_summary summary;

    lck_spin_lock(&frames_lock);
    switch_latency_summarize(&latency, &summary);
    switch_latency_restart(&latency);
    lck_spin_unlock(&frames_lock);
    return switch_latency_format(&summary, out, size);
}

#else /* DEVELOPMENT || DEBUG */

void switch_frames_touch(__unused uint64_t stamp, __unused bool down, __unused unsigned int changes)
{}

void switch_frames_touch_activity(uint64_t *began, uint64_t *last_down)
{
    *began = *last_down = 0;
}

size_t switch_frames_line(__unused bool interaction, char *out, size_t size)
{
    if (size != 0) {
        out[0] = 0;
    }
    return 0;
}

void switch_frames_present(__unused uint64_t vsync, __unused uint64_t submit, __unused uint64_t latch)
{}

size_t switch_frames_latency_line(char *out, size_t size)
{
    if (size != 0) {
        out[0] = 0;
    }
    return 0;
}

#endif /* DEVELOPMENT || DEBUG */
