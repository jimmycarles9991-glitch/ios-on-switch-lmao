// SPDX-License-Identifier: MIT
// What gpu_chan.c takes from the rest of the driver (nm -u of its object), for switch_gpu_chan_new_test: nvif's channel
// objects over a model of the host's RAMFC, the other calls as no-ops. The model: nvkm's channel constructor writes
// RAMFC (gk104_chan_ramfc_write) and nvkm_uchan_init puts the channel on its runlist enabled, where the host may load it
// before its first submission; a loaded channel's RAMFC lives in the PBDMA and goes back to memory when a preempt
// switches it out, as each engine object's context bind does (nvkm_chan_cctx_bind: block, preempt, allow).
#include "gpu_internal.h"
#include "SwitchGpuChannelEpoch.h"
#include "TegraGM20BRamfcOrder.h"
#include "switch_gpu_chan_new_harness.h"

struct gpu_state tgm_gpu;
struct platform_device tgm_gpu_pdev;
struct switch_gpu_busy tgm_busy;
struct stats_state tgm_stats;
struct wstat_state tgm_wstat;
struct present_state tgm_present;
struct scrub_state tgm_scrub;
struct tgm_gate tgm_sc7_gate;
struct switch_gpu_wake_word tgm_waiter_words[TGM_WAITERS];
u64 tgm_waiter_woken_ns[TGM_WAITERS];
u64 tgm_live_ids;
u64 tgm_high_end_ns, tgm_high_end_pt;
DEFINE_MUTEX(tgm_gate_lock);
DEFINE_MUTEX(tgm_live_lock);
LIST_HEAD(tgm_live_chans);

static struct nvkm_device device;
static struct switch_chan_new_host host;
static struct tgm_vm vm;
static volatile u32 fence;
static struct tgm_chan *made;
static u32 push_words[1024];
static u32 userd[128];
static int timeline;
static int isolated_critical = -1;

__attribute__((constructor)) static void switch_chan_new_device(void)
{
	tgm_gpu.device = &device;
	tgm_gpu.stage = 3;
}

struct switch_chan_new_host *switch_chan_new_host(void)
{
	return &host;
}

int switch_chan_new_make(uint32_t chid, int critical)
{
	host = (struct switch_chan_new_host){.chid = chid};
	vm.owner = &vm;
	return tgm_chan_new(&vm, &vm, &fence, 0x200000, "backboardd", critical, &made);
}

uint32_t switch_chan_new_recorded_timeslice(void)
{
	return made->timeslice;
}

void switch_chan_new_delete(void)
{
	tgm_chan_del(&made);
}

void switch_chan_new_leave_running(void)
{
	made->submitted = tgm_chan_completed(made) + 1;
}

int switch_chan_new_isolated_critical(void)
{
	return isolated_critical;
}

void switch_chan_new_set_level(int high, int critical)
{
	tgm_chan_set_level(made, high ? TGM_LEVEL_HIGH : TGM_LEVEL_FOREGROUND, critical);
}

int switch_chan_new_high(void)
{
	return made->high;
}

int switch_chan_new_critical(void)
{
	return made->critical;
}

static void host_save(void)
{
	if (host.loaded) {
		host.memory = host.pbdma;
		host.loaded = 0;
	}
}

static void host_load(void)
{
	if (host.enabled) {
		host.pbdma = host.memory;
		host.loaded = 1;
	}
}

void switch_chan_new_job(void)
{
	host_save();
	host_load();
	host_save();
}

void tgm_ramfc_write(u32 chid, u32 offset, u32 value)
{
	if (chid != host.chid)
		return;
	if (offset == RAMFC_TIMESLICE)
		host.memory.timeslice = value;
	else if (offset == TGM_RAMFC_ACQUIRE)
		host.memory.acquire = value;
}

void tgm_acquire_arm(u32 chid)
{
	tgm_ramfc_write(chid, TGM_RAMFC_ACQUIRE, SWITCH_CHAN_NEW_ACQUIRE);
}

static int step_own(void *arg)
{
	return 1;
}

static void step_release(void *arg)
{
}

static void step_disable(void *arg)
{
	host.enabled = 0;
}

static void step_enable(void *arg)
{
	host.enabled = 1;
	host_load();
}

static int step_preempt(void *arg)
{
	host_save();
	return 0;
}

struct step_write {
	void (*write)(void *arg);
	void *arg;
};

static void step_write(void *arg)
{
	struct step_write *w = arg;

	w->write(w->arg);
}

int tgm_sched_ramfc(struct nvkm_device *dev, unsigned int chid, void (*write)(void *arg), void *arg,
		    int wait)
{
	struct step_write w = {write, arg};
	const struct tgm_ramfc_steps steps = {&w,          step_own,     step_release, step_disable,
					      step_enable, step_preempt, step_write};

	host.quiesced_waited += wait != 0;
	return chid == host.chid ? tgm_ramfc_write_quiesced(&steps) : -ENOENT;
}

int nvif_object_ctor(struct nvif_object *parent, const char *name, u32 handle, s32 oclass,
		     void *data, u32 size, struct nvif_object *object)
{
	memset(object, 0, sizeof(*object));
	object->oclass = oclass;
	if (oclass == MAXWELL_CHANNEL_GPFIFO_A) {
		((struct nvif_chan_v0 *)data)->chid = (u8)host.chid;
		host.memory.timeslice = RAMFC_TIMESLICE_DEFAULT;
		host.memory.acquire = SWITCH_CHAN_NEW_NOUVEAU_ACQUIRE;
		host.enabled = 1;
		host_load();
	} else {
		step_disable(NULL);
		step_preempt(NULL);
		step_enable(NULL);
	}
	return 0;
}

void nvif_object_dtor(struct nvif_object *object)
{
}

int nvif_object_map(struct nvif_object *object, void *argv, u32 argc)
{
	object->map.ptr = userd;
	object->map.size = sizeof(userd);
	return 0;
}

void nvif_object_unmap(struct nvif_object *object)
{
	object->map.ptr = NULL;
}

static void push_kick(struct nvif_push *push)
{
	push->cur = push->bgn = push_words;
}

int nvif_chan906f_ctor(struct nvif_chan *chan, void *userd_map, void *gpfifo, u32 gpfifo_size,
		       void *push, u64 push_addr, u32 push_size, void *sema, u64 sema_addr)
{
	chan->push.bgn = chan->push.cur = push_words;
	chan->push.end = push_words + ARRAY_SIZE(push_words);
	chan->push.kick = push_kick;
	return 0;
}

int nvif_chan_gpfifo_wait(struct nvif_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	return 0;
}

int nvif_event_ctor_(struct nvif_object *parent, const char *name, u32 handle, nvif_event_func func,
		     bool wait, struct nvif_event_v0 *args, u32 argc, bool warn,
		     struct nvif_event *event)
{
	return 0;
}

int nvif_event_allow(struct nvif_event *event)
{
	return 0;
}

void nvif_event_dtor(struct nvif_event *event)
{
}

u64 nvif_fifo_runlist(struct nvif_device *dev, u64 engine)
{
	return 1;
}

int nvif_mem_ctor_type(struct nvif_mmu *mmu, const char *name, s32 oclass, int type, u8 page,
		       u64 size, void *argv, u32 argc, struct nvif_mem *mem)
{
	return 0;
}

void nvif_mem_dtor(struct nvif_mem *mem)
{
}

int nvif_vmm_get(struct nvif_vmm *vmm, enum nvif_vmm_get type, bool sparse, u8 page, u8 align,
		 u64 size, struct nvif_vma *vma)
{
	vma->addr = 0x100000;
	vma->size = size;
	return 0;
}

int nvif_vmm_map(struct nvif_vmm *vmm, u64 addr, u64 size, void *argv, u32 argc,
		 struct nvif_mem *mem, u64 offset)
{
	return 0;
}

void nvif_vmm_put(struct nvif_vmm *vmm, struct nvif_vma *vma)
{
}

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t gfp,
		      unsigned long attrs)
{
	*handle = 0x80000000;
	return switch_exec_host_alloc(size);
}

void dma_free_attrs(struct device *dev, size_t size, void *va, dma_addr_t handle,
		    unsigned long attrs)
{
	switch_exec_host_free(va);
}

void *switch_linux_malloc(uint64_t size, int zero)
{
	return switch_exec_host_alloc(size);
}

void switch_linux_free(const void *block)
{
	switch_exec_host_free((void *)block);
}

bool switch_linux_call_enter(void *call)
{
	return false;
}

bool switch_linux_call_cancel(void *call)
{
	return false;
}

ssize_t strscpy(char *dst, const char *src, size_t size)
{
	size_t n = 0;

	while (n + 1 < size && src[n]) {
		dst[n] = src[n];
		n++;
	}
	dst[n] = 0;
	return (ssize_t)n;
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

void tgm_gpu_lock(void)
{
	mutex_lock(&tgm_gpu.lock);
}

void tgm_gate_enter(void)
{
}

int tgm_all_clients(void)
{
	return 1;
}

uint32_t tgm_chan_completed(struct tgm_chan *chan)
{
	return READ_ONCE(*chan->fence_cpu);
}

int switch_gpu_timeline_init(void)
{
	return 0;
}

struct switch_gpu_timeline *switch_gpu_timeline_create(uint64_t uid,
						       const volatile uint32_t *completed)
{
	return (struct switch_gpu_timeline *)&timeline;
}

void switch_gpu_timeline_close(struct switch_gpu_timeline *t)
{
}

void switch_gpu_timeline_token(struct switch_gpu_timeline *t, uint32_t value,
			       struct switch_gpu_backing_token *token)
{
}

uint64_t tgm_hold_held_ns(unsigned int chid, uint64_t now)
{
	return 0;
}

void tgm_hold_forget(unsigned int chid)
{
}

void tgm_sched_level(struct nvkm_device *dev, unsigned int chid, unsigned int to)
{
}

void tgm_sched_forget(struct nvkm_device *dev, unsigned int chid)
{
}

void tgm_fault_publish(struct tgm_chan *chan)
{
}

void tgm_fault_unpublish(struct tgm_chan *chan)
{
}

void switch_gpu_chan_named(const struct tgm_chan *chan)
{
}

void tgm_stats_gr_bind(struct tgm_chan *chan)
{
}

void tgm_stats_gr_unbind(struct tgm_chan *chan)
{
}

uint32_t tgm_chan_waiter(struct tgm_chan *chan)
{
	return 0;
}

uint32_t tgm_waiter_seq(uint32_t waiter)
{
	return 0;
}

void nvxnu_pass_barrier(void)
{
}

void nvxnu_wword_wake(struct switch_gpu_wake_word *word, int pass)
{
}

u64 nvkm_timer_read(struct nvkm_timer *timer)
{
	return 0;
}

unsigned long nvxnu_pool_dirty_free_pages(void)
{
	return 0;
}

void nvxnu_pool_free_dirty(unsigned long long pa, unsigned long size)
{
}

void nvxnu_pool_free_zeroed(unsigned long long pa, unsigned long size)
{
}

void nvxnu_wake(void)
{
}

void switch_gpu_compositor_job(uint64_t ns)
{
}

void switch_gpu_compositor_sync(uint32_t syncs, uint64_t ns, int behind)
{
}

void switch_gpu_events_completed(void)
{
}

void tgm_chan_hung(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	nvxnu_panic("tgm_chan_hung: %s", who);
}

/* nvkm's kill reads the channel's criticality (tgm_chan_killed); the model's kill is immediate */
void tgm_chan_isolate(struct tgm_chan *chan, const char *report, u64 now)
{
	isolated_critical = chan->critical;
	chan->error = 4;
}

int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)
{
	return 0;
}

void tgm_chan_need(struct tgm_chan *chan, uint32_t seqno)
{
}

void tgm_chan_quiet_wake(struct tgm_chan *chan)
{
}

int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	return 0;
}

void tgm_clock_anchors_fill(u32 page)
{
}

uint32_t switch_gpu_channel_memory_refused, switch_gpu_channel_creates;

void switch_gpu_channel_epoch_move(void)
{
}

void switch_gpu_channel_epoch_fill(volatile uint32_t *page)
{
}

void tgm_copy_reset_nonstall(void)
{
}

void tgm_deadline_clock(uint64_t ns, uint64_t pt)
{
}

void tgm_deadline_progress(uint64_t ns, int biased)
{
}

uint64_t tgm_deadline_pt_ns(uint64_t pt_span)
{
	return 0;
}

void tgm_hold_released(unsigned int chid, uint64_t end_pt, int job_last)
{
}

void tgm_hold_sampled(unsigned int chid, uint64_t asked_pt, uint64_t start_pt)
{
}

int tgm_mem_new(struct tgm_vm *vm_, const uint64_t *pas, uint32_t npages, struct tgm_mem **pmem)
{
	return -ENOMEM;
}

void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)
{
	return NULL;
}

void tgm_stats_gr_context(void)
{
}

int tgm_vm_kmap_raw(struct tgm_vm *vm_, struct tgm_mem *mem, uint64_t bytes, int read_only,
		    uint64_t *va)
{
	return -ENOMEM;
}

int tgm_waiter_idle(uint32_t waiter, uint32_t seq, uint64_t deadline_ns)
{
	return 0;
}

void tgm_waiter_unwoken(uint32_t waiter, uint32_t seq, uint32_t seqno)
{
}
