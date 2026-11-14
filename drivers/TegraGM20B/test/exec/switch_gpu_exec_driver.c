// SPDX-License-Identifier: MIT
// What gpu_exec.c takes from the rest of the driver (nm -u of its object), as the other gpu_*.c units define it: the
// shared state, and the calls outside the commit point's order as no-ops.
#include "gpu_internal.h"
#include "switch_gpu_exec_fixture.h"

struct gpu_state tgm_gpu;
struct idle_state tgm_idle;
struct switch_gpu_busy tgm_busy;
struct wstat_state tgm_wstat;
struct frame_gpu_state tgm_frame_gpu;
struct fences_state tgm_fence_pages;
struct tgm_gate tgm_sc7_gate;
DEFINE_MUTEX(tgm_gate_lock);
LIST_HEAD(tgm_live_chans);
DEFINE_MUTEX(tgm_live_lock);

static struct nvkm_device device;
static u32 parked;

__attribute__((constructor)) static void switch_exec_device(void)
{
	tgm_gpu.device = &device;
}

uint32_t tgm_chan_completed(struct tgm_chan *chan)
{
	return READ_ONCE(*chan->fence_cpu);
}

void tgm_chan_park_promises(struct tgm_chan *chan, const struct switch_gpu_backing_token *tokens,
			    u32 count)
{
	++parked;
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

u64 tgm_idle_raise(void)
{
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

void tgm_deadline_kicked(int compositor, int app)
{
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

void tgm_hold_track(struct nvkm_device *dev, unsigned int chid, int on)
{
}

int tgm_order_waits(struct tgm_chan *chan)
{
	return 0;
}

enum tgm_sched_kick tgm_sched_kicked(struct nvkm_device *dev, unsigned int chid,
				     struct tgm_sched_queued *queued)
{
	memset(queued, 0, sizeof(*queued));
	return TGM_KICK_CLEAR;
}

void tgm_stats_tick(void)
{
}

void tgm_stats_loud(struct tgm_chan *chan, uint32_t reason)
{
}

int nvif_event_allow(struct nvif_event *event)
{
	return 0;
}

u64 nvkm_timer_read(struct nvkm_timer *timer)
{
	return nvxnu_nanotime();
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
