// SPDX-License-Identifier: MIT
// XNU side of the nvkm shim (see shim/include/nvxnu.h): logs and errors, GPU interrupts, the GPU memory pool, cache
// maintenance and embedded firmware; kits/SwitchLinux has the kernel services. No Linux names in here.
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/counter.h>
#include <kern/host_statistics.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <stdarg.h>
#include <string.h>
#include <sys/kdebug.h>
#include "include/nvxnu.h"
#include "../SwitchGpuBacking.h"
#include "../SwitchGpuOrdered.h"
#include "../../../kits/SwitchDMA/SwitchDcache.h"
#include "../../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../ArmGIC/SwitchInterrupts.h"
#include "../../TegraDisplayFlip/SwitchFrameTimeline.h"

extern "C" {
vm_offset_t phystokv(uint64_t pa);  // physical aperture (osfmk/arm64/arm_vm_init.c)
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
boolean_t ml_get_interrupts_enabled(void);
boolean_t switch_ram_gpu_pool(uint64_t *start, uint64_t *end);
void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
void IOLogv(const char *format, va_list ap);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
int vsnprintf(char *, size_t, const char *, va_list);
void tgm_present_pass(void);    // gpu_present.c
void tgm_present_filter(void);  // SwitchGpuPresentFilter.cpp
void tgm_promise_pass(void);    // gpu_promise_giveup.c
}

#if DEVELOPMENT || DEBUG
extern "C" void nvxnu_stats(const char *fmt, ...)
{
	char line[768];
	va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	os_log(OS_LOG_DEFAULT, "%s", line);
}
#endif

// nvkm's interrupt handler runs under spin_lock_irqsave, as in a Linux hard IRQ, and IOLog asserts with interrupts off.
// With interrupts off os_log is not safe, so XNU only formats the line into the message buffer with the kernel's
// printf, which replays it to the unified log later: that printf has no privacy annotations ("%{public}s" prints
// "{public}s" and drops the string), so the format is plain "%s".
static void nvxnu_report(const char *line)
{
	if (ml_get_interrupts_enabled()) {
		IOLog("%s", line);
	} else {
		os_log_error(OS_LOG_DEFAULT, "%s", line);
	}
}

extern "C" void nvxnu_log(const char *fmt, ...)
{
	va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
	va_start(ap, fmt);
	switch_linux_vlog(fmt, ap);
	va_end(ap);
}

// Bring-up step trace (M1/M2/CLK steps): only with kTracePaced, set in source for a bring-up test, which also gives the
// console time to draw each line before the next step. Without it nothing is printed: the stage results (M1..M4 ok) are TegraGM20B.cpp's own
// lines, and every GPU failure panics with one line.
#if DEVELOPMENT || DEBUG
extern "C" void nvxnu_step_impl(const char *fmt, ...)
{
	if (!nvxnu_trace_paced())
		return;
	va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
	va_start(ap, fmt);
	IOLogv(fmt, ap);
	va_end(ap);
	IOSleep(400);
}

constexpr bool kTracePaced = false;

extern "C" int nvxnu_trace_paced(void)
{
	return static_cast<int>(kTracePaced);
}
#endif

extern "C" int nvxnu_vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap)
{
	return vsnprintf(buf, size, fmt, ap);
}

extern "C" void nvxnu_panic(const char *fmt, ...)
{
	char line[1024];
	va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	panic("%s", line);
}

// nvkm's error reports. The ones nvkm's interrupt handler makes (only ever on the GPU interrupt work loop's thread, see
// gpuInterrupt) are joined into one line for that pass; nvkm reports a fault or exception, then starts its recovery, in
// the same pass, so the line names the cause before anything is torn down.
namespace
{
thread_t gIrqThread;
char gIrqErrors[480];
size_t gIrqErrorsLen;
volatile int gErrorsFatal;
int (*gErrorsContained)(const char *errors);
}

static SwitchGpuOrdered gGpuOrdered;

extern "C" unsigned nvxnu_gpu_ordered_begin(void)
{
	return gGpuOrdered.enter(reinterpret_cast<uintptr_t>(current_thread()));
}

extern "C" void nvxnu_gpu_ordered_end(unsigned scope)
{
	gGpuOrdered.leave(scope);
}

extern "C" int nvxnu_gpu_ordered(void)
{
	return static_cast<int>(gGpuOrdered.active(reinterpret_cast<uintptr_t>(current_thread())));
}

extern "C" void nvxnu_error(const char *dev, const char *fmt, ...)
{
	char line[256];
	va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	char report[sizeof(line) + 64];
	snprintf(report, sizeof(report), "tegra-gm20b %s: %s", dev, line);
	nvxnu_report(report);
	if (current_thread() != __atomic_load_n(&gIrqThread, __ATOMIC_RELAXED))
		return;
	size_t n = strnlen(line, sizeof(line));
	while (n && (line[n - 1] == '\n' || line[n - 1] == ' '))
		line[--n] = 0;
	if (gIrqErrorsLen) {
		gIrqErrorsLen += strlcpy(gIrqErrors + gIrqErrorsLen, " | ",
					 sizeof(gIrqErrors) - gIrqErrorsLen);
	}
	if (gIrqErrorsLen < sizeof(gIrqErrors)) {
		gIrqErrorsLen += strlcpy(gIrqErrors + gIrqErrorsLen, line,
					 sizeof(gIrqErrors) - gIrqErrorsLen);
	}
	if (gIrqErrorsLen > sizeof(gIrqErrors) - 1)
		gIrqErrorsLen = sizeof(gIrqErrors) - 1;
}

// From the end of the GPU self-test (M3) on, any such report panics (bring-up keeps its own FAIL lines before that),
// unless `contained` (gpu_hang.c client fault isolation) takes it as one client's.
extern "C" void nvxnu_errors_fatal(int (*contained)(const char *errors))
{
	gErrorsContained = contained;
	__atomic_store_n(&gErrorsFatal, 1, __ATOMIC_RELEASE);
}

extern "C" const char *nvxnu_gpu_errors(void)
{
	return gIrqErrors;
}

// No polled mode: both GPU lines are claimed at the end of M1 (panic if they cannot be), every wait sleeps until a
// wake, and a GPU interrupt that stays pending without reaching us panics with the PMC and GIC state.
extern "C" int
nvxnu_poll_irqs(void);  // shim/linux.c: the nvkm handler's irqreturn_t, -1 if none ran

namespace
{
constexpr uint32_t kGpuIntids[2] = {32 + 157,
				    32 + 158};  // "stall", "nonstall" (Linux tegra210.dtsi:427-428)
constexpr uint64_t kGpuBar0 = 0x57000000;
constexpr uint32_t PMC_INTR_0 = 0x100, PMC_INTR_1 = 0x104, PMC_INTR_EN_0 = 0x140,
		   PMC_INTR_EN_1 = 0x144;
constexpr uint64_t kCheckNs = 1000000000;  // a quiet wait looks at PMC this often
constexpr uint64_t kGraceNs = 50000000;    // for a wake already under way
constexpr uint32_t kStorm = 1000;          // consecutive interrupts nvkm did not handle
volatile uint32_t gWakeSeq, gIrqRuns;
#if DEVELOPMENT || DEBUG
uint64_t gIrqPasses[2];  // by line: stall, nonstall
#endif
volatile int gIrqActive, gGpuOff;
uint32_t gUnhandled;
volatile uint8_t *gPmc;
IOWorkLoop *gIrqLoop;
IOInterruptEventSource *gIrqSources[2];

uint32_t pmc(uint32_t offset)
{
	return switch_mmio_read32(gPmc, offset);
}

// An enabled PMC tree with something pending drives its GIC line. A powered-down GPU has none (and no PMC to read).
bool pmcPending()
{
	if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE))
		return false;
	return ((pmc(PMC_INTR_0) != 0u) && ((pmc(PMC_INTR_EN_0) & 1) != 0u)) ||
	       ((pmc(PMC_INTR_1) != 0u) && ((pmc(PMC_INTR_EN_1) & 1) != 0u));
}

[[noreturn]] void gpuPanic(const char *why, const char *what, uint32_t value)
{
	char stall[192];
	char nonstall[192];
	switch_interrupt_describe(kGpuIntids[0], stall, sizeof(stall));
	switch_interrupt_describe(kGpuIntids[1], nonstall, sizeof(nonstall));
	if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE)) {
		panic("tegra-gm20b: GPU interrupt %s (%s %u, INTID %u/%u) while the GPU is powered "
		      "down for system sleep: %s | %s",
		      why, what, value, kGpuIntids[0], kGpuIntids[1], stall, nonstall);
	}
	panic("tegra-gm20b: GPU interrupt %s (%s %u, INTID %u/%u): PMC INTR %#x/%#x EN %#x/%#x, %s "
	      "| %s",
	      why, what, value, kGpuIntids[0], kGpuIntids[1], pmc(PMC_INTR_0), pmc(PMC_INTR_1),
	      pmc(PMC_INTR_EN_0), pmc(PMC_INTR_EN_1), stall, nonstall);
}

// Work loop action for either GPU line (level; the GIC masks it while this runs). nvkm_intr unarms the GPU's
// top-level interrupt enables, runs the subdev handlers and rearms, so the line is quiet on return. A line nvkm
// keeps finding nothing on is stuck: panic rather than spin. Errors nvkm reported in the pass panic (nvxnu_error).
// NOLINTNEXTLINE(misc-const-correctness): IOInterruptEventSource::Action's signature
void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)
{
	// a line taken before nvxnu_irq_suspend() whose pass runs after it: the GPU is off now
	if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE))
		return;
	__atomic_fetch_add(&gIrqRuns, 1, __ATOMIC_RELAXED);
#if DEVELOPMENT || DEBUG
	__atomic_fetch_add(&gIrqPasses[source == gIrqSources[1]], 1, __ATOMIC_RELAXED);
#else
	(void)source;
#endif
	gIrqErrorsLen = 0;
	gIrqErrors[0] = 0;
	__atomic_store_n(&gIrqThread, current_thread(), __ATOMIC_RELAXED);
	const int handled = nvxnu_poll_irqs();
	__atomic_store_n(&gIrqThread, static_cast<thread_t>(nullptr), __ATOMIC_RELAXED);
	if (gIrqErrorsLen && __atomic_load_n(&gErrorsFatal, __ATOMIC_ACQUIRE) &&
	    !(gErrorsContained && gErrorsContained(gIrqErrors)))
		panic("tegra-gm20b: GPU error: %s", gIrqErrors);
	if (handled == 1) {
		gUnhandled = 0;
	} else if (++gUnhandled >= kStorm) {
		gpuPanic("storm", "unhandled", gUnhandled);
	}
	tgm_present_pass();
	tgm_promise_pass();
	nvxnu_wake();
	switch_gpu_backing_completed();
}

// The nonstall line's primary filter: the present watch's producer, read from memory alone (the GPU may be powered
// down); true, as the plain event source on a level line does, masks the line until the work loop's pass has run nvkm,
// which alone quiets it.
// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool gpuNonstallFilter(OSObject *, IOFilterInterruptEventSource *)
{
	tgm_present_filter();
	return true;
}

// Mach's assert-then-check: a wake between the caller's condition test and this sleep moves the word, and one
// after assert_wait_deadline() clears the wait, so neither is lost. Returns true if woken, false at `until`.
bool sleepUntil(const volatile uint32_t *word, nvxnu_u32 seq, uint64_t until_ns)
{
	const uint64_t now = nvxnu_nanotime();
	if (until_ns <= now)
		return false;
	uint64_t interval = 0;
	nanoseconds_to_absolutetime(until_ns - now, &interval);
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the wait event is the word's address
	assert_wait_deadline(reinterpret_cast<event_t>(const_cast<uint32_t *>(word)), THREAD_UNINT,
			     mach_absolute_time() + interval);
	if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != seq) {
		clear_wait(current_thread(), THREAD_AWAKENED);
		return true;
	}
	return thread_block(THREAD_CONTINUE_NULL) != THREAD_TIMED_OUT;
}
}

extern "C" nvxnu_u32 nvxnu_word_seq(const volatile nvxnu_u32 *word)
{
	return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

extern "C" void nvxnu_word_wake(volatile nvxnu_u32 *word)
{
	__atomic_fetch_add(word, 1, __ATOMIC_RELEASE);
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): the wait event is the word's address
	thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(word)));
}

// Sleeps until woken or the deadline. Every quiet second it looks at PMC: a GPU interrupt pending and enabled at two
// checks in a row with no interrupt taken in between never reached the CPU.
extern "C" int nvxnu_word_idle(const volatile nvxnu_u32 *word, nvxnu_u32 seq, nvxnu_u64 deadline_ns)
{
	if (!__atomic_load_n(&gIrqActive, __ATOMIC_ACQUIRE)) {
		panic("tegra-gm20b: GPU wait before its interrupts are attached (INTID %u/%u)",
		      kGpuIntids[0], kGpuIntids[1]);
	}
	bool pendingSeen = false;
	uint32_t runsAtPending = 0;
	for (;;) {
		const uint64_t now = nvxnu_nanotime();
		if (now >= deadline_ns)
			return 0;
		if (sleepUntil(word, seq,
			       deadline_ns - now < kCheckNs ? deadline_ns : now + kCheckNs))
			return 1;
		const uint32_t runs = __atomic_load_n(&gIrqRuns, __ATOMIC_RELAXED);
		const bool pending = pmcPending();
		if (pending && pendingSeen && runs == runsAtPending)
			gpuPanic("lost", "wait", seq);
		pendingSeen = pending;
		runsAtPending = runs;
	}
}

extern "C" void nvxnu_word_wake_due(const volatile nvxnu_u32 *word, nvxnu_u32 seq, const char *what,
				    nvxnu_u32 value)
{
	if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != seq ||
	    sleepUntil(word, seq, nvxnu_nanotime() + kGraceNs))
		return;
	gpuPanic("lost", what, value);
}

extern "C" void nvxnu_kdebug(nvxnu_u32 code, nvxnu_u64 a, nvxnu_u64 b, nvxnu_u64 c)
{
#if SWITCH_FRAME_TIMELINE
	KDBG(SWITCH_KDBG_CODE(code), a, b, c);
#else
	(void)code, (void)a, (void)b, (void)c;
#endif
}

extern "C" void nvxnu_irq_passes(nvxnu_u64 *stall, nvxnu_u64 *nonstall)
{
#if DEVELOPMENT || DEBUG
	*stall = __atomic_load_n(&gIrqPasses[0], __ATOMIC_RELAXED);
	*nonstall = __atomic_load_n(&gIrqPasses[1], __ATOMIC_RELAXED);
#else
	*stall = *nonstall = 0;
#endif
}

extern "C" nvxnu_u32 nvxnu_wake_seq(void)
{
	return nvxnu_word_seq(&gWakeSeq);
}

extern "C" void nvxnu_wake(void)
{
	nvxnu_word_wake(&gWakeSeq);
}

extern "C" int nvxnu_idle(nvxnu_u32 seq, nvxnu_u64 deadline_ns)
{
	return nvxnu_word_idle(&gWakeSeq, seq, deadline_ns);
}

extern "C" void nvxnu_wake_due(nvxnu_u32 seq, const char *what, nvxnu_u32 value)
{
	nvxnu_word_wake_due(&gWakeSeq, seq, what, value);
}

// Claims both GPU lines on a work loop (switch_interrupt_nub sleeps until the GIC is up). Called at the end of M1:
// request_irq() has recorded nvkm's handler (nvkm_device_ctor) and nvkm has not armed anything yet, so the PMC
// enables are still at reset and neither line can be asserted.
extern "C" void nvxnu_irq_start(void)
{
	if (__atomic_load_n(&gIrqActive, __ATOMIC_ACQUIRE))
		return;
	gPmc = switch_mmio_map(kGpuBar0, 0x1000);
	gIrqLoop = IOWorkLoop::workLoop();
	for (int i = 0; i < 2; i++) {
		IOService *nub = switch_interrupt_nub(kGpuIntids[i], false);
		if (!gIrqLoop)
			gIrqSources[i] = nullptr;
		else if (i)
			gIrqSources[i] = IOFilterInterruptEventSource::filterInterruptEventSource(
				gIrqLoop, gpuInterrupt, gpuNonstallFilter, nub, 0);
		else
			gIrqSources[i] = IOInterruptEventSource::interruptEventSource(
				gIrqLoop, gpuInterrupt, nub, 0);
		if (!gPmc || !gIrqSources[i] ||
		    gIrqLoop->addEventSource(gIrqSources[i]) != kIOReturnSuccess) {
			panic("tegra-gm20b: cannot attach the GPU %s interrupt (INTID %u)",
			      i ? "nonstall" : "stall", kGpuIntids[i]);
		}
	}
	__atomic_store_n(&gIrqActive, 1, __ATOMIC_RELEASE);
	for (auto *source : gIrqSources)
		source->enable();
}

// System sleep: the GPU's power goes after this returns. Both lines are masked first; the flag is set on the work loop, so
// a pass already running finishes before it and a pass for an interrupt taken earlier, which IOInterruptEventSource still
// delivers after disable(), returns without reading the GPU.
extern "C" void nvxnu_irq_suspend(void)
{
	if (!__atomic_load_n(&gIrqActive, __ATOMIC_ACQUIRE))
		return;
	for (auto *source : gIrqSources)
		source->disable();
	gIrqLoop->runAction(
		[](OSObject *, void *, void *, void *, void *) -> IOReturn {
			__atomic_store_n(&gGpuOff, 1, __ATOMIC_RELEASE);
			return kIOReturnSuccess;
		},
		nullptr);
}

// After the power-up, before nvkm arms anything (as nvxnu_irq_start() at the end of M1): the PMC enables are at reset.
extern "C" void nvxnu_irq_resume(void)
{
	if (!__atomic_load_n(&gIrqActive, __ATOMIC_ACQUIRE))
		return;
	gUnhandled = 0;
	__atomic_store_n(&gGpuOff, 0, __ATOMIC_RELEASE);
	for (auto *source : gIrqSources)
		source->enable();
}

extern "C" void nvxnu_dcache_clean(const void *va, nvxnu_u64 size)
{
	switch_dcache_clean(va, size);
}

extern "C" void nvxnu_dcache_clean_inval(const void *va, nvxnu_u64 size)
{
	switch_dcache_clean_invalidate(va, size);
}

// Pages outside the kernel's RAM (device memory a task mapped) have no cacheable alias to maintain.
extern "C" void nvxnu_dcache_clean_inval_pa(nvxnu_u64 pa, nvxnu_u64 size)
{
	if (switch_dma_pa_managed(pa)) {
		switch_dcache_phys_lines(SWITCH_DCACHE_CLEAN_INVALIDATE, pa, size);
		switch_dcache_sync();
	}
}

// One barrier for the whole list: a pair per page is ~1800 barriers for a 3.6 MB import.
extern "C" void nvxnu_dcache_clean_inval_pages(const nvxnu_u64 *pas, nvxnu_u32 count)
{
	switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, pas, count);
	switch_dcache_sync();
}

extern "C" void switch_gpu_backing_cache(const uint64_t *pages, uint32_t count, uint64_t offset,
					 uint64_t bytes, int operation)
{
	const switch_dcache_op op =
		operation == SWITCH_GPU_CACHE_CLEAN_INVALIDATE ? SWITCH_DCACHE_CLEAN_INVALIDATE
		: operation == SWITCH_GPU_CACHE_INVALIDATE     ? SWITCH_DCACHE_INVALIDATE
							       : SWITCH_DCACHE_CLEAN;
	while (bytes && (offset >> 12) < count) {
		const uint64_t within = offset & 4095;
		const uint64_t span = bytes < 4096 - within ? bytes : 4096 - within;
		const uint64_t pa = pages[offset >> 12] + within;
		if (switch_dma_pa_managed(pa))
			switch_dcache_phys_lines(op, pa, span);
		offset += span;
		bytes -= span;
	}
	switch_dcache_sync();
}

extern "C" void switch_gpu_backing_retire(void);
extern "C" void switch_gpu_backing_expire(void);
static void *switch_backing_retire_work, *switch_backing_expiry_work;

extern "C" int switch_gpu_backing_prepare_retirement(void)
{
	if (!switch_backing_retire_work)
		switch_backing_retire_work = nvxnu_work_alloc(
			[](void *) {
				switch_gpu_backing_retire();
			},
			nullptr);
	if (!switch_backing_expiry_work)
		switch_backing_expiry_work = nvxnu_work_alloc(
			[](void *) {
				switch_gpu_backing_expire();
			},
			nullptr);
	return switch_backing_retire_work && switch_backing_expiry_work ? 0 : -12;
}

extern "C" void switch_gpu_backing_schedule_retirement(void)
{
	nvxnu_work_schedule(switch_backing_retire_work);
}

extern "C" uint64_t switch_gpu_backing_time_ns(void)
{
	return nvxnu_nanotime();
}

extern "C" void switch_gpu_backing_arm_expiry(uint64_t deadline_ns)
{
	if (!deadline_ns) {
		nvxnu_work_cancel(switch_backing_expiry_work);
		return;
	}
	const uint64_t now = nvxnu_nanotime();
	nvxnu_work_schedule_after(switch_backing_expiry_work,
				  deadline_ns > now ? deadline_ns - now : 1);
}

// The system's page faults so far (every vm_fault) and those that zero-filled a page: XNU's own counters, as vm_stat reads
// them (the stats' "mem" line takes the window's deltas). user_* stay 0: they need user-fault timing, which this
// kernel's XNU does not carry.
extern "C" void nvxnu_vm_faults(struct nvxnu_faults *f)
{
	f->faults = counter_load(&vm_statistics_faults);
	f->zero_fill = counter_load(&vm_statistics_zero_fill_count);
	f->user_n = f->user_ns = f->user_max_ns = 0;
}

// The GPU memory pool, preferably the physical range the kernel keeps out of the VM at boot (arm_vm_init.c
// switch_ram_init, switch_ram_gpu_pool), which its physical aperture
// maps write-combined and nothing maps cacheable: phystokv() is the pool's only mapping (a 128 MiB ml_io_map_wcomb runs
// out of kernel VA).
// Fallback: a contiguous IOBufferMemoryDescriptor below 4 GiB whose pages are switched to write-combined
// for every mapping (the NativeAdreno writeCombinePool() recipe, after cleaning the cacheable alias).
static struct {
	IOBufferMemoryDescriptor *descriptor;
	uint64_t pa, size;
	void *va;
} pool;

extern "C" int nvxnu_pool_init(int *zeroed)
{
	const uint64_t bytes = 256ULL << 20;
	*zeroed = 0;
	if (pool.va)
		return 0;
	uint64_t start = 0;
	uint64_t end = 0;
	if (switch_ram_gpu_pool(&start, &end)) {
		// Bring-up trace: one console line before each step that touches memory or hardware for the first time, so
		// the last line names the step that hung.
		pool.va = reinterpret_cast<void *>(phystokv(start));
		nvxnu_step(
			"tegra-gm20b: M1 step: reserved pool %#llx + %llu MiB at physmap va %p\n",
			start, (end - start) >> 20, pool.va);
		if (!pool.va)
			return -12;
		pool.pa = start;
		pool.size = end - start;
		// No CPU clear (0.3 s of boot for 256 MiB through the write-combined aperture): every page starts
		// dirty, an allocation before the GPU runs zeroes the pages it takes, and the copy engine zeroes the rest
		// once the GPU is up (gpu_scrub.c "pool scrub").
		return 0;
	}
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task,
		static_cast<IOOptionBits>(kIODirectionInOut) |
			static_cast<IOOptionBits>(kIOMemoryMapperNone) |
			kIOMemoryPhysicallyContiguous,
		bytes, 0xfffff000ULL);
	if (!md)
		return -12;
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return -12;
	}
	IOByteCount len = 0;
	const addr64_t first = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
	void *va = md->getBytesNoCopy();
	if (!first || (first & PAGE_MASK) || !va || len < bytes || first + bytes > (1ULL << 32)) {
		md->complete();
		md->release();
		return -12;
	}
	memset(va, 0, bytes);
	switch_dcache_clean_invalidate(va, bytes);
	for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
		pmap_set_cache_attributes(static_cast<ppnum_t>(atop_64(first + off)),
					  VM_WIMG_WCOMB);
	}
	pool.descriptor = md;
	pool.pa = first;
	pool.size = bytes;
	pool.va = va;
	*zeroed = 1;
	return 0;
}

extern "C" nvxnu_u32 nvxnu_vm_page_size(void)
{
	return PAGE_SIZE;
}

extern "C" nvxnu_u64 nvxnu_ram_bytes(void)
{
	return max_mem;
}

// The buffer stays wired for the kext's life: nothing gives such memory back.
extern "C" int nvxnu_contig_alloc(nvxnu_u64 bytes, nvxnu_u64 *pa)
{
	switch_dma_buffer buffer = {};
	if (!switch_dma_alloc(&buffer, bytes, 0xffffffffffULL & ~static_cast<uint64_t>(PAGE_MASK)))
		return -12;
	*pa = buffer.pa;
	return 0;
}

extern "C" nvxnu_u64 nvxnu_pool_base_pa(void)
{
	return pool.pa;
}

extern "C" nvxnu_u64 nvxnu_pool_size(void)
{
	return pool.size;
}

extern "C" void *nvxnu_pool_base_va(void)
{
	return pool.va;
}

// Scattered pool pages: one kernel mapping over several physical ranges, remembered for vunmap.
namespace
{
struct nvxnu_vmapping {
	nvxnu_vmapping *next;
	IOMemoryMap *map;
	void *va;
};
}  // namespace

static nvxnu_vmapping *vmaps;
static IOLock *vmaps_lock;

extern "C" void *nvxnu_vmap(const nvxnu_u64 *pas, nvxnu_u32 count)
{
	if (!vmaps_lock) {
		IOLock *fresh = IOLockAlloc();
		// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap writes the current value back into it
		IOLock *expected = nullptr;
		if (!__atomic_compare_exchange_n(&vmaps_lock, &expected, fresh, false,
						 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			IOLockFree(fresh);
	}
	IOAddressRange *ranges = IONew(IOAddressRange, count);
	if (!ranges)
		return nullptr;
	nvxnu_u32 runs = 0;
	bool whole = true;
	for (nvxnu_u32 i = 0; i < count; i++) {
		if (runs && ranges[runs - 1].address + ranges[runs - 1].length == pas[i]) {
			ranges[runs - 1].length += 4096;
			continue;
		}
		// a kernel mapping is whole VM pages, so only the first run may start inside one and only the last end there
		if (runs &&
		    (((ranges[runs - 1].address + ranges[runs - 1].length) | pas[i]) & PAGE_MASK))
			whole = false;
		ranges[runs++] = {pas[i], 4096};
	}
	if (!whole) {
		IODelete(ranges, IOAddressRange, count);
		return nullptr;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRanges(
		ranges, runs,
		static_cast<IOOptionBits>(kIODirectionInOut) |
			static_cast<IOOptionBits>(kIOMemoryTypePhysical64),
		TASK_NULL);
	IODelete(ranges, IOAddressRange, count);
	if (!md)
		return nullptr;
	IOMemoryMap *map =
		md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
	md->release();
	nvxnu_vmapping *m = map ? IONew(nvxnu_vmapping, 1) : nullptr;
	if (!m) {
		if (map)
			map->release();
		return nullptr;
	}
	m->map = map;
	m->va = reinterpret_cast<void *>(map->getVirtualAddress());
	IOLockLock(vmaps_lock);
	m->next = vmaps;
	vmaps = m;
	IOLockUnlock(vmaps_lock);
	return m->va;
}

// NOLINTNEXTLINE(misc-const-correctness): nvxnu.h's declaration, which the vendored C code calls
extern "C" void nvxnu_vunmap(void *va)
{
	if (!vmaps_lock)
		return;
	IOLockLock(vmaps_lock);
	for (nvxnu_vmapping **p = &vmaps; *p; p = &(*p)->next) {
		if ((*p)->va == va) {
			nvxnu_vmapping *m = *p;
			*p = m->next;
			IOLockUnlock(vmaps_lock);
			m->map->release();
			IODelete(m, nvxnu_vmapping, 1);
			return;
		}
	}
	IOLockUnlock(vmaps_lock);
}

// The firmware table is firmware.S's.
namespace
{
struct nvxnu_fw_entry {
	const char *name;
	const unsigned char *start;
	const unsigned char *end;
};
}  // namespace

extern "C" const nvxnu_fw_entry tegra_gm20b_firmware[];

extern "C" int nvxnu_firmware_find(const char *name, const void **data, unsigned long *size)
{
	for (const nvxnu_fw_entry *e = tegra_gm20b_firmware; e->name; e++) {
		if (!strcmp(e->name, name)) {
			*data = e->start;
			*size = static_cast<unsigned long>(e->end - e->start);
			return e->end > e->start ? 0 : -2;
		}
	}
	return -2;
}
