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
#include <string.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "SwitchGpuCompositorSync.h"
#include "SwitchGpuPreemptLanding.h"
#include "SwitchGpuSpanNotes.h"

extern "C" int proc_selfpid(void);

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
// kern.switch_gpu_paths sees what its own work took, which the compositor's frames do not move. The least recently
// counting process gives up its slot.
constexpr uint32_t kProcesses = 16;

struct Process {
	int pid;
	uint64_t last;
	uint64_t counts[SWITCH_GPU_PATH_COUNT];
};

IOLock *lock;
Process processes[kProcesses];
uint64_t window[SWITCH_GPU_PATH_COUNT];

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

// processLock held
Process *slot(int pid, bool claim)
{
	Process *oldest = &processes[0];
	for (Process &p : processes) {
		if (p.pid == pid)
			return &p;
		if (p.last < oldest->last)
			oldest = &p;
	}
	if (!claim)
		return nullptr;
	*oldest = {pid, 0, {}};
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
	uint64_t max = __atomic_load_n(&gCompositorJobMax, __ATOMIC_RELAXED);
	while (ns > max && !__atomic_compare_exchange_n(&gCompositorJobMax, &max, ns, false,
							__ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
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
	char head[160];
	switch_gpu_comp_sync_format(head, sizeof(head), &sync);
	IOLockLock(held);
	switch_gpu_spans::take(gSpans, __atomic_exchange_n(&gCompositorJobMax, 0, __ATOMIC_RELAXED),
			       head, line, size);
	IOLockUnlock(held);
}

extern "C" void switch_gpu_path_taken(enum switch_gpu_path path, uint32_t count)
{
	if (!count)
		return;
	__atomic_fetch_add(&window[path], count, __ATOMIC_RELAXED);
	IOLock *held = processLock();
	if (!held)
		return;
	IOLockLock(held);
	Process *p = slot(proc_selfpid(), true);
	p->last = mach_absolute_time();
	p->counts[path] += count;
	IOLockUnlock(held);
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
	for (uint32_t i = 0; i < SWITCH_GPU_PATH_COUNT; ++i)
		counts[i] = __atomic_exchange_n(&window[i], 0, __ATOMIC_RELAXED);
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
	if (IOLock *held = processLock()) {
		IOLockLock(held);
		if (const Process *p = slot(pid, false))
			memcpy(counts, p->counts, sizeof(counts));
		IOLockUnlock(held);
	}
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
