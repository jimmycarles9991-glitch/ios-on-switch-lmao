/* SPDX-License-Identifier: MIT */
/*
 * C interface between the IOKit layer (TegraGM20B.cpp, TegraGM20BUserClient.cpp) and the nvkm/nvif layer
 * (gpu_*.c). Plain C types only; every object is opaque. All calls may block; call from thread context.
 */
#ifndef TEGRA_GM20B_GPU_H
#define TEGRA_GM20B_GPU_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Kernel-owned part of every GPU address space (CONTRACT.md §4.3): [TGM_KERNEL_VA, TGM_VA_LIMIT). */
#define TGM_USER_VA_START 0x1000ULL
#define TGM_KERNEL_VA (1ULL << 39)
#define TGM_VA_LIMIT (1ULL << 40)

struct tgm_vm;
struct tgm_mem;
struct tgm_chan;

struct tgm_info {
	uint32_t boot0;
	uint16_t chipset;
	uint8_t gpc_count;
	uint16_t tpc_count;
	uint32_t comptag_lines; /* 0: no compression (TegraGM20BComp.h) */
};

/* Bring the GPU up to `stage` (1 = M1 power/ring/GPCPLL, 2 = + ACR/PMU/FECS/GPCCS, 3 = + channel/CE/fence
 * self-test). Writes one human-readable result line into `line`. Returns 0 or a negative errno. */
int tgm_bringup(int stage, char *line, unsigned int len);
/* System sleep (gpu_power.c "system sleep", TegraGM20BSleep.h): the GPU off and back, for whatever stage the finished bring-up
 * reached; each step is one tgm_journal() line, and a step that cannot be done panics with one line. */
void tgm_sleep(void);
void tgm_wake(void);
/* Rail-gating (TegraGM20BRail.h), thread context. down: the GPU off with tgm_gpu.lock released, 0 or -EBUSY (no M3); up:
 * back if it is down (a tgm_gpu.lock or gate caller wakes it the same way, under the lock it owns); railed: whether it is
 * down. sc7_hold/release: system sleep with the GPU down holds tgm_gpu.lock across it on SwitchSuspend's thread (1), or
 * takes tgm_sleep's path (0). idle: gpu_clock.c's idle clock reached (its work item); idle_now: at the idle clock, or none. */
int tgm_rail_down(void);
void tgm_rail_up(void);
int tgm_railed(void);
int tgm_rail_sc7_hold(void);
void tgm_rail_sc7_release(void);
void tgm_rail_idle(void);
int tgm_idle_now(void);
/* TegraGM20B.cpp, around every rail-up, under tgm_gpu.lock (never block): one starts (SwitchThermal holds its CPU raise), one
 * is done (`demand`: work woke the GPU, not the display). */
void tgm_rail_moving(void);
void tgm_rail_moved(int demand);
/* One power-journal line (TegraGM20B.cpp: drivers/SwitchSuspend switch_power_journal). Thread context. */
void tgm_journal_impl(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define tgm_journal(...)                               \
	do {                                           \
		if (TGM_STATS)                         \
			tgm_journal_impl(__VA_ARGS__); \
	} while (0)
int tgm_stage(void);
void tgm_get_info(struct tgm_info *info);
uint64_t tgm_timestamp_ns(void);
/* Waiting for a channel's fence (interrupt-driven from stage 1 on; no polled mode): with the channel valid, take
 * waiter = tgm_chan_waiter(chan) and seq = tgm_waiter_seq(waiter), publish the seqno (tgm_chan_want), then test the
 * fence; tgm_waiter_idle() sleeps until that channel's fence reaches the lowest seqno published, it is killed or
 * destroyed (1), or deadline_ns (the absolutetime_to_nanoseconds(mach_absolute_time()) clock) passes (0). The waiter
 * stays valid after the channel is gone. Only a job's own fence interrupts, and a quiet job's only once tgm_chan_want
 * asked for it: a seqno is announced once the fence passes the end of the first job at or after it that interrupts
 * (tgm_chan_announced). One found announced after tgm_waiter_idle() returned 0 was not woken by its interrupt:
 * tgm_waiter_unwoken() allows 50 ms for the wake, then panics. */
uint32_t tgm_chan_waiter(struct tgm_chan *chan);
void tgm_chan_want(struct tgm_chan *chan, uint32_t seqno);
int tgm_chan_announced(struct tgm_chan *chan, uint32_t seqno);
uint32_t tgm_waiter_seq(uint32_t waiter);
int tgm_waiter_idle(uint32_t waiter, uint32_t seq, uint64_t deadline_ns);
void tgm_waiter_unwoken(uint32_t waiter, uint32_t seq, uint32_t seqno);

/* GPU memory pool (contiguous, write-combined in the kernel, below 4 GiB) */
/* align: at least the VM page (0 for just that) */
void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa);
void *tgm_pool_alloc_ready(uint64_t bytes, uint64_t align, uint64_t reserve, uint64_t *pa);
void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa);
/* A compressible BO's memory: `bytes` (a 128 KiB multiple) as zeroed 128 KiB-aligned 128 KiB chunks of the pool, in no
 * order and adjacent or not; `pas` gets every 4 KiB page. 0, or -ENOMEM with nothing taken.
 * tgm_pool_free_chunks gives them back dirty, for the scrubber. */
int tgm_pool_alloc_chunks(uint64_t bytes, uint64_t *pas);
void tgm_pool_free_chunks(const uint64_t *pas, uint32_t npages);

/* Address spaces: one per user-client connection, which claims it (tgm_vm_claim_connection): only channels made for
 * that owner may address it, so BoFree's wait for the connection's channels covers every channel that can reach its
 * pages. The kernel's own copiers claim theirs with tgm_vm_claim and an owner that is not an AGXDeviceUserClient: a
 * hang's line reads pushes through a connection's claim only (tgm_client_interlocks). */
int tgm_vm_new(struct tgm_vm **pvm);
void tgm_vm_claim(struct tgm_vm *vm, const void *owner);
void tgm_vm_claim_connection(struct tgm_vm *vm, const void *connection);
void tgm_vm_del(struct tgm_vm **pvm);
/* Wrap physical pages as GPU memory. `pas` (one entry per 4 KiB page) must stay valid until tgm_mem_del. lock_wait_ns
 * (or NULL): with the stats, how long the call waited for tgm_gpu.lock. */
int tgm_mem_new(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages, struct tgm_mem **pmem);
int tgm_mem_new_waited(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages,
		       struct tgm_mem **pmem, uint64_t *lock_wait_ns);
/* The same for pages no other memory object will ever name (a BO's own): such memory may take compressible kinds. */
int tgm_mem_new_exclusive(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages,
			  struct tgm_mem **pmem);
int tgm_mem_can_compress(const struct tgm_mem *mem);
void tgm_mem_del(struct tgm_mem **pmem);
/* User range [TGM_USER_VA_START, TGM_KERNEL_VA): raw binds. read_only: the GPU may only read the range (a write is an
 * MMU fault). tgm_vm_map and tgm_vm_unmap invalidate the L2 and the TLB before they return; tgm_vm_map_ordered (a
 * claimed space's) leaves both to the next submission of each of its channels (TegraGM20BTlb.h). A compressible kind
 * needs exclusive memory and whole 128 KiB pages (-EINVAL otherwise, -ENOSPC once the memory could get no comptags);
 * *page_shift is the GMMU page the map used, which its unmap must name. */
int tgm_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem, uint64_t offset,
	       uint8_t kind, int read_only);
int tgm_vm_map_ordered(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		       uint64_t offset, uint8_t kind, int read_only, uint8_t *page_shift);
int tgm_vm_unmap(struct tgm_vm *vm, uint64_t va, uint64_t range);
int tgm_vm_unmap_pages(struct tgm_vm *vm, uint64_t va, uint64_t range, uint8_t page_shift);
struct switch_gpu_mapping;
int switch_gpu_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		      uint64_t offset, uint8_t kind, int read_only,
		      struct switch_gpu_mapping **mapping);
int switch_gpu_vm_map_volatile(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
			       uint64_t offset, uint8_t kind, int read_only, int vol,
			       struct switch_gpu_mapping **mapping);
void switch_gpu_vm_unmap(struct tgm_vm *vm, struct switch_gpu_mapping **mapping);
/* Cuts [va, va + range) out of *mapping, made by switch_gpu_vm_map_volatile or _prepare of mem with kind, and consumes
 * it, changing no PTE. What stays below and above the range comes back as *below and *above, the range's own entries as
 * *taken (each NULL when empty). A 128 KiB page an edge cuts goes whole to *taken, keeping its PTE until *taken is
 * unmapped; its part outside the range comes back in *below or *above as 4 KiB entries pending switch_gpu_vm_install,
 * which the caller runs once that PTE is gone. A compressed mapping cut inside a 128 KiB page is refused, and on any
 * error *mapping is untouched. */
int switch_gpu_vm_split(struct tgm_vm *vm, struct switch_gpu_mapping **mapping, struct tgm_mem *mem,
			uint8_t kind, int read_only, int vol, uint64_t va, uint64_t range,
			struct switch_gpu_mapping **below, struct switch_gpu_mapping **taken,
			struct switch_gpu_mapping **above);
/* switch_gpu_vm_map_volatile's runs, every entry pending: no PTE is written until switch_gpu_vm_install. */
int switch_gpu_vm_prepare(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
			  uint64_t offset, uint8_t kind, struct switch_gpu_mapping **mapping);
/* Maps a mapping's pending entries (GPU-ordered, as switch_gpu_vm_map_volatile). One that cannot be mapped leaves the
 * mapping, unmapped; -ENOMEM then. switch_gpu_vm_unmap skips pending entries. */
int switch_gpu_vm_install(struct tgm_vm *vm, struct switch_gpu_mapping *mapping,
			  struct tgm_mem *mem, uint8_t kind, int read_only, int vol);
int switch_gpu_mapping_pending(const struct switch_gpu_mapping *mapping);
/* whether any entry has PTEs */
int switch_gpu_mapping_installed(const struct switch_gpu_mapping *mapping);
/* The connection's bind epoch: while *cpu is behind target, every submission in vm first acquires (on the GPU) the
 * word at va until it reaches target, then invalidates the TLB (switch_gpu bind epoch, SwitchGpuClientRetirement.h). */
void tgm_vm_bind_wait(struct tgm_vm *vm, uint64_t va, const volatile uint32_t *cpu,
		      uint32_t target);
/* The VA range from a mapping's first entry to the end of its last. */
int switch_gpu_mapping_span(const struct switch_gpu_mapping *mapping, uint64_t *va,
			    uint64_t *bytes);
/* "va+bytes/page shift" per entry, for a log line. */
void switch_gpu_mapping_describe(const struct switch_gpu_mapping *mapping, char *out, size_t size);
/* A zero-bandwidth-clear entry (ds, l2 as TegraGM20BAbiZbc has them): its table index, or -EINVAL (format), -ENOSPC
 * (table full), -ENODEV (no compression). */
int tgm_zbc_add(int depth, uint32_t format, const uint32_t ds[4], const uint32_t l2[4]);
/* Zcull's layout (TegraGM20BZcullABI.h method 'ZC'), or -EOPNOTSUPP where the GPU reported no zcull RAM. */
struct TegraGM20BAbiZcullInfo;
int tgm_zcull_info(struct TegraGM20BAbiZcullInfo *out);
/* 0 when booted with switch-no-gpu-compression (TegraGM20B.cpp): GetInfo leaves TegraGM20BInfoCompression clear, so no
 * client binds a compressible kind or adds ZBC entries. */
int switch_gpu_compression_on(void);
/* Kernel range: map `mem` somewhere at or above TGM_KERNEL_VA and return the address. */
int tgm_vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, uint64_t *va);

/* Channels: GPFIFO on the GR runlist, subchannels 0..4 = 3D, compute, M2MF, 2D, copy. The channel releases
 * its seqno (4 bytes) to fence_gpu_va; fence_cpu is the kernel's write-combined view of the same word. owner (the
  * creating process) names it in nvkm's reports and our panic lines. critical: the compositor's (with a vm_owner: its
 * connection asked for the top GPU priority, tgm_chan_set_level) or the
 * kernel's; the compositor's get its GPU priority (TegraGM20BSched.h). A GPU error nvkm pins on a client's channel, or a
 * hang the GPU is stuck on, kills only that channel (tgm_chan_error then non-zero, its fence released) instead of
 * panicking. */
int tgm_chan_new(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		 uint64_t fence_gpu_va, const char *owner, int critical, struct tgm_chan **pchan);
int tgm_chan_new_copy(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		      uint64_t fence_gpu_va, const char *owner, int critical,
		      struct tgm_chan **pchan);
/* The channel's runlist level (TegraGM20BSchedPolicy.h enum tgm_level), from its connection's GPU priority: high makes it
 * the compositor's (its kicks preempt app work, the deadline's holds leave it, the compositor's timeslice). Serialized
 * with the channel's submissions. */
void tgm_chan_set_level(struct tgm_chan *chan, unsigned int level);
/* GPU for every process (client fault isolation, 64 connections, client pool quota) */
int tgm_all_clients(void);
/* pool bytes not allocated */
uint64_t tgm_pool_avail(void);
/* Zero a pool BO's range (write-combined; off any client's thread) and give it back clean, so the next tgm_pool_alloc of
 * those pages does no memset. tgm_pool_free gives a range back as it is. */
void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa);
/* Give a freed pool range back zeroed by the GPU's copy engine (gpu_scrub.c "pool scrub"), without the CPU waiting: 0 if queued
 * (the range comes back clean once the copy engine's fence passes), else (no scrubber, its queue full, or not a pool
 * range) the caller zeroes it. */
int tgm_pool_scrub(uint64_t pa, uint64_t bytes);
void tgm_chan_del(struct tgm_chan **pchan);

struct tgm_push {
	uint64_t va;
	uint32_t dwords;
	uint8_t no_prefetch;
	uint8_t incomplete;
	uint8_t fence; /* an internal fence after it (TegraGM20BPushFence, SwitchGpuJobFences.h) */
	uint8_t reserved;
};
/* Append the pushes, then the fence (WFI semaphore release of the next seqno), then kick. User mappings are
 * L2-cacheable: the L2's clean lines are invalidated on the GPU before the pushes are fetched and its dirty lines
 * written back before the seqno is released (gpu_inline.h l2_on). The pushes run after what the acquires tgm_chan_order
 * queued for the channel wait for. -EBUSY when the GPFIFO stays full for 1 s (a panic from the self-test on), -EIO on a
 * dead channel. */
struct tgm_prof;
int tgm_chan_exec(struct tgm_chan *chan, const struct tgm_push *push, uint32_t count,
		  uint32_t *seqno, const struct tgm_prof *prof);
struct switch_gpu_backing_token;
uint64_t switch_gpu_chan_identity(struct tgm_chan *chan);
void switch_gpu_chan_token(struct tgm_chan *chan, uint32_t seqno,
			   struct switch_gpu_backing_token *token);
void switch_gpu_chan_next_token(struct tgm_chan *chan, struct switch_gpu_backing_token *token);
/* The token of the seqno the next job's own fence releases, after its `fences` internal ones. */
void switch_gpu_chan_job_token(struct tgm_chan *chan, uint32_t fences,
			       struct switch_gpu_backing_token *token);
/* Enter before taking backing locks; leave after commit/abort. Room waits require those locks released. */
void switch_gpu_chan_submit_enter(struct tgm_chan *chan);
void switch_gpu_chan_submit_leave(void);
int switch_gpu_chan_wait_submission_room(struct tgm_chan *chan, const struct tgm_push *push,
					 uint32_t count, uint32_t dependency_count,
					 const struct tgm_prof *prof);
int switch_gpu_chan_exec(struct tgm_chan *chan, const struct tgm_push *push, uint32_t count,
			 const struct switch_gpu_backing_token *dependencies,
			 uint32_t dependency_count, uint32_t *seqno, const struct tgm_prof *prof);

enum {
	SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT = 1u << 0,
	SWITCH_GPU_SUBMIT_HOST_WRITTEN = 1u << 1,
	SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY = 1u << 2,
	SWITCH_GPU_SUBMIT_FORCE_FENCE = 1u << 3,
	/* the job's end raises no interrupt unless a waiter asked for one (SwitchGpuQuiet.h); only on a channel with an owner
	 * that pushes wake entries (tgm_chan_on_wake) */
	SWITCH_GPU_SUBMIT_QUIET = 1u << 4,
};

int switch_gpu_chan_exec_visibility(struct tgm_chan *chan, const struct tgm_push *push,
				    uint32_t count,
				    const struct switch_gpu_backing_token *dependencies,
				    uint32_t dependency_count, uint32_t *seqno,
				    const struct tgm_prof *prof, uint32_t visibility);

/* SubmitJob: switch_gpu_chan_exec_visibility after an acquire on each sibling's fence word reaching `seqno`. A sibling is
 * another channel of the same address space, kept alive by the caller until this returns. */
enum {
	SWITCH_GPU_MAX_SIBLINGS = 8
};

struct switch_gpu_sibling_wait {
	struct tgm_chan *source;
	uint32_t seqno;
};

/* A word the job's fence push sets to `value` after the job (a writer promise it keeps, SwitchGpuWriterPromise.h) */
enum {
	SWITCH_GPU_MAX_RELEASES = 4
};

struct switch_gpu_release {
	uint64_t va;
	uint32_t value;
};

int switch_gpu_chan_exec_job(struct tgm_chan *chan, const struct tgm_push *push, uint32_t count,
			     const struct switch_gpu_backing_token *dependencies,
			     uint32_t dependency_count,
			     const struct switch_gpu_sibling_wait *siblings, uint32_t sibling_count,
			     uint32_t *seqno, const struct tgm_prof *prof, uint32_t visibility,
			     const struct switch_gpu_release *releases, uint32_t release_count);
/* switch_gpu_chan_exec_job with acquires on event words as well (SwitchGpuWordWait.h) */
struct switch_gpu_word_wait;
int switch_gpu_chan_exec_events(struct tgm_chan *chan, const struct tgm_push *push, uint32_t count,
				const struct switch_gpu_backing_token *dependencies,
				uint32_t dependency_count,
				const struct switch_gpu_sibling_wait *siblings,
				uint32_t sibling_count, const struct switch_gpu_word_wait *words,
				uint32_t word_count, uint32_t *seqno, const struct tgm_prof *prof,
				uint32_t visibility, const struct switch_gpu_release *releases,
				uint32_t release_count);
/* The channel's jobs through `seqno` release event signals mid-job: their nonstall interrupts run event retirement. */
void switch_gpu_chan_event_releases(struct tgm_chan *chan, uint32_t seqno);
struct TegraGM20BAbiFenceSemantic;
void switch_gpu_chan_semantic_visibility(struct tgm_chan *,
					 volatile struct TegraGM20BAbiFenceSemantic *);
void switch_gpu_chan_visibility(struct tgm_chan *chan, volatile uint32_t *visible_cpu,
				uint64_t visible_gpu);
void switch_gpu_fence_hold(const volatile uint32_t *fence);
void switch_gpu_fence_put(const volatile uint32_t *fence);
/* A word of the user fence pages by its byte offset in the pool (what every address space's fence view adds to its
 * base), or NULL; and the offset of one */
volatile uint32_t *tgm_fence_word(uint32_t offset);
uint32_t tgm_fence_offset(const volatile uint32_t *word);
/* The owner of the fence page holding the word at `offset`: a page freed and handed to another connection changes it */
uint32_t tgm_fence_owner(uint32_t offset);
/* Writer promises given up on before their job wrote the surface (SwitchGpuWriterPromise.h): marked, and whether one is;
 * and, for a direct present watching one (SwitchGpuPresentWatch.h), the job that keeps it submitted, or its word
 * released from the CPU */
void tgm_promise_abandon(uint32_t offset, uint32_t value);
int tgm_promise_abandoned(uint32_t offset, uint32_t value);
/* The give-up of a promise nobody keeps (SwitchGpuPromiseGiveUp.h): its work items, before any promise is made; a
 * reader that parks on one (reader: a GPU job; else a waiter that leaves itself): 1 parked, 0 nothing to wait for;
 * and the job that keeps one, submitted on `chan` */
int tgm_promise_open(void);
struct switch_gpu_giveup_job;
struct switch_gpu_promise_claim;
/* A make's guard against a give-up raising the same word (SwitchGpuWriterPromise.h), for the page whose promise slot 0
 * is `first` */
void tgm_promise_claim(const volatile uint32_t *first, struct switch_gpu_promise_claim *claim);
int tgm_promise_park(uint32_t offset, uint32_t value, const char *waiter, const char *who,
		     const struct switch_gpu_giveup_job *reader, int display);
void tgm_promise_leave(uint32_t offset, uint32_t value, int display);
/* The GPU's power-down (system sleep and rail-gating) and power-up: the give-up's call stays armed and touches no
 * register meanwhile, and every promise still waited on gets its whole second from the power-up */
void tgm_promise_suspend(void);
void tgm_promise_resume(void);
void tgm_promise_kept(uint32_t offset, uint32_t value, struct tgm_chan *chan,
		      const struct switch_gpu_backing_token *job);
void tgm_present_promise_kept(uint32_t offset, uint32_t value,
			      const struct switch_gpu_backing_token *producer);
void tgm_present_promise_released(uint32_t offset, uint32_t value);
/* The stats' push profile (gpu_fenced.h "push profile"): `prof` (or NULL) names each push of a sampled submission by what it
 * asks of the GPU (TegraGM20BUserClient.cpp scanPushes); the submission then stamps the GPU's time after each push whose
 * class is not TGM_PC_SKIP, and each window's "gpuprof" line gives every process's GPU time by class. Draws and clears
 * are split by their pass's first color target: its size (full >= 600k samples, mid >= 100k, small), pitch-linear or
 * block-linear, multisampled or not. */
#define TGM_PROF_MAX 128

enum {
	TGM_PC_DRAW = 0,   /* + size * 4 + pitch * 2 + msaa */
	TGM_PC_CLEAR = 12, /* + the same */
	TGM_PC_PASS = 24,  /* begins a render pass, draws and clears nothing */
	TGM_PC_SYNC = 25,  /* only waits, cache invalidates and host flushes */
	TGM_PC_COMPUTE = 26,
	TGM_PC_COPY = 27,
	TGM_PC_2D = 28,
	TGM_PC_STATE = 29, /* state and anything else */
	TGM_PC_FENCE = 30, /* the kernel's fence push (WFI, L2 flush): the end stamp's share */
	TGM_PC_COUNT = 31,
	TGM_PC_SKIP =
		0xff, /* no stamp after this push: a method may continue into the next one, or it was not read */
};

struct tgm_prof {
	uint8_t cls[TGM_PROF_MAX];
	/* each push's draws, and those shaded per sample (PushCounts::sampleDraws) */
	uint16_t draws[TGM_PROF_MAX], sampleDraws[TGM_PROF_MAX];
	/* its pass's colour target 0 in thousands of samples, and its interlock enables (SwitchGpuProfileSequence.h) */
	uint16_t kpx[TGM_PROF_MAX];
	uint8_t interlocks[TGM_PROF_MAX];
};

uint32_t tgm_chan_submitted(struct tgm_chan *chan);
uint32_t tgm_chan_completed(struct tgm_chan *chan);
/* `fenced(arg)` from the interrupt pass whenever the channel's fence may have moved, and once when it is killed (its
 * fence then released): under nvkm's event lock, so it must not block (a thread call does the work). */
void tgm_chan_on_fence(struct tgm_chan *chan, void (*fenced)(void *arg), void *arg);
/* Quiet job ends (SwitchGpuQuiet.h). on_wake: the channel's owner takes asks for wake entries; `wake(arg, tag)` must not
 * block (it schedules a thread call that runs tgm_chan_quiet_wake under the owner's submission order). need: anything
 * that will sleep until the channel reaches `seqno` asks first, then tests the fence (any context a thread call may be
 * entered from). need_uid: the same for the GM20B channel of that uid. need_tokens: the same for each GM20B channel the
 * backing tokens name by uid and reset generation (switch_gpu_quiet_token_on); another engine's token asks nothing. Both
 * take tgm_live_lock. quiet_wake: the owner, with no submission of its on the channel under way, outside the gate. */
void tgm_chan_on_wake(struct tgm_chan *chan, void (*wake)(void *arg, uint32_t tag), void *arg,
		      uint32_t tag);
void tgm_chan_need(struct tgm_chan *chan, uint32_t seqno);
void tgm_chan_need_uid(uint32_t uid, uint32_t seqno);
void tgm_chan_need_tokens(const struct switch_gpu_backing_token *tokens, uint32_t count);
void tgm_chan_quiet_wake(struct tgm_chan *chan);
/* The group a channel belongs to (its connection's task): tgm_chan_order relates the channels of one group. */
void tgm_chan_set_group(struct tgm_chan *chan, const void *group);
/* GPU-side order between the channels of chan's group (TegraGM20BABI.h method 14), no CPU wait: semaphore acquires the
 * channels' next tgm_chan_exec starts with. TGM_ORDER_AFTER: chan's next submission waits for the last submission of every
 * other channel of the group with work pending; TGM_ORDER_BEFORE: every other channel's next submission waits for chan to
 * complete `seqno` (0: its last submission; a later one is -EINVAL). *queued: the acquires queued. -ENODEV: an address
 * space has no view of the fence pages; -ENOSPC: a channel has TGM_ORDER_MAX acquires queued from other channels. */
#define TGM_ORDER_AFTER 1u
#define TGM_ORDER_BEFORE 2u
#define TGM_ORDER_MAX 16
int tgm_chan_order(struct tgm_chan *chan, uint32_t flags, uint32_t seqno, uint32_t *queued);
/* 0 healthy; else the CONTRACT.md error code (1 MMU fault, 2 GR exception, 3 host error, 4 timeout). Only before the
 * self-test passed, or under client fault isolation for a client's channel: an error on a kernel channel panics
 * (nvkm's report in the panic string). */
uint32_t tgm_chan_error(struct tgm_chan *chan);
/* The completed seqno, then whether the channel had failed by then: tgm_chan_killed publishes its error before it advances
 * the fence over the work it abandoned, so a seqno read here with *failed 0 was executed. */
uint32_t tgm_chan_settled(struct tgm_chan *chan, int *failed);
/* For error 1: the GPU VA its MMU fault report names (0: none named). */
uint64_t tgm_chan_fault_va(struct tgm_chan *chan);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
/* Debug kernels: when nvkm's recovery killed the channel (nvxnu_nanotime; 0: not killed). */
uint64_t tgm_chan_killed_ns(struct tgm_chan *chan);
#endif
/* Panic naming `who` and the channel's state (fence, GPFIFO GET/PUT, PGRAPH/engine/PBDMA status, MC error registers):
 * work submitted on it did not complete and nvkm reported no error. */
void tgm_chan_hung(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
	__attribute__((noreturn));
/* A wait for `seqno` on chan ran out after waited_ns: kills the client channel the GPU is stuck on (chan's error then
 * says so if it was chan) and returns; returns as well when nothing is stuck (slow work); panics (tgm_chan_hung) when
 * the GPU is stuck on a kernel channel or nothing at all progresses. The caller waits again. */
void tgm_chan_stalled(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns);
/* After a wait for `seqno` timed out: tgm_chan_stalled once the channel has completed nothing for 2 s while work is
 * pending; a single submission that runs that long is taken for a hang. Returns the nanoseconds after which the next
 * check can find the next step of that rule, 0 when the channel has nothing pending: a timer that drives the checks is
 * armed for that and for nothing sooner. */
uint64_t tgm_chan_check_progress(struct tgm_chan *chan, const char *who, uint32_t seqno);
/* A hang's line (TegraGM20BUserClient.cpp): the SET_PIXEL_SHADER_INTERLOCK_CONTROL methods with conflict detection on
 * in the `n` pushes at va[i] (dwords[i] long) of `client`'s connection (its space's tgm_vm_claim_connection), and the
 * last engine method among them (subchannel << 16 | method, 0: none; TegraGM20BPushScan.h lastEngine). Returns
 * the pushes it read (one in a host import has no kernel view), or -1 when the connection's lock is held by another
 * thread. */
int tgm_client_interlocks(const void *client, const uint64_t *va, const uint32_t *dwords,
			  uint32_t n, uint64_t *interlocks, uint32_t *last_engine);

/* For a CPU reader of GPU-written memory outside the GPU client (NativeLegacyDisplay's copy of a swapped IOSurface,
 * which calls it weakly): wait for every GPU submission made so far (all channels), then clean+invalidate
 * [va, va + bytes) of the reader's mapping. Returns at once, touching no cache, when no user channel exists.
 * 0; -ETIMEDOUT when a channel has not completed its work after timeout_ns (tgm_chan_stalled first, from the
 * self-test on: a stuck channel is killed and waited for). Thread context. */
int tegra_gm20b_sync_for_cpu(const void *va, uint64_t bytes, uint64_t timeout_ns);

/* Fence pages: one per connection, from one pool range the GPU present maps whole (gpu_chan.c). Zeroed; 0 or -ENOMEM. */
int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa);
void tgm_fence_page_put(const volatile uint32_t *va);

/* The kernel's own client channels whose writes other channels follow on the GPU (the copy engine's plane copies, the
 * tiling moves): each one's fence (a TegraGM20BAbiFence) is in the fence range every address space maps, as a
 * connection's channels' are, never handed out again. *page_pa is its 4 KiB page, which the channel's own space maps
 * writable, and *offset the fence's place in it. */
enum tgm_fence_kernel_channel {
	TGM_FENCE_KERNEL_COPY,
	TGM_FENCE_KERNEL_RELAYOUT,
	TGM_FENCE_KERNEL_BRIDGE,
	TGM_FENCE_KERNEL_CACHED_ZERO,
	TGM_FENCE_KERNEL_CHANNELS,
};

int tgm_fence_kernel(unsigned int which, volatile uint32_t **va, uint64_t *page_pa,
		     uint32_t *offset);
/* A channel's fence word is a TegraGM20BAbiFence: its kill record zeroed, for a channel about to be made on it. */
void tgm_fence_kill_clear(volatile uint32_t *fence);

/* GPU present (gpu_present.c "GPU present"): a kernel channel and address space, made at the first call (from the self-test on).
 * Mappings in it are volatile, at addresses the caller picks in [TGM_USER_VA_START, TGM_KERNEL_VA).
 * tgm_present_copy_dependencies queues one copy engine copy, pitch source (stride; src_block_log2 >= 0: block-linear,
 * width x 4 bytes across, blocks of 2^src_block_log2 GOBs) to a block-linear destination (dst_pitch bytes across, blocks
 * one GOB wide and 2^block_height_log2 GOBs high, 8-line GOBs; dst_va 0: no copy), after exactly the producer tokens
 * given, and returns at once: 0, or -EBUSY (one in flight), -EINVAL, -ENODEV. done(arg, status) runs exactly once for an
 * accepted present, from its completion work item, when its bytes are in DRAM: status 0, or the producers' failure.
 * One not done in 1 s is the present's deadline's (gpu_present_deadline.c).
 * tgm_present_outstanding: whether the copy accepted with `arg` is not completed yet (its callback not returned). */
int tgm_present_open(struct tgm_vm **pvm);
int tgm_present_map(uint64_t va, struct tgm_mem *mem, uint64_t bytes, uint8_t kind);
int tgm_present_unmap(uint64_t va, uint64_t bytes);
int tgm_present_copy_dependencies(uint64_t src_va, uint64_t stride, int32_t src_block_log2,
				  uint32_t width, uint32_t height, uint64_t dst_va,
				  uint32_t dst_pitch, uint32_t block_height_log2,
				  void (*done)(void *, int), void *arg,
				  const struct switch_gpu_backing_token *dependencies,
				  uint32_t dependency_count);
int tgm_present_outstanding(const void *arg);
/* tgm_present_copy_dependencies with no job when exactly one GM20B producer is left to wait for: its own interrupt
 * pass completes the present (gpu_present.c tgm_present_pass). 1: not watchable, queue the job instead. */
int tgm_present_watch_dependencies(void (*done)(void *, int), void *arg,
				   const struct switch_gpu_backing_token *dependencies,
				   uint32_t dependency_count);
void tgm_present_pass(void);
/* SwitchGpuPresentFilter.cpp: the armed watch's token, read by the nonstall interrupt's primary filter
 * (tgm_present_filter); attached once armed, detached at its claim before its tokens drop and before done runs. */
void tgm_present_filter_attach(const struct switch_gpu_backing_token *token);
void tgm_present_filter_detach(void);
void tgm_present_filter(void);
/* Host import bookkeeping: `pa` is the import's first page; add 1 when made, 0 when released (gpu_sync.c vm_imports). */
void tgm_vm_import_note(struct tgm_vm *vm, uint64_t pa, int add);

/* Clean+invalidate the CPU caches over pages of ordinary RAM (by physical address), so the GPU, which does not snoop
 * them, reads what the CPU wrote and no dirty line is later written back over what the GPU wrote. */
void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages);

/* Host-import coherency registry (TegraGM20BCoherency.cpp), kept coherent at every process's IOSurfaceLock/Unlock of
 * an imported surface (tegra_gm20b_iosurface_trap). add at import, with the first page, the IOSurface ID (0: not an
 * IOSurface) and the import's page list; it returns the entry the import holds (NULL without memory: no lazy
 * coherency). The importer cleans its pages itself, then says so (cleaned), unless *covered: a live import of the
 * same surface that finished its clean, kept clean since by the traps, already spans them. gpu_writes: the import does
 * not say the GPU only reads it (TegraGM20BImportGpuReads). put at release; wrote from Exec, for a writable import a
 * submission may have written: it marks only a surface the GPU may write. take_stats returns and zeroes the traps' lock
 * invalidations and unlock cleans (count, bytes each). */
struct tgm_coh;
int tgm_coherency_init(void);
struct tgm_coh *tgm_coherency_add(uint64_t pa, uint32_t surface_id, const uint64_t *pas,
				  uint32_t npages, int writable, int gpu_writes, int *covered);
/* SurfaceWrites (method 15): every entry of the surface counts submissions as GPU writes from now on. */
void tgm_coherency_gpu_writes(uint32_t surface_id);
void tgm_coherency_cleaned(struct tgm_coh *c);
struct switch_gpu_backing;
/* Success consumes the supplied surface reference; it keeps a parked tiled identity from being recycled. */
int tgm_coherency_pin(struct tgm_coh *c, struct switch_gpu_backing *backing, void *surface,
		      void (*release_surface)(void *));
void tgm_coherency_put(struct tgm_coh *c);
/* always: mark it even if the GPU only samples it (TegraGM20BPushImportsWritten). uid, seqno: the submission (channel
 * tgm_chan_uid, its fence seqno) that may have written it, which a CPU lock waits for. */
void tgm_coherency_wrote(struct tgm_coh *c, int always, uint32_t uid, uint32_t seqno);
/* The submissions an entry keeps as its writers: the raw path's channel and m2v's, in each of two processes. A later one
 * of the same channel takes its slot, and so does any once its writer completed; past that the entry loses track. */
#define TGM_COH_WRITERS 4
/* A submission whose ExecSurfaces (method 17) listed the surface as written: wrote with `always`, and a listed writer too,
 * which another channel's listed Exec waits for on the GPU. */
void tgm_coherency_wrote_listed(struct tgm_coh *c, uint32_t uid, uint32_t seqno);
/* The listed writers (uid << 32 | seqno) `c` names, done or not; *lost: nonzero when listed writers found no slot since
 * tgm_coherency_lost_seen(c, *lost) last cleared it (some submissions that may write it are not among them); *pa: the
 * surface's first page. */
uint32_t tgm_coherency_writers(const struct tgm_coh *c, uint64_t out[TGM_COH_WRITERS],
			       uint32_t *lost, uint64_t *pa);
/* Every channel that imports the surface completed what it had submitted: the count `lost` read goes, unless more were
 * lost since. */
void tgm_coherency_lost_seen(struct tgm_coh *c, uint32_t lost);
/* Also the locks that waited for GPU writes and the AvoidSync locks refused (count each). */
void tgm_coherency_take_stats(uint64_t out[6]);
/* XNU's iokit_user_client_trap, after an IOSurfaceRootUserClient trap ran: index 2 lock, 3 unlock (IOConnectTrap3 p1 the
 * surface ID, p2 the lock options). */
void tegra_gm20b_iosurface_trap(uint32_t index, uint32_t surface_id, uint32_t options);
/* Before an IOSurfaceLock trap (surface ID, lock options): returns after the GPU submissions that may write the surface
 * have completed; nonzero, at once, when kIOSurfaceLockAvoidSync is set and one has not (the lock fails with
 * kIOReturnCannotLock). */
int tegra_gm20b_iosurface_lock_will(uint32_t surface_id, uint32_t options);
/* Before an IOSurface lock (index 2) or unlock (3) trap through IOSurfaceRootUserClient `client`: a block-linear surface's
 * CPU view (TegraGM20BViews.cpp); nonzero refuses the lock (kIOReturnCannotLock). */
int tegra_gm20b_iosurface_view(void *client, uint32_t index, uint32_t surface_id, uint32_t options);
/* A channel's submissions as the coherency registry names them (gpu_sync.c): uid, a 32-bit channel id never reused while the
 * channel lives; done, whether submission seqno completed (a channel that is gone or was killed counts as done); wait,
 * until it is done, on the fence interrupt (a wait that runs long recovers what the GPU is stuck on, as
 * tegra_gm20b_sync_for_cpu's does); all, every live channel's submissions so far. */
uint32_t tgm_chan_uid(struct tgm_chan *chan);
int tgm_writer_done(uint32_t uid, uint32_t seqno);
void tgm_writer_wait(uint32_t uid, uint32_t seqno);
int tgm_writers_all_done(void);
void tgm_writers_wait_all(void);
/* SurfaceLayout (method 16) for the import holding `c`: the surface's layout, decided at the first call with `writes` while
 * `c` is its only import (the surface's contents tiled before it says so); 1 block-linear at this shape, 0 pitch, -1
 * block-linear at another shape. Sleeps on the copy engine's fence interrupt while it tiles. */
int tgm_coherency_layout(struct tgm_coh *c, uint32_t width, uint32_t height, uint32_t bpp,
			 uint32_t bytes_per_row, int writes);
/* The same for an asker that reads the decided layout's block height (SurfaceLayoutInfo): the decision may lower the block
 * height below NIL's to fit the surface's allocation (TegraGM20BLayoutFit.h), where tgm_coherency_layout's leaves that
 * shape pitch-linear. */
int tgm_coherency_layout_fitted(struct tgm_coh *c, uint32_t width, uint32_t height, uint32_t bpp,
				uint32_t bytes_per_row, int writes);

/* TegraGM20BTiled.cpp's stats, this window's (debug kernels): layouts decided block-linear, then pitch because the first
 * import only samples, because another import or a CPU lock existed, because the shape does not tile, because the
 * tiling move failed; the live block-linear surfaces and their bytes; the CPU views' scratch bytes held now and at most */
void tgm_tiled_take_stats(uint64_t out[9]);
/* The same window's relayouts (TegraGM20BTiled.h tgm_relayout_count): tiling moves skipped because nothing had written
 * the surface, untiles deferred to a CPU lock, deferred untiles a lock made, deferred surfaces gone block-linear, and the
 * moves made */
void tgm_tiled_take_relayouts(uint64_t out[5]);

/* TegraGM20BRelayout.cpp: one copy-engine move of a block-linear surface (TegraGM20BRelayout.h), behind its GPU writers
 * (a semaphore acquire on each that is still alive), returning once the bytes are in DRAM (sleeping on the fence
 * interrupt). 0 or -errno. Serialised; init once before. */
struct tgm_layout;

struct tgm_relayout_pages {
	const uint64_t *pas; /* one per 4 KiB page */
	uint32_t npages;
};

struct tgm_relayout_writer {
	uint32_t uid, seqno;
};

int tgm_relayout_init(void);
int tgm_relayout(const struct tgm_relayout_pages *surface, const struct tgm_relayout_pages *rows,
		 const struct tgm_layout *layout, uint32_t bytes_per_row,
		 int op /* TGM_RELAYOUT_* */, const struct tgm_relayout_writer *writers,
		 uint32_t nwriters);
/* TGM_RELAYOUT_TO_ROWS or TO_TILED by the CPU through the physical aperture, when tgm_relayout failed (the copy engine
 * faulted, or its channel or mappings could not be had), after sleeping on each writer's fence interrupt: it allocates
 * nothing and cannot fail, so a CPU lock that holds a scratch always has the rows, and what it wrote always reaches the
 * texels. */
void tgm_relayout_by_cpu(const struct tgm_relayout_pages *surface,
			 const struct tgm_relayout_pages *rows, const struct tgm_layout *layout,
			 uint32_t bytes_per_row, int op, const struct tgm_relayout_writer *writers,
			 uint32_t nwriters);
/* With the stats: the moves the CPU made for the copy engine since the last call (tgm_relayout_by_cpu, and a relayout
 * the CPU finished after a fault), and the surface bytes they covered. */
void tgm_relayout_cpu_stats(uint64_t *moves, uint64_t *bytes);
/* tgm_writer_wait for a kernel copy's submission that starts with semaphore acquires on `acquired` (in push order):
 * while one of them has not completed, the wait is late on that writer, which a long wait recovers, not on the copy. */
void tgm_copy_wait(uint32_t uid, uint32_t seqno, const struct tgm_relayout_writer *acquired,
		   uint32_t nacquired);
/* The GPU address in `vm` (one that maps every connection's fence page, as every tgm_vm_new space does) of channel uid's
 * fence seqno; -ENOENT when it is gone or killed (its work is complete or abandoned). */
int tgm_writer_fence_va(struct tgm_vm *vm, uint32_t uid, uint64_t *va);
/* Method 17's acquires (TegraGM20BFences.h): chan's next submission waits on the GPU for each writer (uid << 32 | seqno) of
 * another live channel that has not completed it, queued as tgm_chan_order's; follow_importers, for the last submission of
 * every other channel whose address space imports the surface whose first page is `pa` and has work pending (*pending:
 * how many). 0, or tgm_chan_order's -ENODEV (no view of the fence pages) or -ENOSPC (TGM_ORDER_MAX channels queued): the
 * rest go unqueued. */
int tgm_chan_follow(struct tgm_chan *chan, const uint64_t *writers, uint32_t n);
int tgm_chan_follow_importers(struct tgm_chan *chan, uint64_t pa, uint32_t *pending);
/* A GPU client made (add 1) or removed (add -1) a bind of the whole surface holding `c` with TEGRA_SCANOUT_TILED_KIND:
 * the surface is tiled while any such bind lives, whatever other binds of it say (tegra_gm20b_surface_tiled). */
void tgm_coherency_tiled_bind(struct tgm_coh *c, int add);

/* The stats (every periodic tegra-gm20b line, the submissions' GPU stamps, the push scan and profile): debug kernels
 * only; a release kernel has none of their code or strings. */
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define TGM_STATS 1
#else
#define TGM_STATS 0
#endif
/* Debug kernels: copies a present runs after its frame's copy and before its fence (TegraGM20BReadback.cpp), each into a
 * pitch-linear destination dst_pitch bytes across, width x 4 bytes of height lines, from a pitch source (src_block_log2
 * < 0: src_pitch bytes per row) or a block-linear one (8-line GOBs, blocks one GOB wide and 2^src_block_log2 high,
 * src_pitch bytes across). tgm_present_copy_read_dependencies is tgm_present_copy_dependencies with them. A release
 * kernel has the type only. */
struct tgm_present_read {
	uint64_t src_va, dst_va;
	uint32_t src_pitch, dst_pitch, width, height;
	int32_t src_block_log2;
	uint32_t reserved;
};

#define TGM_PRESENT_READS 2

#if TGM_STATS
/* Debug kernels: clock gating on (1) or off (0), for the A/B (TegraGM20B.cpp's debug.switch_gpu_clkgate). 0 or -EBUSY. */
int tgm_clkgate_switch(int on);

struct nvkm_device;
int tgm_gpu_held(void (*fn)(struct nvkm_device *device, void *arg), void *arg);

/* Debug kernels: the GM20B registers L4T's capture reads (TegraGM20BRegDump.h, regdump.c), as text into buf; *len its
 * length. 0, -EBUSY before M3, -ENOMEM, -ENOSPC when cap is short of tgm_regdump_bytes(). */
size_t tgm_regdump_bytes(void);
int tgm_regdump(char *buf, size_t cap, size_t *len);

struct TegraGM20BAbiRegdump;
struct TegraGM20BAbiRegdumpReply;
/* Debug kernels: TegraGM20BRegDumpABI.h's call, its answer in reply. 0, or -EINVAL for a bad tag or reserved word. */
int tgm_regdump_log(const struct TegraGM20BAbiRegdump *in, struct TegraGM20BAbiRegdumpReply *reply);

struct TegraGM20BAbiCopyReset;
struct TegraGM20BAbiCopyResetReply;
/* Debug kernels: TegraGM20BCopyResetABI.h's call (gpu_copy_reset.c). 0, -EINVAL, or -ENOMEM. */
int tgm_copy_reset_call(const struct TegraGM20BAbiCopyReset *in,
			struct TegraGM20BAbiCopyResetReply *reply);

int tgm_present_copy_read_dependencies(uint64_t src_va, uint64_t stride, int32_t src_block_log2,
				       uint32_t width, uint32_t height, uint64_t dst_va,
				       uint32_t dst_pitch, uint32_t block_height_log2,
				       void (*done)(void *, int), void *arg,
				       const struct tgm_present_read *reads, uint32_t nreads,
				       const struct switch_gpu_backing_token *dependencies,
				       uint32_t dependency_count);

#endif
/* The stats: one log line per 5 s of submissions (submits, fence latency, imports, pool, MC errors).
 * tgm_stats_import(+bytes, first page) for each host import made, (-bytes, first page) for each one released. */
void tgm_stats_import(int64_t bytes, uint64_t pa);

/* With the stats, what making and dropping GPU memory costs the CPU ("tegra-gm20b: mem", per 5 s window):
 * a BoAlloc of `bytes` (pool: write-combined pool memory; else cached pages, phase_ns = kernel allocation and wiring,
 * cache clean, page list, GPU memory object), a host import (phase_ns = wiring, page list, GPU memory object, cache
 * clean; skipped: always 0, the import always cleans), a VmBind map or unmap of `pages`, a BoFree, a cached BO's view
 * mapped with its pages entered at once (prefault). */
enum {
	TGM_PHASES = 4
};

/* kind: TGM_BO_CACHED (made on the caller's thread: allocated, zero-filled, cleaned), TGM_BO_POOL, TGM_BO_SPARE (a cached
 * BO taken from TegraGM20BUserClient.cpp's spares: zeroed and cleaned off every client's thread); owner: the process.
 * tgm_stats_compositor_pool: the compositor's pool BOs, +/-. tgm_spare_counts and tgm_owner_stats (each process's BoAllocs
 * this window, then cleared) are TegraGM20BUserClient.cpp's, for the "mem" and "mem3" lines. */
enum {
	TGM_BO_CACHED = 0,
	TGM_BO_POOL = 1,
	TGM_BO_SPARE = 2
};

void tgm_stats_bo_alloc(uint64_t bytes, int kind, uint64_t ns, const uint64_t phase_ns[TGM_PHASES],
			const char *owner);
void tgm_stats_compositor_pool(int64_t bytes);
void tgm_spare_counts(uint64_t *kept_bytes, uint64_t *ready, uint64_t *pool_zeroing_bytes);
/* Where the pool is (TegraGM20BUserClient.cpp, for the stats2 line): pool BOs of clients other than the compositor, pool
 * pages freed but still behind their view's release (orphans: a task still maps the view, or an import of it is live),
 * every client's compressible BOs with their count, and coherent BOs served from system memory instead
 * (tgm_pool::FromSystem) with their count. */
void tgm_pool_breakdown(uint64_t *client_bytes, uint64_t *orphan_bytes, uint64_t *orphans,
			uint64_t *compressible_bytes, uint64_t *compressibles,
			uint64_t *system_bytes, uint64_t *systems);
unsigned long tgm_owner_stats(char *out, unsigned long size);
void tgm_stats_bo_import(uint64_t bytes, int read_only, int skipped, uint64_t ns,
			 const uint64_t phase_ns[TGM_PHASES]);

/* With the stats, one host import's costs for the "imports" line and, past 4 ms, a "slow import" line of its own: the
 * wiring's wall time and the importing thread's CPU time in it (the rest it waited: free pages, busy pages, a
 * decompression, the CPU), the system's zero-fill faults meanwhile, tgm_gpu.lock's wait inside the GPU memory object, the
 * coherency registry's add and the clean. tgm_stats_cpu_ns and tgm_stats_zero_fills read the thread's CPU time and the
 * system's zero-fill count (0 without the stats). */
struct tgm_import_cost {
	uint64_t bytes, total_ns, wire_ns, wire_cpu_ns, zero_fills, pages_ns, mem_ns, lock_ns,
		coh_ns, clean_ns;
	uint32_t surface_id;
	int read_only;
	const char *owner;
};
#if TGM_STATS
/* Debug kernels: a writer promise's word the CPU raised (gpu_promise.c), and whether a word was one of the last few */
void tgm_promise_cpu_raised(uint32_t offset, uint32_t value);
int tgm_promise_was_cpu_raised(uint32_t offset, uint32_t value);
uint64_t tgm_stats_cpu_ns(void);
uint64_t tgm_stats_zero_fills(void);
void tgm_stats_import_cost(const struct tgm_import_cost *cost);
#else
static inline uint64_t tgm_stats_cpu_ns(void)
{
	return 0;
}

static inline uint64_t tgm_stats_zero_fills(void)
{
	return 0;
}

static inline void tgm_stats_import_cost(const struct tgm_import_cost *cost)
{
	(void)cost;
}
#endif
void tgm_stats_bind(int map, uint64_t pages, uint64_t ns);
/* A fence wait that slept on `waiter`, then woke (woken 1) or reached its deadline (0), and found its fence done or
 * not: the "waits" line's wakes, waits woken at their seqno, and wake-to-waiter latency. */
void tgm_stats_wait(uint32_t waiter, int woken, int done);

/* A SubmitAddress by phase (TegraGM20BUserClient.cpp): resolving its uses under the connection lock, owning them
 * (backing locks, dependency scan, room waits), chan_exec, and the commit after it. The "exec by process" line. */
enum {
	TGM_SUBMIT_RESOLVE,
	TGM_SUBMIT_LOCK,
	TGM_SUBMIT_EXEC,
	TGM_SUBMIT_COMMIT,
	TGM_SUBMIT_PHASES
};
#if TGM_STATS
void tgm_stats_submit(struct tgm_chan *chan, const uint64_t ns[TGM_SUBMIT_PHASES]);
#endif
void tgm_stats_bo_free(uint64_t ns);
void tgm_stats_prefault(uint64_t bytes);

/* drivers/SwitchPower (gpu_clock.c "idle clock"): switch_perf_hint's touch/missed-frame/launch hints (any context; back to
 * the run rate at once if idle), and the CPU governor's tick (scheduler context: loads, at most one work item). */
void tegra_gm20b_perf_hint(unsigned int hint);
/* A continuous-nanosecond service boundary, clamped to the next 20 ms. */
void tegra_gm20b_perf_deadline_ns(uint64_t until);
void tegra_gm20b_idle_tick(void);

#ifdef __cplusplus
}
#endif
#endif
