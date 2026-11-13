/* SPDX-License-Identifier: MIT */
/*
 * Boundary between the Linux-flavoured C world (vendored nouveau nvkm + shim/linux.c) and XNU.
 * Everything here is plain C with fixed-width types only, so it can be included from both sides
 * without dragging Linux names into XNU translation units or XNU names into nvkm ones.
 * The kernel services are kits/SwitchLinux's; the GPU's own are in shim/xnu_glue.cpp.
 */
#ifndef NVXNU_H
#define NVXNU_H
#include "../../../../kits/SwitchLinux/SwitchLinuxLock.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long nvxnu_u64;
typedef unsigned int nvxnu_u32;

/* logging (always goes to the kernel log; callers filter) */
void nvxnu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Periodic statistics: the unified log only (os_log), never the boot console. */
void nvxnu_stats(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Bring-up trace: nvxnu_log, then 400 ms so the console draws the line before the next (possibly hanging) step. */
void nvxnu_step_impl(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#if DEVELOPMENT || DEBUG
#define NVXNU_DIAGNOSTICS 1
#else
#define NVXNU_DIAGNOSTICS 0
#endif
#define nvxnu_step(...)                               \
	do {                                          \
		if (NVXNU_DIAGNOSTICS)                \
			nvxnu_step_impl(__VA_ARGS__); \
	} while (0)
/* Traced bring-up (a source-level switch, off): trace lines wait for the console (nvxnu_step 400 ms, 2 s before M1, 3 s
 * per stage) */
int nvxnu_trace_paced(void);
int nvxnu_vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
/* nvkm's error reports (dev_err and worse; nvkm_error/nvkm_fatal): logged like nvxnu_log. Those made by nvkm's interrupt
 * handler (the GPU interrupt work loop) are also kept for that pass, and once nvxnu_errors_fatal() was called the pass
 * ends in one panic quoting them: every GPU failure nvkm reports (MMU fault, PBDMA, GR trap/exception, FECS, LTC, PRI,
 * BIND/SCHED/CHSW errors) stops the machine with its own words on screen. A non-NULL `contained` sees the pass's reports
 * first (on the work loop, nvkm's handlers done) and returns 1 to go on instead: they stay with one GPU client. */
void nvxnu_error(const char *dev, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void nvxnu_errors_fatal(int (*contained)(const char *errors));
/* what the current interrupt pass has reported so far ("" if nothing) */
const char *nvxnu_gpu_errors(void);
void nvxnu_panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* sleeping locks (thread context): kits/SwitchLinux's */
static inline void *nvxnu_mutex_alloc(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
	return switch_linux_mutex_alloc();
}

static inline void nvxnu_mutex_lock(void *m)
{
	switch_linux_mutex_lock(m);
}

static inline void nvxnu_mutex_unlock(void *m)
{
	switch_linux_mutex_unlock(m);
}

static inline int nvxnu_mutex_trylock(void *m)
{
	return switch_linux_mutex_trylock(m) ? 1 : 0;
}

/* With `m` held: releases it, sleeps until nvxnu_wakeup(event) or the deadline (~0: none), takes it again. 1 if woken. */
static inline int nvxnu_mutex_sleep(void *m, const void *event, nvxnu_u64 deadline_ns)
{
	return switch_linux_mutex_sleep(m, event, deadline_ns) ? 1 : 0;
}

static inline void nvxnu_wakeup(const void *event)
{
	switch_linux_wakeup(event);
}

static inline int nvxnu_irqs_enabled(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
	return switch_linux_irqs_disabled() ? 0 : 1;
}

static inline nvxnu_u64 nvxnu_nanotime(void)  // NOLINT(modernize-redundant-void-arg): a C header
{
	return switch_linux_nanotime();
}

/* mach_absolute_time units to nvxnu_nanotime's */
static inline nvxnu_u64 nvxnu_abstime_to_ns(nvxnu_u64 abstime)
{
	return switch_linux_abstime_to_ns(abstime);
}

/* XNU's delay: spins where blocking is not allowed or not worth it, blocks otherwise */
static inline void nvxnu_udelay(nvxnu_u32 us)
{
	switch_linux_udelay(us);
}

static inline void nvxnu_usleep(nvxnu_u32 us)
{
	switch_linux_usleep_range(us, us);
}

/* deferred work: run fn(arg) on a kernel thread call, never two runs of one item at once (a Linux work item's rule) */
static inline void *nvxnu_work_alloc(void (*fn)(void *), void *arg)
{
	return switch_linux_call_alloc(fn, arg);
}

static inline void nvxnu_work_schedule(void *w)
{
	if (w)
		(void)switch_linux_call_enter(w);
}

/* 1 if a run was pending (and is now cancelled); returns once no run is under way */
static inline int nvxnu_work_cancel_wait(void *w)
{
	return w && switch_linux_call_cancel_wait(w) ? 1 : 0;
}

/* once, ns from now (a deadline, not a loop); cancel: no wait, any context */
static inline void nvxnu_work_schedule_after(void *w, nvxnu_u64 ns)
{
	if (w)
		(void)switch_linux_call_enter_after(w, ns);
}

static inline void nvxnu_work_cancel(void *w)
{
	if (w)
		(void)switch_linux_call_cancel(w);
}

static inline volatile void *nvxnu_ioremap(nvxnu_u64 pa, nvxnu_u64 size, int write_combined)
{
	return switch_linux_ioremap(pa, size, write_combined);
}

/*
 * GPU memory pool: physically contiguous, below 4 GiB, mapped write-combined in the kernel.
 * Either the range XNU withholds from the VM (switch_ram_gpu_pool) or, as a
 * fallback, an IOBufferMemoryDescriptor. Page granular (4 KiB).
 */
/* *zeroed: 1 if the pool's pages were zeroed here (the fallback); the reserved range is not, its pages start dirty and
 * the GPU's copy engine zeroes them once it runs (gpu_scrub.c "pool scrub") */
int nvxnu_pool_init(int *zeroed);
/* XNU's VM page: what a CPU mapping covers, 16 KiB over the A57's 4 KiB granule. Pool and GPU pages stay 4 KiB. */
nvxnu_u32 nvxnu_vm_page_size(void);
/* the memory XNU manages (max_mem): what the GPU's compression covers, as nvgpu covers Linux's RAM */
nvxnu_u64 nvxnu_ram_bytes(void);
/* Wired, physically contiguous, zeroed memory for the GPU alone (no CPU view is kept), never freed; 0 on success */
int nvxnu_contig_alloc(nvxnu_u64 bytes, nvxnu_u64 *pa);
nvxnu_u64 nvxnu_pool_base_pa(void);
nvxnu_u64 nvxnu_pool_size(void);
void *nvxnu_pool_base_va(void);
/* map pool pages (by pa) into one virtually contiguous kernel range, write-combined; NULL unless every break between
 * runs of consecutive pages falls on a VM page */
void *nvxnu_vmap(const nvxnu_u64 *pas, nvxnu_u32 count);
void nvxnu_vunmap(void *va);
void nvxnu_dcache_clean(const void *va, nvxnu_u64 size);
void nvxnu_dcache_clean_inval(const void *va, nvxnu_u64 size);
/* clean+invalidate [pa, pa + size) (one page at most) through the physical aperture; no-op outside the kernel's RAM */
void nvxnu_dcache_clean_inval_pa(nvxnu_u64 pa, nvxnu_u64 size);
/* the same over `count` 4 KiB pages, with one barrier pair for the whole list instead of one per page */
void nvxnu_dcache_clean_inval_pages(const nvxnu_u64 *pas, nvxnu_u32 count);

/* A user raw map whose page-table writes the GPU orders (SwitchGpuVmMap.h, under its VM lock): between begin and end, on the
 * calling thread only, nvkm skips the L2 invalidate after page-table writes and the MMU TLB invalidate. */
unsigned nvxnu_gpu_ordered_begin(void);
void nvxnu_gpu_ordered_end(unsigned scope);
int nvxnu_gpu_ordered(void);

/* the system's page faults so far (every vm_fault) and the zero-filled ones among them (XNU's vm_stat counters); with the
 * kernel's user-fault timing, the user faults handled so far, their total time and the longest since the last call (ns) */
struct nvxnu_faults {
	nvxnu_u64 faults, zero_fill, user_n, user_ns, user_max_ns;
};

void nvxnu_vm_faults(struct nvxnu_faults *f);

/* firmware embedded in the kernel image (shim/TegraGM20BFirmware.S); 0 on success */
int nvxnu_firmware_find(const char *name, const void **data, unsigned long *size);

/*
 * GPU interrupts, no polled mode. nvxnu_irq_start() claims the GPU's stall and nonstall GIC SPIs (157/158 = INTID
 * 189/190, Linux tegra210.dtsi:427-428) on a work loop whose action runs nvxnu_poll_irqs() (nvkm_intr reads every
 * MC leaf, so both lines share it) and then nvxnu_wake(); it panics if it cannot. Waiters take nvxnu_wake_seq(),
 * test their condition and call nvxnu_idle(), which sleeps until the sequence moves (1) or the deadline passes (0),
 * and panics when a GPU interrupt stays pending at PMC without being taken, or when called before nvxnu_irq_start().
 * nvxnu_wake() is safe with spinlocks held. nvxnu_wake_due(): `what` (a fence, say) is done although nothing woke
 * its waiter since `seq`; after 50 ms for a wake under way it panics with the PMC and GIC state.
 */
void nvxnu_irq_start(void);
/* System sleep: nvxnu_irq_suspend() before the GPU's power goes (both lines masked, no pass reads the GPU from then on,
 * nvxnu_idle() no longer looks at PMC); nvxnu_irq_resume() once it is back, before nvkm arms its interrupts. */
void nvxnu_irq_suspend(void);
void nvxnu_irq_resume(void);
/* A frame timeline kdebug tracepoint (SwitchFrameTimeline.h): debug kernels only, free while tracing is off. */
void nvxnu_kdebug(nvxnu_u32 code, nvxnu_u64 a, nvxnu_u64 b, nvxnu_u64 c);
/* The interrupt passes run so far, by line. */
void nvxnu_irq_passes(nvxnu_u64 *stall, nvxnu_u64 *nonstall);
nvxnu_u32 nvxnu_wake_seq(void);
void nvxnu_wake(void);
int nvxnu_idle(nvxnu_u32 seq, nvxnu_u64 deadline_ns);
void nvxnu_wake_due(nvxnu_u32 seq, const char *what, nvxnu_u32 value);
/* The same on a wake word of the caller's (a word that must outlive its sleepers): nvxnu_word_wake() moves it and wakes
 * only the threads sleeping on it, not every GPU waiter. */
nvxnu_u32 nvxnu_word_seq(const volatile nvxnu_u32 *word);
void nvxnu_word_wake(volatile nvxnu_u32 *word);
int nvxnu_word_idle(const volatile nvxnu_u32 *word, nvxnu_u32 seq, nvxnu_u64 deadline_ns);
void nvxnu_word_wake_due(const volatile nvxnu_u32 *word, nvxnu_u32 seq, const char *what,
			 nvxnu_u32 value);

/* Tegra210 platform pieces (platform.c), used by the clk/regulator/reset/pmc shims */
int tegra_gm20b_platform_map(void);
int tegra_gm20b_rail_enable(int on);
int tegra_gm20b_rail_get_uv(int *uv);
int tegra_gm20b_rail_set_uv(int uv);
int tegra_gm20b_rail_max_uv(void);
nvxnu_u32 tegra_gm20b_dvfs_khz(int row); /* L4T B01 GPU rate of table row, 0 past the end */
int tegra_gm20b_dvfs_na_uv(
	nvxnu_u32 khz, int speedo); /* its NAPLL voltage, worst over temperature; -22 off-table */
int tegra_gm20b_clk_enable(int id, int on);
unsigned long tegra_gm20b_clk_get_rate(int id);
int tegra_gm20b_clk_set_rate(int id, unsigned long rate);
void tegra_gm20b_reset(int assert);
int tegra_gm20b_unclamp(void);
int tegra_gm20b_mc_flush(int flush);
nvxnu_u32 tegra_gm20b_fuse_read(nvxnu_u32 offset); /* Linux tegra_fuse_readl() offsets */
/* MC_INTSTATUS (cleared after the read), MC_ERR_STATUS, MC_ERR_ADR (Linux drivers/memory/tegra/mc.h): what the memory
 * controller refused since the previous call */
void tegra_gm20b_mc_errors(nvxnu_u32 *intstatus, nvxnu_u32 *status, nvxnu_u32 *addr);
/* The MC's carveout 2, the GPU's WPR1: CFG0, BOM, SIZE_128KB, CLIENT_ACCESS2, CLIENT_ACCESS4 */
void tegra_gm20b_mc_gpu_wpr(nvxnu_u32 v[5]);

enum {
	TEGRA_GM20B_CLK_GPU = 0,
	TEGRA_GM20B_CLK_REF = 1,
	TEGRA_GM20B_CLK_PWR = 2
};

#ifdef __cplusplus
}
#endif
#endif
