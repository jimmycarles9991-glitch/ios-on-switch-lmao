// SPDX-License-Identifier: MIT
// The stats' push scan (TegraGM20BUserClientInternal.h kScanFirst, kScanEvery), debug kernels: a submission's pushes
// read and counted for the "push" lines, and its process's largest colour target. A profiled submission, and each of an
// app connection's first kScanFirst, is read before its kick (a profiled one's kick stamps it); the others are read on a
// thread call (SwitchGpuPushScanQueue.h), off the submitting thread.
#include "TegraGM20BUserClientInternal.h"
#include "SwitchGpuPushScanQueue.h"
#include <kern/thread_call.h>

namespace
{
constexpr uint64_t kScanMaxBytes = 512 << 10, kScanWindowNs = 5000000000ULL;
constexpr uint32_t kDeferredRecords = 4, kDeferredPushes = 512;

struct PushOwner {
	char name[17];
	PushCounts c;
};

PushOwner gPushOwners[kPushOwners];
uint64_t gPushSince;
IOLock *gPushLock;
switch_gpu_peaks::Peak gTargetPeaks[switch_gpu_peaks::kOwners];

using ScanQueue = SwitchGpuPushScanQueue<kDeferredRecords, kDeferredPushes>;

struct Deferral {
	IOLock *lock;
	thread_call_t call;
};

Deferral *gDeferral;
ScanQueue gQueue;  // under gDeferral's lock

bool pushLockMade()
{
	if (__atomic_load_n(&gPushLock, __ATOMIC_ACQUIRE))
		return true;
	IOLock *fresh = IOLockAlloc();
	// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap writes the current value back into it
	IOLock *expected = nullptr;
	if (!__atomic_compare_exchange_n(&gPushLock, &expected, fresh, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE))
		IOLockFree(fresh);
	return __atomic_load_n(&gPushLock, __ATOMIC_ACQUIRE) != nullptr;
}

void logWindow(const PushOwner (&window)[kPushOwners])
{
	static char line
		[700];  // only the read that ends a window (one per 5 s, chosen under gPushLock) logs
	for (const auto &i : window) {
		const PushCounts &w = i.c;
		if (!w.scanned)
			continue;
		const uint64_t n = w.scanned;
		const uint64_t per[] = {w.bytes >> 10, w.passes,           w.depthPasses,
					w.msaaPasses,  w.targets,          w.linearTargets,
					w.draws,       w.clears,           w.wfi,
					w.shaderInval, w.shaderInvalNoWfi, w.texInval,
					w.setRef,      w.l2Inval,          w.hostWfi,
					w.ceCopies,    w.ceBytes >> 10,    w.ceWfi,
					w.blits,       w.dispatches,       w.acquires,
					w.psBarriers,  w.interlocks,       w.twinDraws};
		constexpr size_t kFields = sizeof(per) / sizeof(per[0]);
		char v[kFields][24];
		for (size_t k = 0; k < kFields; k++)
			snprintf(v[k], sizeof(v[k]), "%llu.%llu", per[k] / n, per[k] * 10 / n % 10);
		// one string argument: os_log drops a message whose scalar arguments outgrow its buffer (gpu_stats_tick.c tgm_stats_tick)
		snprintf(line, sizeof(line),
			 "%s: %llu of %llu submits read, %s KiB each (%llu%% readable); per "
			 "submit: %s passes "
			 "(%s depth, %s MSAA), %s RTs (%s pitch), %s draws, %s clears; %s WFI, %s "
			 "shinv+WFI, %s "
			 "shinv, %s texinv, %s setref, %s L2 inv, %s host WFI; %s CE (%s KiB), %s "
			 "CE WFI, %s 2D, "
			 "%s disp, %s acq; %s PS barrier, %s interlock, %s twin draws; %llu bad",
			 i.name, w.scanned, w.submits, v[0],
			 w.bytes ? w.readBytes * 100 / w.bytes : 0, v[1], v[2], v[3], v[4], v[5],
			 v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16],
			 v[17], v[18], v[19], v[20], v[21], v[22], v[23], w.unparsed);
		os_log(OS_LOG_DEFAULT, "tegra-gm20b: push %s", line); /* log only */
	}
}

// Count a submission's read into `owner`'s window and its largest target into the peaks; every 5 s the window's lines.
void scanCount(const tgm_scan::SubmissionRead &read, const char *owner, bool compositor,
	       uint32_t submits)
{
	tgm_scan::PushCounts c = read.c;
	const tgm_scan::TargetImage largest = read.largest;
	c.submits = submits;
	if (compositor)
		switch_gpu_compositor_scanned(c.passes, c.draws, c.interlocks, c.twinDraws);
	if (!pushLockMade())
		return;
	const uint64_t now = nowNs();
	static PushOwner window[kPushOwners];
	bool log = false;
	IOLockLock(gPushLock);
	PushOwner *o = nullptr;
	for (uint32_t i = 0; i < kPushOwners - 1 && !o; i++) {
		if (!gPushOwners[i].c.scanned ||
		    !strncmp(gPushOwners[i].name, owner, sizeof(gPushOwners[i].name) - 1))
			o = &gPushOwners[i];
	}
	if (!o) {  // more processes than lines this window: the last line takes the rest
		o = &gPushOwners[kPushOwners - 1];
		strlcpy(o->name, "(others)", sizeof(o->name));
	} else if (!o->c.scanned) {
		strlcpy(o->name, owner, sizeof(o->name));
	}
	for (size_t k = 0; k < sizeof(c) / sizeof(uint64_t); k++)
		reinterpret_cast<uint64_t *>(&o->c)[k] += reinterpret_cast<const uint64_t *>(&c)[k];
	switch_gpu_peaks::note(gTargetPeaks, owner, largest.width, largest.height, largest.samples);
	if (!gPushSince) {
		gPushSince = now;
	} else if (now - gPushSince >= kScanWindowNs) {
		gPushSince = now;
		memcpy(window, gPushOwners, sizeof(window));
		bzero(gPushOwners, sizeof(gPushOwners));
		log = true;
	}
	IOLockUnlock(gPushLock);
	if (!log)
		return;
	logWindow(window);
	uint64_t dropped = 0, late = 0;
	if (Deferral *d = __atomic_load_n(&gDeferral, __ATOMIC_ACQUIRE)) {
		IOLockLock(d->lock);
		dropped = gQueue.dropped;
		late = gQueue.late;
		gQueue.dropped = gQueue.late = 0;
		IOLockUnlock(d->lock);
	}
	if (dropped || late)
		os_log(OS_LOG_DEFAULT,
		       "tegra-gm20b: push scan: %llu submissions not read (every slot waited for "
		       "the thread call), %llu read too late (their submission may have completed "
		       "first: not counted)",
		       (unsigned long long)dropped, (unsigned long long)late);
}

// Whether the record's submission may have completed: its channel's fence reached its last seqno, or the fence page
// went to another connection
bool submissionPassed(const ScanQueue::Record &r)
{
	const volatile uint32_t *fence = tgm_fence_word(r.fence);
	return !fence || static_cast<int32_t>(*fence - r.seqno) >= 0 ||
	       tgm_fence_owner(r.fence) != r.fenceOwner;
}

void deferredWork(thread_call_param_t param, thread_call_param_t)
{
	Deferral *d = static_cast<Deferral *>(param);
	for (;;) {
		IOLockLock(d->lock);
		const ScanQueue::Record *r = gQueue.take();
		IOLockUnlock(d->lock);
		if (!r)
			return;
		tgm_scan::SubmissionRead read = {};
		const bool kept = switch_gpu_scan_record(
			*r, submissionPassed, [&](const ScanQueue::Record &record) {
				read = tgm_scan::readSubmission(record.push, record.count, nullptr,
								record.metalMacros, kScanMaxBytes);
			});
		if (kept)
			scanCount(read, r->owner, r->compositor, r->submits);
		IOLockLock(d->lock);
		gQueue.late += !kept;
		gQueue.done();
		IOLockUnlock(d->lock);
	}
}

Deferral *deferral()
{
	Deferral *d = __atomic_load_n(&gDeferral, __ATOMIC_ACQUIRE);
	if (d)
		return d;
	Deferral *fresh = IONew(Deferral, 1);
	if (!fresh)
		return nullptr;
	bzero(fresh, sizeof(*fresh));
	fresh->lock = IOLockAlloc();
	fresh->call = fresh->lock ? thread_call_allocate_with_options(deferredWork, fresh,
								      THREAD_CALL_PRIORITY_LOW,
								      THREAD_CALL_OPTIONS_ONCE)
				  : nullptr;
	if (!fresh->call || !__atomic_compare_exchange_n(&gDeferral, &d, fresh, false,
							 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		if (fresh->call)
			thread_call_free(fresh->call);
		if (fresh->lock)
			IOLockFree(fresh->lock);
		IODelete(fresh, Deferral, 1);
		return d;
	}
	return fresh;
}
}  // namespace

#if TGM_DEBUG_PROBES
void tgm_uc::tgm_target_peaks_line(char *line, size_t size)
{
	if (size)
		line[0] = 0;
#if TGM_PROF
	IOLock *held = __atomic_load_n(&gPushLock, __ATOMIC_ACQUIRE);
	if (!held)
		return;
	IOLockLock(held);
	switch_gpu_peaks::line(gTargetPeaks, line, size);
	IOLockUnlock(held);
#endif
}
#endif

// By every submission path (exec, SubmitJob), before its kick: the submissions tgm_scan::scanWeight picks are read,
// every kProfileEvery-th of those profiled, whose profile the kick stamps. An app connection's first kScanFirst are read
// here, once a connection: a process that submits a few times (a lock screen poster, whose first is an upload and whose
// draws follow) may finish before a thread call reads them or drops them, and its pushes may lie outside the pool. The
// render server's are not: it waits for this thread. Those reads (write-combined, up to kScanMaxBytes each) are a debug
// kernel's launch-time cost; a release kernel returns at its first line. Channels of one connection submit at once with
// no connection lock: the counts are atomic.
const tgm_prof *AGXDeviceUserClient::scanSubmission(uint32_t slot, uint32_t fences,
						    const tgm_push *pushes, uint32_t count,
						    tgm_prof &prof, bool metalMacros)
{
	if (!TGM_PROF || !count)
		return nullptr;
	const uint32_t n = __atomic_fetch_add(&pushSubmits_, 1u, __ATOMIC_RELAXED);
	const uint32_t submits = tgm_scan::scanWeight(n, kScanFirst, kScanEvery);
	if (!submits)
		return nullptr;
	const bool profiled =
		count <= TGM_PROF_MAX &&
		__atomic_fetch_add(&pushScans_, 1u, __ATOMIC_RELAXED) % kProfileEvery == 0;
	if (profiled) {
		scanPushes(pushes, count, &prof, metalMacros, submits);
		return &prof;
	}
	if (n < kScanFirst && !critical_ && count <= TGM_PROF_MAX)
		scanPushes(pushes, count, nullptr, metalMacros, submits);
	else
		deferScan(slot, fences, pushes, count, metalMacros, submits);
	return nullptr;
}

// A profiled submission's read, now: its pushes through the kernel's view of the BOs they lie in (pool BOs
// write-combined, cached BOs their kernel mapping)
void AGXDeviceUserClient::scanPushes(const tgm_push *pushes, uint32_t count, tgm_prof *prof,
				     bool metalMacros, uint32_t submits)
{
	if (!TGM_PROF || count > TGM_PROF_MAX)
		return;
	switch_gpu_scan_words words[TGM_PROF_MAX];
	const BindingTable::Entry *hit = nullptr;
	for (uint32_t i = 0; i < count; i++)
		words[i] = {
			pushView(pushes[i].va, static_cast<uint64_t>(pushes[i].dwords) * 4, hit),
			pushes[i].dwords};
	scanCount(tgm_scan::readSubmission(words, count, prof, metalMacros, kScanMaxBytes), owner_,
		  critical_, submits);
}

// Inside the channel's submit lock (the published bindings it reads stay until it leaves): where each push's words lie in
// the pool, whose kernel mapping lives as long as the kernel, for the thread call to read. A push in any other BO is not read: its mapping may go first.
// The record names the submission's last seqno on its channel's fence (its `fences` internal ones come before it),
// past which its words may be another's.
void AGXDeviceUserClient::deferScan(uint32_t slot, uint32_t fences, const tgm_push *pushes,
				    uint32_t count, bool metalMacros, uint32_t submits)
{
	Deferral *d = deferral();
	if (!d)
		return;
	const BindingTable::Entry *hit = nullptr;
	IOLockLock(d->lock);
	ScanQueue::Record *r = gQueue.reserve();
	if (r) {
		const uint32_t kept = count < kDeferredPushes ? count : kDeferredPushes;
		switch_gpu_scan_fill(
			*r, owner_, metalMacros, kept, kDeferredPushes, [&](uint32_t i) {
				const uint64_t bytes = static_cast<uint64_t>(pushes[i].dwords) * 4;
				const uint32_t *words = pushView(pushes[i].va, bytes, hit);
				return switch_gpu_scan_words{
					words && hit && hit->bo->pool_va ? words : nullptr,
					pushes[i].dwords};
			});
		r->compositor = critical_;
		r->submits = submits;
		r->fence = tgm_fence_offset(fence_va_ + static_cast<size_t>(slot) * 16);
		r->fenceOwner = tgm_fence_owner(r->fence);
		r->seqno = tgm_chan_submitted(chans_[slot]) + fences + 1;
		gQueue.publish();
	}
	IOLockUnlock(d->lock);
	if (r)
		thread_call_enter(d->call);
}
