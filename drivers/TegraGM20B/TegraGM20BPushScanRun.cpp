// SPDX-License-Identifier: MIT
// The stats' push scan (TegraGM20BUserClientInternal.h kScanEvery), debug kernels: a submission's pushes read and
// counted for the "push" lines, and its process's largest colour target. A profiled submission is read before its kick,
// which stamps it; the others are read on a thread call (SwitchGpuPushScanQueue.h), off the submitting thread.
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
					w.psBarriers,  w.interlocks};
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
			 "%s disp, %s acq; %s PS barrier, %s interlock; %llu bad",
			 i.name, w.scanned, w.submits, v[0],
			 w.bytes ? w.readBytes * 100 / w.bytes : 0, v[1], v[2], v[3], v[4], v[5],
			 v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16],
			 v[17], v[18], v[19], v[20], v[21], v[22], w.unparsed);
		os_log(OS_LOG_DEFAULT, "tegra-gm20b: push %s", line); /* log only */
	}
}

// Count what a submission's pushes ask of the GPU (`push[i].words` null: not read) into `owner`'s window; every 5 s the
// window's lines. `rawMacros`: a job's (only the raw path submits jobs), whose macro slots are the raw path's.
void scanRead(const switch_gpu_scan_words *push, uint32_t count, tgm_prof *prof, bool rawMacros,
	      const char *owner, bool compositor)
{
	PushCounts c = {};
	PushParse p = {};
	p.rawMacros = rawMacros;
	uint64_t budget = kScanMaxBytes;
	bool whole = true;
	tgm_scan::TargetImage largest = {};
	for (uint32_t i = 0; i < count; i++) {
		if (prof && i < TGM_PROF_MAX) {
			prof->cls[i] = TGM_PC_SKIP;
			prof->draws[i] = prof->sampleDraws[i] = prof->kpx[i] = 0;
			prof->interlocks[i] = 0;
		}
		const uint64_t bytes = static_cast<uint64_t>(push[i].dwords) * 4;
		c.bytes += bytes;
		if (!push[i].words || bytes > budget) {
			scanPush(c, p, whole, nullptr, 0);  // not read: no later push is stamped
			continue;
		}
		budget -= bytes;
		c.readBytes += bytes;
		const uint64_t drawsBefore = c.draws;
		const uint64_t clearsBefore = c.clears;
		const uint64_t sampleBefore = c.sampleDraws;
		const uint64_t interlocksBefore = c.interlocks;
		const uint8_t cls = scanPush(c, p, whole, push[i].words, bytes / 4);
		if (c.draws > drawsBefore || c.clears > clearsBefore) {
			const tgm_scan::TargetImage drawn = tgm_scan::targetImage(p);
			if (static_cast<uint64_t>(drawn.width) * drawn.height >
			    static_cast<uint64_t>(largest.width) * largest.height)
				largest = drawn;
		}
		if (prof && i < TGM_PROF_MAX) {
			prof->cls[i] = cls;
			const uint64_t draws = c.draws - drawsBefore;
			const uint64_t sample = c.sampleDraws - sampleBefore;
			prof->draws[i] = static_cast<uint16_t>(draws > 0xffff ? 0xffff : draws);
			prof->sampleDraws[i] =
				static_cast<uint16_t>(sample > 0xffff ? 0xffff : sample);
			const uint64_t kpx = tgm_scan::targetPixels(p) / 1000;
			const uint64_t interlocks = c.interlocks - interlocksBefore;
			prof->kpx[i] = static_cast<uint16_t>(kpx > 0xffff ? 0xffff : kpx);
			prof->interlocks[i] =
				static_cast<uint8_t>(interlocks > 0xff ? 0xff : interlocks);
		}
	}
	c.scanned = 1;
	c.submits = kScanEvery;  // this connection's submissions since its last one read
	if (compositor)
		switch_gpu_compositor_scanned(c.passes, c.draws);
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
	uint64_t dropped = 0;
	if (Deferral *d = __atomic_load_n(&gDeferral, __ATOMIC_ACQUIRE)) {
		IOLockLock(d->lock);
		dropped = gQueue.dropped;
		gQueue.dropped = 0;
		IOLockUnlock(d->lock);
	}
	if (dropped)
		os_log(OS_LOG_DEFAULT,
		       "tegra-gm20b: push scan: %llu submissions not read (every slot waited for "
		       "the thread call)",
		       (unsigned long long)dropped);
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
		scanRead(r->push, r->count, nullptr, r->rawMacros, r->owner, r->compositor);
		IOLockLock(d->lock);
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
	fresh->call = fresh->lock ? thread_call_allocate_with_priority(deferredWork, fresh,
								       THREAD_CALL_PRIORITY_LOW)
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

#if TGM_STATS
void tgm_uc::tgm_target_peaks_line(char *line, size_t size)
{
	if (size)
		line[0] = 0;
	IOLock *held = __atomic_load_n(&gPushLock, __ATOMIC_ACQUIRE);
	if (!held)
		return;
	IOLockLock(held);
	switch_gpu_peaks::line(gTargetPeaks, line, size);
	IOLockUnlock(held);
}
#endif

// By every submission path (exec, SubmitJob), before its kick: every kScanEvery-th submission is read, every
// kProfileEvery-th of those profiled, whose profile the kick stamps. Channels of one connection submit at once with no
// connection lock: the counts are atomic.
const tgm_prof *AGXDeviceUserClient::scanSubmission(const tgm_push *pushes, uint32_t count,
						    tgm_prof &prof, bool rawMacros)
{
	if (!TGM_STATS || !count ||
	    __atomic_fetch_add(&pushSubmits_, 1u, __ATOMIC_RELAXED) % kScanEvery)
		return nullptr;
	const bool profiled =
		count <= TGM_PROF_MAX &&
		__atomic_fetch_add(&pushScans_, 1u, __ATOMIC_RELAXED) % kProfileEvery == 0;
	if (!profiled) {
		deferScan(pushes, count, rawMacros);
		return nullptr;
	}
	scanPushes(pushes, count, &prof, rawMacros);
	return &prof;
}

// A profiled submission's read, now: its pushes through the kernel's view of the BOs they lie in (pool BOs
// write-combined, cached BOs their kernel mapping)
void AGXDeviceUserClient::scanPushes(const tgm_push *pushes, uint32_t count, tgm_prof *prof,
				     bool rawMacros)
{
	if (!TGM_STATS || count > TGM_PROF_MAX)
		return;
	switch_gpu_scan_words words[TGM_PROF_MAX];
	const BindingTable::Entry *hit = nullptr;
	for (uint32_t i = 0; i < count; i++)
		words[i] = {
			pushView(pushes[i].va, static_cast<uint64_t>(pushes[i].dwords) * 4, hit),
			pushes[i].dwords};
	scanRead(words, count, prof, rawMacros, owner_, critical_);
}

// Inside the channel's submit lock (the published bindings it reads stay until it leaves): where each push's words lie in
// the pool, whose kernel mapping lives as long as the kernel, for the thread call to read. A push in any other BO is not read: its mapping may go first.
void AGXDeviceUserClient::deferScan(const tgm_push *pushes, uint32_t count, bool rawMacros)
{
	Deferral *d = deferral();
	if (!d)
		return;
	const BindingTable::Entry *hit = nullptr;
	IOLockLock(d->lock);
	ScanQueue::Record *r = gQueue.reserve();
	if (r) {
		const uint32_t kept = count < kDeferredPushes ? count : kDeferredPushes;
		switch_gpu_scan_fill(*r, owner_, rawMacros, kept, kDeferredPushes, [&](uint32_t i) {
			const uint64_t bytes = static_cast<uint64_t>(pushes[i].dwords) * 4;
			const uint32_t *words = pushView(pushes[i].va, bytes, hit);
			return switch_gpu_scan_words{words && hit && hit->bo->pool_va ? words
										      : nullptr,
						     pushes[i].dwords};
		});
		r->compositor = critical_;
		gQueue.publish();
	}
	IOLockUnlock(d->lock);
	if (r)
		thread_call_enter(d->call);
}
