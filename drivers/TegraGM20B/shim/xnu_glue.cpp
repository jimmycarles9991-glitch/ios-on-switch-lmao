// SPDX-License-Identifier: MIT
// XNU side of the nvkm shim (see shim/include/nvxnu.h): heap, locks, time, thread calls, GPU interrupts, MMIO, the
// GPU memory pool, cache maintenance and embedded firmware. No Linux names in here.
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread_call.h>
#include <kern/counter.h>
#include <kern/host_statistics.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <stdarg.h>
#include <string.h>
#include <sys/kdebug.h>
#include "include/nvxnu.h"
#include "../../ArmGIC/SwitchInterrupts.h"
#include "../../TegraDisplayFlip/SwitchFrameTimeline.h"

extern "C" {
vm_offset_t phystokv(uint64_t pa);  // physical aperture (osfmk/arm64/arm_vm_init.c)
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
boolean_t ml_set_interrupts_enabled(boolean_t enable);
boolean_t ml_get_interrupts_enabled(void);
boolean_t switch_ram_gpu_pool(uint64_t *start, uint64_t *end);
void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
boolean_t pmap_valid_page(ppnum_t pn);
void IOLogv(const char *format, va_list ap);
int vsnprintf(char *, size_t, const char *, va_list);
}

static lck_grp_t *nvxnu_grp()
{
	static lck_grp_t *grp;
	if (!grp) {
		lck_grp_t *fresh = lck_grp_alloc_init("TegraGM20B", LCK_GRP_ATTR_NULL);
		lck_grp_t *expected = nullptr;
		if (!__atomic_compare_exchange_n(&grp, &expected, fresh, false, __ATOMIC_ACQ_REL,
						 __ATOMIC_ACQUIRE))
			lck_grp_free(fresh);
	}
	return grp;
}

extern "C" void nvxnu_stats(const char *fmt, ...)
{
	char line[768];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	os_log(OS_LOG_DEFAULT, "%s", line);
}

// nvkm's interrupt handler runs under spin_lock_irqsave, as in a Linux hard IRQ, and IOLog asserts with interrupts off.
// os_log queues a line from any context, without the console copy.
static void nvxnu_report(const char *line)
{
	if (ml_get_interrupts_enabled())
		IOLog("%s", line);
	else
		os_log_error(OS_LOG_DEFAULT, "%{public}s", line);
}

extern "C" void nvxnu_log(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	nvxnu_report(line);
}

// Bring-up step trace (M1/M2/CLK steps): only with kTracePaced, set in source for a bring-up test, which also gives the
// console time to draw each line before the next step. Without it nothing is printed: the stage results (M1..M4 ok) are TegraGM20B.cpp's own
// lines, and every GPU failure panics with one line.
extern "C" void nvxnu_step(const char *fmt, ...)
{
	if (!nvxnu_trace_paced())
		return;
	va_list ap;
	va_start(ap, fmt);
	IOLogv(fmt, ap);
	va_end(ap);
	IOSleep(400);
}

constexpr bool kTracePaced = false;

extern "C" int nvxnu_trace_paced(void)
{
	return kTracePaced;
}

extern "C" int nvxnu_vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap)
{
	return vsnprintf(buf, size, fmt, ap);
}

extern "C" void nvxnu_panic(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
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

static thread_t gGpuOrderedThread;

extern "C" void nvxnu_gpu_ordered_begin(void)
{
	__atomic_store_n(&gGpuOrderedThread, current_thread(), __ATOMIC_RELAXED);
}

extern "C" void nvxnu_gpu_ordered_end(void)
{
	__atomic_store_n(&gGpuOrderedThread, (thread_t) nullptr, __ATOMIC_RELAXED);
}

extern "C" int nvxnu_gpu_ordered(void)
{
	return current_thread() == __atomic_load_n(&gGpuOrderedThread, __ATOMIC_RELAXED);
}

extern "C" void nvxnu_error(const char *dev, const char *fmt, ...)
{
	char line[256];
	va_list ap;
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
	if (gIrqErrorsLen)
		gIrqErrorsLen += strlcpy(gIrqErrors + gIrqErrorsLen, " | ",
					 sizeof(gIrqErrors) - gIrqErrorsLen);
	if (gIrqErrorsLen < sizeof(gIrqErrors))
		gIrqErrorsLen += strlcpy(gIrqErrors + gIrqErrorsLen, line,
					 sizeof(gIrqErrors) - gIrqErrorsLen);
	if (gIrqErrorsLen > sizeof(gIrqErrors) - 1)
		gIrqErrorsLen = sizeof(gIrqErrors) - 1;
}

// From the end of the GPU self-test (M3) on, any such report panics (bring-up keeps its own FAIL lines before that),
// unless `contained` (gpu.c client fault isolation) takes it as one client's.
extern "C" void nvxnu_errors_fatal(int (*contained)(const char *errors))
{
	gErrorsContained = contained;
	__atomic_store_n(&gErrorsFatal, 1, __ATOMIC_RELEASE);
}

extern "C" const char *nvxnu_gpu_errors(void)
{
	return gIrqErrors;
}

// A 16-byte size header in front of every block: nvxnu_free needs no size.
extern "C" void *nvxnu_malloc(unsigned long size, int zero)
{
	if (size > (1UL << 30))
		return nullptr;
	unsigned long *p =
		static_cast<unsigned long *>(zero ? IOMallocZero(size + 16) : IOMalloc(size + 16));
	if (!p)
		return nullptr;
	p[0] = size;
	return p + 2;
}

extern "C" void nvxnu_free(void *ptr)
{
	if (!ptr)
		return;
	unsigned long *p = static_cast<unsigned long *>(ptr) - 2;
	IOFree(p, p[0] + 16);
}

extern "C" unsigned long nvxnu_malloc_size(void *ptr)
{
	return ptr ? (static_cast<unsigned long *>(ptr) - 2)[0] : 0;
}

extern "C" void *nvxnu_mutex_alloc(void)
{
	return lck_mtx_alloc_init(nvxnu_grp(), LCK_ATTR_NULL);
}

extern "C" void nvxnu_mutex_free(void *m)
{
	if (m)
		lck_mtx_free(static_cast<lck_mtx_t *>(m), nvxnu_grp());
}

extern "C" void nvxnu_mutex_lock(void *m)
{
	lck_mtx_lock(static_cast<lck_mtx_t *>(m));
}

extern "C" void nvxnu_mutex_unlock(void *m)
{
	lck_mtx_unlock(static_cast<lck_mtx_t *>(m));
}

extern "C" int nvxnu_mutex_trylock(void *m)
{
	return lck_mtx_try_lock(static_cast<lck_mtx_t *>(m)) ? 1 : 0;
}

extern "C" void *nvxnu_spin_alloc(void)
{
	return lck_spin_alloc_init(nvxnu_grp(), LCK_ATTR_NULL);
}

extern "C" void nvxnu_spin_free(void *s)
{
	if (s)
		lck_spin_free(static_cast<lck_spin_t *>(s), nvxnu_grp());
}

extern "C" void nvxnu_spin_lock(void *s)
{
	lck_spin_lock(static_cast<lck_spin_t *>(s));
}

extern "C" void nvxnu_spin_unlock(void *s)
{
	lck_spin_unlock(static_cast<lck_spin_t *>(s));
}

extern "C" unsigned long nvxnu_irq_save(void)
{
	return ml_set_interrupts_enabled(FALSE) ? 1UL : 0UL;
}

extern "C" void nvxnu_irq_restore(unsigned long flags)
{
	ml_set_interrupts_enabled(static_cast<boolean_t>(flags));
}

extern "C" void *nvxnu_rw_alloc(void)
{
	return lck_rw_alloc_init(nvxnu_grp(), LCK_ATTR_NULL);
}

extern "C" void nvxnu_rw_lock_shared(void *rw)
{
	lck_rw_lock_shared(static_cast<lck_rw_t *>(rw));
}

extern "C" void nvxnu_rw_unlock_shared(void *rw)
{
	lck_rw_unlock_shared(static_cast<lck_rw_t *>(rw));
}

extern "C" void nvxnu_rw_lock_exclusive(void *rw)
{
	lck_rw_lock_exclusive(static_cast<lck_rw_t *>(rw));
}

extern "C" void nvxnu_rw_unlock_exclusive(void *rw)
{
	lck_rw_unlock_exclusive(static_cast<lck_rw_t *>(rw));
}

extern "C" nvxnu_u64 nvxnu_nanotime(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

extern "C" nvxnu_u64 nvxnu_abstime_to_ns(nvxnu_u64 abstime)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(abstime, &ns);
	return ns;
}

extern "C" void nvxnu_udelay(nvxnu_u32 us)
{
	IODelay(us);
}

extern "C" void nvxnu_usleep(nvxnu_u32 us)
{
	delay(static_cast<int>(us));
}

extern "C" void nvxnu_yield(void)
{
	delay(1);
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
uint64_t gIrqPasses[2];  // by line: stall, nonstall
volatile int gIrqActive, gGpuOff;
uint32_t gUnhandled;
volatile uint32_t *gPmc;
IOWorkLoop *gIrqLoop;
IOInterruptEventSource *gIrqSources[2];

uint32_t pmc(uint32_t offset)
{
	return gPmc[offset / 4];
}

// An enabled PMC tree with something pending drives its GIC line. A powered-down GPU has none (and no PMC to read).
bool pmcPending()
{
	if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE))
		return false;
	return (pmc(PMC_INTR_0) && (pmc(PMC_INTR_EN_0) & 1)) ||
	       (pmc(PMC_INTR_1) && (pmc(PMC_INTR_EN_1) & 1));
}

[[noreturn]] void gpuPanic(const char *why, const char *what, uint32_t value)
{
	char stall[192], nonstall[192];
	switch_interrupt_describe(kGpuIntids[0], stall, sizeof(stall));
	switch_interrupt_describe(kGpuIntids[1], nonstall, sizeof(nonstall));
	if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE))
		panic("tegra-gm20b: GPU interrupt %s (%s %u, INTID %u/%u) while the GPU is powered "
		      "down for system sleep: %s | %s",
		      why, what, value, kGpuIntids[0], kGpuIntids[1], stall, nonstall);
	panic("tegra-gm20b: GPU interrupt %s (%s %u, INTID %u/%u): PMC INTR %#x/%#x EN %#x/%#x, %s "
	      "| %s",
	      why, what, value, kGpuIntids[0], kGpuIntids[1], pmc(PMC_INTR_0), pmc(PMC_INTR_1),
	      pmc(PMC_INTR_EN_0), pmc(PMC_INTR_EN_1), stall, nonstall);
}

// Work loop action for either GPU line (level; the GIC masks it while this runs). nvkm_intr unarms the GPU's
// top-level interrupt enables, runs the subdev handlers and rearms, so the line is quiet on return. A line nvkm
// keeps finding nothing on is stuck: panic rather than spin. Errors nvkm reported in the pass panic (nvxnu_error).
void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)
{
	// a line taken before nvxnu_irq_suspend() whose pass runs after it: the GPU is off now
	if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE))
		return;
	__atomic_fetch_add(&gIrqRuns, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&gIrqPasses[source == gIrqSources[1]], 1, __ATOMIC_RELAXED);
	gIrqErrorsLen = 0;
	gIrqErrors[0] = 0;
	__atomic_store_n(&gIrqThread, current_thread(), __ATOMIC_RELAXED);
	const int handled = nvxnu_poll_irqs();
	__atomic_store_n(&gIrqThread, static_cast<thread_t>(nullptr), __ATOMIC_RELAXED);
	if (gIrqErrorsLen && __atomic_load_n(&gErrorsFatal, __ATOMIC_ACQUIRE) &&
	    !(gErrorsContained && gErrorsContained(gIrqErrors)))
		panic("tegra-gm20b: GPU error: %s", gIrqErrors);
	if (handled == 1)
		gUnhandled = 0;
	else if (++gUnhandled >= kStorm)
		gpuPanic("storm", "unhandled", gUnhandled);
	nvxnu_wake();
}

// Mach's assert-then-check: a wake between the caller's condition test and this sleep moves the word, and one
// after assert_wait_deadline() clears the wait, so neither is lost. Returns true if woken, false at `until`.
bool sleepUntil(const volatile uint32_t *word, nvxnu_u32 seq, uint64_t until_ns)
{
	const uint64_t now = nvxnu_nanotime();
	if (until_ns <= now)
		return false;
	uint64_t interval;
	nanoseconds_to_absolutetime(until_ns - now, &interval);
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
	thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(word)));
}

// Sleeps until woken or the deadline. Every quiet second it looks at PMC: a GPU interrupt pending and enabled at two
// checks in a row with no interrupt taken in between never reached the CPU.
extern "C" int nvxnu_word_idle(const volatile nvxnu_u32 *word, nvxnu_u32 seq, nvxnu_u64 deadline_ns)
{
	if (!__atomic_load_n(&gIrqActive, __ATOMIC_ACQUIRE))
		panic("tegra-gm20b: GPU wait before its interrupts are attached (INTID %u/%u)",
		      kGpuIntids[0], kGpuIntids[1]);
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
	*stall = __atomic_load_n(&gIrqPasses[0], __ATOMIC_RELAXED);
	*nonstall = __atomic_load_n(&gIrqPasses[1], __ATOMIC_RELAXED);
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
	gPmc = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGpuBar0, 0x1000));
	gIrqLoop = IOWorkLoop::workLoop();
	for (int i = 0; i < 2; i++) {
		IOService *nub = switch_interrupt_nub(kGpuIntids[i], false);
		gIrqSources[i] = gIrqLoop ? IOInterruptEventSource::interruptEventSource(
						    gIrqLoop, gpuInterrupt, nub, 0)
					  : nullptr;
		if (!gPmc || !gIrqSources[i] ||
		    gIrqLoop->addEventSource(gIrqSources[i]) != kIOReturnSuccess)
			panic("tegra-gm20b: cannot attach the GPU %s interrupt (INTID %u)",
			      i ? "nonstall" : "stall", kGpuIntids[i]);
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

extern "C" int nvxnu_mutex_sleep(void *m, const void *event, nvxnu_u64 deadline_ns)
{
	auto *mtx = static_cast<lck_mtx_t *>(m);
	auto *ev = reinterpret_cast<event_t>(const_cast<void *>(event));
	if (deadline_ns == ~0ULL)
		return lck_mtx_sleep(mtx, LCK_SLEEP_DEFAULT, ev, THREAD_UNINT) != THREAD_TIMED_OUT;
	const uint64_t now = nvxnu_nanotime();
	if (deadline_ns <= now)
		return 0;
	uint64_t interval;
	nanoseconds_to_absolutetime(deadline_ns - now, &interval);
	return lck_mtx_sleep_deadline(mtx, LCK_SLEEP_DEFAULT, ev, THREAD_UNINT,
				      mach_absolute_time() + interval) != THREAD_TIMED_OUT;
}

extern "C" void nvxnu_wakeup(const void *event)
{
	thread_wakeup(reinterpret_cast<event_t>(const_cast<void *>(event)));
}

extern "C" void *nvxnu_work_alloc(void (*fn)(void *), void *arg)
{
	return thread_call_allocate(
		reinterpret_cast<thread_call_func_t>(reinterpret_cast<void *>(fn)), arg);
}

extern "C" void nvxnu_work_schedule(void *w)
{
	if (w)
		thread_call_enter(static_cast<thread_call_t>(w));
}

extern "C" void nvxnu_work_cancel_wait(void *w)
{
	if (w)
		thread_call_cancel_wait(static_cast<thread_call_t>(w));
}

extern "C" void nvxnu_work_schedule_after(void *w, nvxnu_u64 ns)
{
	uint64_t interval, deadline;
	nanoseconds_to_absolutetime(ns, &interval);
	clock_absolutetime_interval_to_deadline(interval, &deadline);
	if (w)
		thread_call_enter_delayed(static_cast<thread_call_t>(w), deadline);
}

extern "C" void nvxnu_work_cancel(void *w)
{
	if (w)
		thread_call_cancel(static_cast<thread_call_t>(w));
}

extern "C" volatile void *nvxnu_ioremap(nvxnu_u64 pa, nvxnu_u64 size, int write_combined)
{
	vm_offset_t va = write_combined ? ml_io_map_wcomb(pa, size) : ml_io_map(pa, size);
	return reinterpret_cast<volatile void *>(va);
}

extern "C" void nvxnu_iounmap(volatile void *va, nvxnu_u64 size)
{
	(void)va;
	(void)size;
}

static void dcache_op(const void *va, nvxnu_u64 size, bool invalidate)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
	uintptr_t a = reinterpret_cast<uintptr_t>(va) & ~(line - 1),
		  end = reinterpret_cast<uintptr_t>(va) + size;
	__asm__ volatile("dsb sy" ::: "memory");
	for (; a < end; a += line) {
		if (invalidate)
			__asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
		else
			__asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

extern "C" void nvxnu_dcache_clean(const void *va, nvxnu_u64 size)
{
	dcache_op(va, size, false);
}

extern "C" void nvxnu_dcache_clean_inval(const void *va, nvxnu_u64 size)
{
	dcache_op(va, size, true);
}

// Pages outside the kernel's RAM (device memory a task mapped) have no cacheable alias to maintain.
extern "C" void nvxnu_dcache_clean_inval_pa(nvxnu_u64 pa, nvxnu_u64 size)
{
	if (pmap_valid_page(static_cast<ppnum_t>(atop_64(pa))))
		dcache_op(reinterpret_cast<const void *>(phystokv(pa)), size, true);
}

// A DSB waits for every maintenance op issued before it, so one before the list (prior stores ordered first) and one after
// it (all lines at the point of coherency) cover all pages; a pair per page is ~1800 barriers for a 3.6 MB import.
extern "C" void nvxnu_dcache_clean_inval_pages(const nvxnu_u64 *pas, nvxnu_u32 count)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
	__asm__ volatile("dsb sy" ::: "memory");
	for (nvxnu_u32 i = 0; i < count; i++) {
		if (!pmap_valid_page(static_cast<ppnum_t>(atop_64(pas[i]))))
			continue;
		const uintptr_t va = phystokv(pas[i] & ~0xfffULL);
		for (uintptr_t a = va; a < va + 4096; a += line)
			__asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
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
	uint64_t start = 0, end = 0;
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
		// once the GPU is up (gpu.c "pool scrub").
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
	dcache_op(va, bytes, true);
	for (uint64_t off = 0; off < bytes; off += PAGE_SIZE)
		pmap_set_cache_attributes(static_cast<ppnum_t>(atop_64(first + off)),
					  VM_WIMG_WCOMB);
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

// The descriptor stays wired for the kext's life: nothing gives such memory back.
extern "C" int nvxnu_contig_alloc(nvxnu_u64 bytes, nvxnu_u64 *pa)
{
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task,
		static_cast<IOOptionBits>(kIODirectionInOut) |
			static_cast<IOOptionBits>(kIOMemoryMapperNone) |
			kIOMemoryPhysicallyContiguous,
		bytes, 0xffffffffffULL & ~static_cast<uint64_t>(PAGE_MASK));
	if (!md)
		return -12;
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return -12;
	}
	IOByteCount len = 0;
	const addr64_t first = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
	void *va = md->getBytesNoCopy();
	if (!first || !va || len < bytes) {
		md->complete();
		md->release();
		return -12;
	}
	// The zero fill went through the cacheable aperture: no dirty line of it may land over what the GPU writes later.
	memset(va, 0, bytes);
	dcache_op(va, bytes, true);
	*pa = first;
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
struct nvxnu_vmapping {
	nvxnu_vmapping *next;
	IOMemoryMap *map;
	void *va;
};

static nvxnu_vmapping *vmaps;
static IOLock *vmaps_lock;

extern "C" void *nvxnu_vmap(const nvxnu_u64 *pas, nvxnu_u32 count)
{
	if (!vmaps_lock) {
		IOLock *fresh = IOLockAlloc(), *expected = nullptr;
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
struct nvxnu_fw_entry {
	const char *name;
	const unsigned char *start, *end;
};

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
