// SPDX-License-Identifier: MIT
#include "SwitchGpuPathStats.h"
#include "TegraGM20BGpu.h"
extern "C" {
#include "TegraGM20BSched.h"
}
#if SWITCH_GPU_PATH_STATS
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "SwitchGpuCompositorRound.h"
#include "SwitchGpuCompositorSync.h"
#include "SwitchGpuPreemptLanding.h"
#include "SwitchGpuSpanNotes.h"

extern "C" int proc_selfpid(void);
extern "C" void _disable_preemption(void);  // NOLINT(bugprone-reserved-identifier): XNU's own name
extern "C" void _enable_preemption(void);   // NOLINT(bugprone-reserved-identifier): XNU's own name

namespace
{
const char *const kNames[SWITCH_GPU_PATH_COUNT] = {
	"internal_jobs",        "external_jobs", "unlocked_writes", "volatile_binds",
	"lean_copy_jobs",       "control_bos",   "cpu_completes",   "ranged_uses",
	"visible_waits",        "native_views",  "native_reads",    "native_tiled_views",
	"event_jobs",           "ready_arms",    "submit_jobs",     "set_jobs",
	"stale_sets",           "address_jobs",  "shared_uses",     "relayout_fresh",
	"relayout_deferred",    "relayout_lazy", "relayout_never",  "relayout_moved",
	"quiet_jobs",           "wake_asks",     "writer_promises", "promise_waits",
	"promise_cpu_releases",
};

// Every path is taken in the syscall or trap of the process it is for, so each process counts its own: a reader of
// kern.switch_gpu_paths sees what its own work took, which the compositor's frames do not move. A process with a slot
// counts with no lock: each CPU has its own cells, which only that CPU stores to, with preemption off, and readers sum
// the CPUs. The lock only hands slots out. The least recently counting process gives up its slot, so a count can land in
// the slot's next owner only when it races a reuse of a process that had stopped counting.
constexpr uint32_t kProcesses = 16, kCpus = 4;  // the X1's cores

struct Process {
	int pid;  // atomic: published once the slot's counts are zeroed
	uint64_t last;
	uint64_t counts[kCpus][SWITCH_GPU_PATH_COUNT];
};

IOLock *lock;
Process processes[kProcesses];
uint64_t window[kCpus][SWITCH_GPU_PATH_COUNT];
// the window line's reader alone: the totals it last printed from
uint64_t windowPrinted[SWITCH_GPU_PATH_COUNT];

void bump(uint64_t &cell, uint32_t count)
{
	__atomic_store_n(&cell, __atomic_load_n(&cell, __ATOMIC_RELAXED) + count, __ATOMIC_RELAXED);
}

uint64_t total(const uint64_t (&cells)[kCpus][SWITCH_GPU_PATH_COUNT], uint32_t path)
{
	uint64_t sum = 0;
	for (const auto &cpu : cells)
		sum += __atomic_load_n(&cpu[path], __ATOMIC_RELAXED);
	return sum;
}

IOLock *processLock()
{
	if (IOLock *made = __atomic_load_n(&lock, __ATOMIC_ACQUIRE))
		return made;
	IOLock *made = IOLockAlloc();
	IOLock *expected = nullptr;
	if (made && !__atomic_compare_exchange_n(&lock, &expected, made, false, __ATOMIC_ACQ_REL,
						 __ATOMIC_ACQUIRE)) {
		IOLockFree(made);
		return expected;
	}
	return made;
}

Process *find(int pid)
{
	for (Process &p : processes)
		if (__atomic_load_n(&p.pid, __ATOMIC_ACQUIRE) == pid)
			return &p;
	return nullptr;
}

// processLock held
Process *claim(int pid)
{
	if (Process *p = find(pid))
		return p;
	Process *oldest = &processes[0];
	for (Process &p : processes)
		if (__atomic_load_n(&p.last, __ATOMIC_RELAXED) <
		    __atomic_load_n(&oldest->last, __ATOMIC_RELAXED))
			oldest = &p;
	for (auto &cpu : oldest->counts)
		for (uint64_t &count : cpu)
			__atomic_store_n(&count, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&oldest->last, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&oldest->pid, pid, __ATOMIC_RELEASE);
	return oldest;
}

void format(char *line, size_t size, size_t used, const uint64_t *counts)
{
	for (uint32_t i = 0; i < SWITCH_GPU_PATH_COUNT && used < size; ++i) {
		const int wrote = snprintf(line + used, size - used, "%s%s=%llu", used ? " " : "",
					   kNames[i], static_cast<unsigned long long>(counts[i]));
		used += wrote > 0 ? static_cast<size_t>(wrote) : 0;
	}
}

int pathsSysctl SYSCTL_HANDLER_ARGS
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	char line[512];
	switch_gpu_path_line(line, sizeof(line));
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

struct sysctl_oid gPathsOid;

// Writer promises given up on since boot (each reported in one "was not kept" line, gpu_promise.c), and how long the
// reader of the last one had waited: counted for every process, as a present's deadline gives one up in the kernel's.
uint64_t promisesAbandoned, promiseWaitedNs;
uint64_t directPromised[3];  // SWITCH_GPU_PATH_DIRECT_*

int promisesSysctl SYSCTL_HANDLER_ARGS
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	char line[160];
	snprintf(line, sizeof(line),
		 "abandoned=%llu last_waited_ms=%llu direct_promised=%llu direct_kept=%llu "
		 "direct_given_up=%llu",
		 static_cast<unsigned long long>(
			 __atomic_load_n(&promisesAbandoned, __ATOMIC_ACQUIRE)),
		 static_cast<unsigned long long>(
			 __atomic_load_n(&promiseWaitedNs, __ATOMIC_RELAXED) / 1000000),
		 static_cast<unsigned long long>(
			 __atomic_load_n(&directPromised[0], __ATOMIC_RELAXED)),
		 static_cast<unsigned long long>(
			 __atomic_load_n(&directPromised[1], __ATOMIC_RELAXED)),
		 static_cast<unsigned long long>(
			 __atomic_load_n(&directPromised[2], __ATOMIC_RELAXED)));
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

struct sysctl_oid gPromisesOid;
switch_gpu_spans::Notes gSpans;
uint64_t gCompositorJobMax;
switch_gpu_comp_sync gSyncWindow, gSyncAsked;
switch_gpu_comp_round gRound;
constexpr uint64_t kPeriodNs = 16666667;
}  // namespace

extern "C" void switch_gpu_path_promise_direct(int what)
{
	if (what >= 0 && what < 3)
		__atomic_fetch_add(&directPromised[what], 1, __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_path_promise_abandoned(uint64_t waited_ns)
{
	__atomic_store_n(&promiseWaitedNs, waited_ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&promisesAbandoned, 1, __ATOMIC_RELEASE);
}

extern "C" void switch_gpu_span_note(const char *owner, const char *text)
{
	IOLock *held = processLock();
	if (!held)
		return;
	IOLockLock(held);
	switch_gpu_spans::add(gSpans, owner, text);
	IOLockUnlock(held);
}

extern "C" void switch_gpu_compositor_job(uint64_t ns)
{
	switch_gpu_comp_round_job(&gRound, ns, kPeriodNs);
	uint64_t max = __atomic_load_n(&gCompositorJobMax, __ATOMIC_RELAXED);
	while (ns > max && !__atomic_compare_exchange_n(&gCompositorJobMax, &max, ns, false,
							__ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

extern "C" void switch_gpu_compositor_import(uint64_t ns)
{
	switch_gpu_comp_round_import(&gRound, ns);
}

extern "C" void switch_gpu_compositor_alloc(uint64_t ns, int cold)
{
	switch_gpu_comp_round_alloc(&gRound, ns, cold);
}

extern "C" void switch_gpu_compositor_scanned(uint64_t passes, uint64_t draws)
{
	switch_gpu_comp_round_scanned(&gRound, passes, draws);
}

extern "C" void switch_gpu_compositor_sync(uint32_t syncs, uint64_t ns, int behind)
{
	switch_gpu_comp_sync_add(&gSyncWindow, syncs, ns, behind);
	switch_gpu_comp_sync_add(&gSyncAsked, syncs, ns, behind);
}

extern "C" void switch_gpu_compositor_sync_window(char *line, size_t size)
{
	switch_gpu_comp_sync taken = {};
	switch_gpu_comp_sync_take(&gSyncWindow, &taken);
	if (size)
		line[0] = 0;
	if (taken.subs)
		switch_gpu_comp_sync_format(line, size, &taken);
}

extern "C" void switch_gpu_span_line(char *line, size_t size)
{
	if (size)
		line[0] = 0;
	IOLock *held = processLock();
	if (!held)
		return;
	switch_gpu_comp_sync sync = {};
	switch_gpu_comp_sync_take(&gSyncAsked, &sync);
	switch_gpu_comp_round round = {};
	switch_gpu_comp_round_take(&gRound, &round);
	char head[320];
	const int put = switch_gpu_comp_round_format(head, sizeof(head), &round);
	const size_t used =
		put > 0 && static_cast<size_t>(put) < sizeof(head) ? static_cast<size_t>(put) : 0;
	switch_gpu_comp_sync_format(head + used, sizeof(head) - used, &sync);
	IOLockLock(held);
	switch_gpu_spans::take(gSpans, __atomic_exchange_n(&gCompositorJobMax, 0, __ATOMIC_RELAXED),
			       head, line, size);
	IOLockUnlock(held);
}

extern "C" void switch_gpu_path_taken(enum switch_gpu_path path, uint32_t count)
{
	if (!count)
		return;
	const int pid = proc_selfpid();
	Process *p = find(pid);
	if (!p) {
		IOLock *held = processLock();
		if (!held)
			return;
		IOLockLock(held);
		p = claim(pid);
		IOLockUnlock(held);
	}
	_disable_preemption();
	const uint32_t cpu = static_cast<uint32_t>(cpu_number()) % kCpus;
	bump(window[cpu][path], count);
	bump(p->counts[cpu][path], count);
	_enable_preemption();
	__atomic_store_n(&p->last, mach_absolute_time(), __ATOMIC_RELAXED);
}

extern "C" void switch_gpu_path_job(uint32_t visibility, int unlocked_writes, int lean_copy,
				    uint32_t ranged_uses)
{
	if (visibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT)
		switch_gpu_path_taken(visibility & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY
					      ? SWITCH_GPU_PATH_EXTERNAL_JOB
					      : SWITCH_GPU_PATH_INTERNAL_JOB,
				      1);
	switch_gpu_path_taken(SWITCH_GPU_PATH_UNLOCKED_WRITE, unlocked_writes ? 1 : 0);
	switch_gpu_path_taken(SWITCH_GPU_PATH_LEAN_COPY_JOB, lean_copy ? 1 : 0);
	switch_gpu_path_taken(SWITCH_GPU_PATH_RANGED_USE, ranged_uses);
	switch_gpu_path_taken(SWITCH_GPU_PATH_EVENT_JOB,
			      (visibility & SWITCH_GPU_SUBMIT_FORCE_FENCE) ? 1 : 0);
}

extern "C" void switch_gpu_path_window(char *line, size_t size)
{
	uint64_t counts[SWITCH_GPU_PATH_COUNT];
	for (uint32_t i = 0; i < SWITCH_GPU_PATH_COUNT; ++i) {
		const uint64_t now = total(window, i);
		counts[i] = now - windowPrinted[i];
		windowPrinted[i] = now;
	}
	if (size)
		line[0] = 0;
	format(line, size, 0, counts);
}

extern "C" void switch_gpu_path_line(char *line, size_t size)
{
	if (!size)
		return;
	const int pid = proc_selfpid();
	uint64_t counts[SWITCH_GPU_PATH_COUNT] = {};
	if (const Process *p = find(pid))
		for (uint32_t i = 0; i < SWITCH_GPU_PATH_COUNT; ++i)
			counts[i] = total(p->counts, i);
	const int head = snprintf(line, size, "flags=%#x pid=%d", switch_gpu_info_flags(), pid);
	format(line, size, head > 0 ? static_cast<size_t>(head) : 0, counts);
	uint32_t preempts = 0;
	uint64_t preemptMax = 0;
	switch_gpu_preempt_counts landed = {};
	tgm_hold_samples_take(pid, &preempts, &preemptMax, &landed);
	size_t used = strlen(line);
	if (used < size)
		snprintf(line + used, size - used,
			 " preempts=%u preempt_max_us=%llu preempt_mid_job=%u preempt_job_end=%u "
			 "preempt_unknown=%u",
			 preempts, static_cast<unsigned long long>(preemptMax / 1000),
			 landed.landed[SWITCH_GPU_LANDED_MID_JOB],
			 landed.landed[SWITCH_GPU_LANDED_JOB_END],
			 landed.landed[SWITCH_GPU_LANDED_UNKNOWN]);
	static const char *const kBuckets[SWITCH_GPU_PREEMPT_BUCKETS] = {
		"lt250", "lt500", "lt1000", "lt2000", "lt4000", "ge4000"};
	for (unsigned b = 0; b < SWITCH_GPU_PREEMPT_BUCKETS; b++) {
		used = strlen(line);
		if (used < size)
			snprintf(line + used, size - used, " preempt_%s_us=%u", kBuckets[b],
				 landed.bucket[b]);
	}
}

extern "C" void switch_gpu_path_register(void)
{
	switch_sysctl_oid(
		&gPathsOid, &sysctl__kern_children, "switch_gpu_paths",
		static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED),
		pathsSysctl, "A",
		"GM20B capability flags, and the paths behind them the reading process took");
	sysctl_register_oid(&gPathsOid);
	switch_sysctl_oid(
		&gPromisesOid, &sysctl__kern_children, "switch_gpu_promises",
		static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED),
		promisesSysctl, "A",
		"writer promises given up on since boot, how long the last one's reader had "
		"waited, and the direct "
		"presents of promised frames watched on their promise, handed to the job that kept "
		"it, and given up");
	sysctl_register_oid(&gPromisesOid);
}
#endif
