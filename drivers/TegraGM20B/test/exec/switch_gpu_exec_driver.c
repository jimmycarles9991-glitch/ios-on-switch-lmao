// SPDX-License-Identifier: MIT
// What gpu_exec.c takes from the rest of the driver (nm -u of its object), as the other gpu_*.c units define it: the
// shared state, and the calls outside the commit point's order as no-ops.
#include "gpu_internal.h"
#include "switch_gpu_exec_fixture.h"

struct gpu_state tgm_gpu;
struct idle_state tgm_idle;
struct switch_gpu_busy tgm_busy;
struct stats_state tgm_stats;
struct wstat_state tgm_wstat;
struct frame_gpu_state tgm_frame_gpu;
struct present_state tgm_present;
struct fences_state tgm_fence_pages;
struct tgm_gate tgm_sc7_gate;
DEFINE_MUTEX(tgm_gate_lock);
LIST_HEAD(tgm_live_chans);
DEFINE_MUTEX(tgm_live_lock);

static struct nvkm_device device;
static u32 parked;
static enum switch_exec_kick_kind test_sched_kick;
static int test_deadline_later;
static u64 test_ptimer;

void switch_exec_reset_driver(void)
{
	memset(&tgm_stats, 0, sizeof(tgm_stats));
	memset(&tgm_busy, 0, sizeof(tgm_busy));
	test_sched_kick = SWITCH_EXEC_KICK_CLEAR;
	test_deadline_later = 0;
	test_ptimer = 0;
}

void switch_exec_set_kick(enum switch_exec_kick_kind sched_kick, int deadline_later)
{
	test_sched_kick = sched_kick;
	test_deadline_later = deadline_later;
}

void switch_exec_set_ptimer(uint64_t pt)
{
	test_ptimer = pt;
}

void switch_exec_take_later_stats(struct switch_exec_later_stats *out)
{
#if TGM_STATS
	out->kicks = __atomic_exchange_n(&tgm_stats.later_kicks, 0, __ATOMIC_RELAXED);
	out->frames = __atomic_exchange_n(&tgm_stats.later_frames, 0, __ATOMIC_RELAXED);
	out->frame_max = __atomic_exchange_n(&tgm_stats.later_frame_max, 0, __ATOMIC_RELAXED);
	out->queue_max = __atomic_exchange_n(&tgm_stats.later_queue_max, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&tgm_stats.later_in_frame, 0, __ATOMIC_RELAXED);
#else
	memset(out, 0, sizeof(*out));
#endif
}

void switch_exec_take_foreign_stats(struct switch_exec_foreign_stats *out)
{
#if TGM_STATS
	out->jobs = __atomic_exchange_n(&tgm_stats.foreign_jobs, 0, __ATOMIC_RELAXED);
	out->fences = __atomic_exchange_n(&tgm_stats.foreign_fences, 0, __ATOMIC_RELAXED);
	out->promises = __atomic_exchange_n(&tgm_stats.foreign_promises, 0, __ATOMIC_RELAXED);
	out->events = __atomic_exchange_n(&tgm_stats.foreign_events, 0, __ATOMIC_RELAXED);
	out->host1x = __atomic_exchange_n(&tgm_stats.foreign_host1x, 0, __ATOMIC_RELAXED);
	out->timed = __atomic_exchange_n(&tgm_stats.foreign_timed, 0, __ATOMIC_RELAXED);
	out->queue_sum = __atomic_exchange_n(&tgm_stats.foreign_queue_sum, 0, __ATOMIC_RELAXED);
	out->queue_max = __atomic_exchange_n(&tgm_stats.foreign_queue_max, 0, __ATOMIC_RELAXED);
#else
	memset(out, 0, sizeof(*out));
#endif
}

void switch_exec_take_l2_inval_stats(struct switch_exec_l2_inval_stats *out)
{
#if TGM_STATS
	out->total = __atomic_exchange_n(&tgm_stats.l2_inval, 0, __ATOMIC_RELAXED);
	out->host_written = __atomic_exchange_n(&tgm_stats.l2_inval_host, 0, __ATOMIC_RELAXED);
	out->compositor = __atomic_exchange_n(&tgm_stats.l2_inval_host_comp, 0, __ATOMIC_RELAXED);
	out->tlb = __atomic_exchange_n(&tgm_stats.l2_inval_tlb, 0, __ATOMIC_RELAXED);
	out->kernel_push = __atomic_exchange_n(&tgm_stats.l2_inval_kernel, 0, __ATOMIC_RELAXED);
#else
	memset(out, 0, sizeof(*out));
#endif
}

__attribute__((constructor)) static void switch_exec_device(void)
{
	tgm_gpu.device = &device;
}

uint32_t tgm_chan_completed(struct tgm_chan *chan)
{
	return READ_ONCE(*chan->fence_cpu);
}

int switch_gpu_promise_park_locked(uint32_t offset, uint32_t value, const char *waiter,
				   const char *who, const struct switch_gpu_giveup_job *reader,
				   int display)
{
	if (!switch_exec_mutex_held())
		nvxnu_panic("promise parked outside live owner");
	++parked;
	return 1;
}

uint32_t switch_exec_parked_promises(void)
{
	return parked;
}

void tgm_gate_enter(void)
{
}

int cpu_number(void)
{
	return 0;
}

void _disable_preemption(void)
{
}

void _enable_preemption(void)
{
}

static void (*idle_raised)(void *context);
static void *idle_context;

void switch_exec_idle_low(void (*raised)(void *context), void *context)
{
	idle_raised = raised;
	idle_context = context;
	tgm_idle.on = tgm_idle.low = raised != NULL;
	/* idle_stats' periodic line, which takes tgm_idle.clk, stays out of the window */
	tgm_idle.log_start = nvxnu_nanotime();
}

u64 tgm_idle_raise(void)
{
	if (idle_raised)
		idle_raised(idle_context);
	tgm_idle.low = 0;
	return 0;
}

void tgm_chan_ramfc_due(struct tgm_chan *chan)
{
}

void tgm_chan_stalled(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
}

void tgm_chan_wake_push(struct tgm_chan *chan)
{
}

int tgm_deadline_kicked(int compositor, int app)
{
	return compositor ? test_deadline_later : 0;
}

uint64_t tgm_deadline_pt_ns(uint64_t pt_span)
{
	return pt_span;
}

int tgm_deadline_app_kicked(void)
{
	return 0;
}

uint64_t tgm_hold_held_ns(unsigned int chid, uint64_t now)
{
	return 0;
}

void tgm_hold_need(struct nvkm_device *dev, unsigned int chid)
{
}

void tgm_hold_released(unsigned int chid, uint64_t end_pt, int job_last)
{
}

void tgm_hold_track(struct nvkm_device *dev, unsigned int chid, int on)
{
}

#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_owned_waits.h"

enum tgm_sched_kick tgm_sched_kicked(struct nvkm_device *dev, unsigned int chid,
				     struct tgm_sched_queued *queued)
{
	memset(queued, 0, sizeof(*queued));
	queued->behind = -1;
	switch (test_sched_kick) {
	case SWITCH_EXEC_KICK_PREEMPTED:
		return TGM_KICK_PREEMPTED;
	case SWITCH_EXEC_KICK_SWITCHING:
		return TGM_KICK_SWITCHING;
	case SWITCH_EXEC_KICK_DEFERRED:
		return TGM_KICK_DEFERRED;
	case SWITCH_EXEC_KICK_CLEAR:
	default:
		return TGM_KICK_CLEAR;
	}
}

void tgm_stats_tick(void)
{
}

void tgm_stats_loud(struct tgm_chan *chan, uint32_t reason)
{
}

void tgm_stats_gr_context(void)
{
}

#if TGM_STATS
void switch_gpu_compositor_job(uint64_t ns)
{
}

void switch_gpu_compositor_sync(uint32_t syncs, uint64_t ns, int behind)
{
}
#endif

int nvif_event_allow(struct nvif_event *event)
{
	return 0;
}

u64 nvkm_timer_read(struct nvkm_timer *timer)
{
	return test_ptimer ? test_ptimer : nvxnu_nanotime();
}

int nvkm_clk_read(struct nvkm_clk *clk, enum nv_clk_src src)
{
	return 0;
}

void switch_gpu_timeline_semantic_visibility(struct switch_gpu_timeline *timeline,
					     volatile struct TegraGM20BAbiFenceSemantic *state)
{
}

void switch_gpu_timeline_visibility(struct switch_gpu_timeline *timeline,
				    const volatile uint32_t *visible)
{
}

void switch_emc_gpu_submit(void)
{
}
