// SPDX-License-Identifier: MIT
// The spare-page pool, its refill and zeroing, and the owner counters.
#include "TegraGM20BUserClientInternal.h"

namespace
{
constexpr uint64_t kSpareCapBytes = SWITCH_GPU_SPARE_CAP_BYTES, kLearnMaxBytes = 32ULL << 20;
static_assert(kClasses == SWITCH_GPU_SPARE_CLASSES, "the pool's classes are the policy's");
}  // namespace

namespace tgm_uc
{

uint64_t classBytes(uint32_t c)
{
	return switch_gpu_spare_class_bytes(c);
}

uint32_t classUp(uint64_t bytes)
{
	return switch_gpu_spare_class_up(bytes);
}
}  // namespace tgm_uc

namespace
{

SparePool *gSpares;
// XNU's vm_pressure_level_t: 0 normal, 1 warning, 2 urgent, 4 critical.
extern "C" int memorystatus_vm_pressure_level;
#if XNU_12377_121_6
extern "C" uint32_t memorystatus_available_pages;
extern "C" uint32_t memstat_reaper_threshold;
#endif

// Within `capPages` of the reaper's threshold (xnu-12377), or at a raised pressure level
bool belowReaperBy(uint64_t capPages)
{
	const int level = __atomic_load_n(&memorystatus_vm_pressure_level, __ATOMIC_RELAXED);
#if XNU_12377_121_6
	return switch_gpu_spares_give_back(
		level, __atomic_load_n(&memorystatus_available_pages, __ATOMIC_RELAXED),
		__atomic_load_n(&memstat_reaper_threshold, __ATOMIC_RELAXED), capPages);
#else
	(void)capPages;
	return switch_gpu_spares_give_back(level, 0, 0, 0);
#endif
}

// Spares go back within one cap of the reaper's threshold, and none is kept or made again until available pages are two
// caps above it: what a give-back frees cannot by itself bring the refill back
bool memoryPressure()
{
	return belowReaperBy(kSpareCapBytes / PAGE_SIZE);
}

bool spareRoom()
{
	return !belowReaperBy(2 * (kSpareCapBytes / PAGE_SIZE));
}

uint32_t spareSlot(const Spare *s)
{
	return switch_gpu_spare_slot(s->kind, switch_gpu_spare_class_of_spare(s->bytes));
}

void spareFree(Spare *s)
{
	switch_gpu_cached_mapping_drop(&s->mapping);
	switch_gpu_cached_buffer_free(&s->buffer);
	IODelete(s->pas, uint64_t, s->bytes >> 12);
	IODelete(s, Spare, 1);
}
}  // namespace

namespace tgm_uc
{

// New VM pages contain CPU-authored zeros; publish them before exposing any GPU alias.
void cleanZeros(void *va, uint64_t bytes)
{
	switch_dcache_clean_invalidate(va, bytes);
}
}  // namespace tgm_uc

namespace
{

// lock held: ready spares of slots asked before `slot`, the least recently asked first, off their lists until `need`
// more bytes fit under the cap; the caller frees them once the lock is dropped
Spare *evictLocked(SparePool *p, uint32_t slot, uint64_t need)
{
	switch_gpu_spare_books &b = p->books;
	Spare *given = nullptr;
	while (b.held + need > kSpareCapBytes) {
		const uint32_t d =
			switch_gpu_spare_books_evict(b, slot, b.held + need - kSpareCapBytes);
		if (d == SWITCH_GPU_SPARE_NONE || !p->ready[d])
			break;
		Spare *s = p->ready[d];
		p->ready[d] = s->next;
		--b.ready[d];
		b.held -= s->bytes;
		s->next = given;
		given = s;
	}
	return given;
}

void spareFreeAll(Spare *s)
{
	while (s) {
		Spare *next = s->next;
		spareFree(s);
		s = next;
	}
}

bool spareBuffer(SparePool *p, switch_gpu_spare_kind kind, uint64_t bytes,
		 SwitchGpuCachedBuffer *buffer)
{
	if (kind == SWITCH_GPU_SPARE_WC)
		return switch_gpu_cached_buffer_write_combined(bytes, buffer);
	if (switch_gpu_cached_buffer_units(bytes, buffer))
		return true;
	if (switch_gpu_spare_layout(bytes).units) {
#if TGM_STATS
		IOLockLock(p->lock);
		++p->notUnits;
		IOLockUnlock(p->lock);
#else
		(void)p;
#endif
	}
	return switch_gpu_cached_buffer_pages(bytes, buffer);
}

// A wired BO of `slot`'s class and kind, or nullptr: a cached one of 128 KiB units where its layout has them (each one
// GMMU big page). Its pages hold what their last owner left: it is a dirty spare until the copy engine has zeroed it
// (spareWork). The refill thread alone makes these: BoAlloc's own path on the allocating thread never searches for
// units.
Spare *spareMake(SparePool *p, uint32_t slot)
{
	const uint64_t bytes = switch_gpu_spare_slot_bytes(slot);
	const auto kind = static_cast<switch_gpu_spare_kind>(switch_gpu_spare_slot_kind(slot));
	Spare *s = IONew(Spare, 1);
	uint64_t *pas = IONew(uint64_t, bytes >> 12);
	SwitchGpuCachedBuffer buffer = {};
	const bool made = spareBuffer(p, kind, bytes, &buffer);
	const SwitchGpuPageList listed =
		s && pas && made ? switch_gpu_cached_buffer_page_list(buffer, bytes, pas)
				 : SwitchGpuPageList::NoSegment;
	if (listed == SwitchGpuPageList::Listed) {
		*s = {nullptr, buffer, pas, bytes, kind, nullptr};
		return s;
	}
	static bool told;
	if (!__atomic_exchange_n(&told, true, __ATOMIC_RELAXED))
		IOLog("tegra-gm20b: the spare refill could not make a %llu KiB %s spare: %s; "
		      "further ones not logged\n",
		      bytes >> 10, kind == SWITCH_GPU_SPARE_WC ? "write-combined" : "cached",
		      !s || !pas ? "no memory for its records"
		      : !made    ? "the VM gave no pages"
		      : listed == SwitchGpuPageList::NoSegment
			      ? "its descriptor has no physical segment"
		      : listed == SwitchGpuPageList::Misaligned
			      ? "a physical segment is not page-aligned"
			      : "a page is past the GPU's 34 bits");
	switch_gpu_cached_buffer_free(&buffer);
	if (pas)
		IODelete(pas, uint64_t, bytes >> 12);
	if (s)
		IODelete(s, Spare, 1);
	return nullptr;
}

void spareZeroed(void *context, int result)
{
	auto *s = static_cast<Spare *>(context);
	SparePool *p = gSpares;
	const uint32_t slot = spareSlot(s);
	switch_gpu_spare_books &b = p->books;
	IOLockLock(p->lock);
	--b.zeroing[slot];
	const bool keep = !result && spareRoom();
	if (keep) {
		s->next = p->ready[slot];
		p->ready[slot] = s;
		++b.ready[slot];
	} else {
		b.held -= s->bytes;
	}
	const bool firstFailure = result && !p->fillFailed;
	p->fillFailed = p->fillFailed || result;
	const bool hasDirty = p->dirty != nullptr || p->poolDirty != nullptr;
	const bool needFill = !p->fillFailed && spareRoom() &&
			      switch_gpu_spare_books_wants_refill(b, kSpareCapBytes);
	IOLockUnlock(p->lock);
	if (firstFailure)
		IOLog("tegra-gm20b: the copy engine's zero-fill of a %llu KiB spare "
		      "failed (%d): no spare is made until the next BoAlloc\n",
		      s->bytes >> 10, result);
	if (!keep)
		spareFree(s);
	if (hasDirty)
		thread_call_enter(p->recycleCall);
	if (needFill)
		thread_call_enter(p->call);
}

void spareRecycleWork(thread_call_param_t param, thread_call_param_t)
{
	SparePool *p = static_cast<SparePool *>(param);
	for (;;) {
		IOLockLock(p->lock);
		if (PoolZero *z = p->poolDirty) {
			p->poolDirty = z->next;
			IOLockUnlock(p->lock);
			tgm_pool_free_zeroed(z->va, z->bytes, z->pa);
			IOLockLock(p->lock);
			p->poolZeroing -= z->bytes;
			IOLockUnlock(p->lock);
			IODelete(z, PoolZero, 1);
			continue;
		}
		const bool pressure = memoryPressure();
		switch_gpu_spare_books &b = p->books;
		if (Spare *s = p->dirty) {
			const uint32_t slot = spareSlot(s);
			p->dirty = s->next;
			--b.dirty[slot];
			const bool keep = !pressure && spareRoom() &&
					  switch_gpu_spare_books_keep(b, slot, kSpareCapBytes);
			Spare *given = keep ? evictLocked(p, slot, 0) : nullptr;
			if (keep)
				++b.zeroing[slot];
			else
				b.held -= s->bytes;
			IOLockUnlock(p->lock);
			spareFreeAll(given);
			if (!keep) {
				spareFree(s);
				continue;
			}
			const int result = switch_gpu_cached_zero(
				s->pas, uint32_t(s->bytes >> 12),
				s->kind == SWITCH_GPU_SPARE_WC ? nullptr : s->buffer.cpu,
				&s->mapping, spareZeroed, s);
			if (result == -16) {
				IOLockLock(p->lock);
#if TGM_STATS
				++p->zeroBusy;
#endif
				--b.zeroing[slot];
				s->next = p->dirty;
				p->dirty = s;
				++b.dirty[slot];
				IOLockUnlock(p->lock);
				return;
			}
			if (result) {
#if TGM_STATS
				__atomic_add_fetch(&p->zeroFailed, 1, __ATOMIC_RELAXED);
#endif
				spareZeroed(s, result);
			}
			continue;
		}
		if (pressure) {
			Spare *all = nullptr;
			for (uint32_t slot = 0; slot < SWITCH_GPU_SPARE_SLOTS; slot++) {
				while (Spare *s = p->ready[slot]) {
					p->ready[slot] = s->next;
					b.held -= s->bytes;
					s->next = all;
					all = s;
				}
				b.ready[slot] = 0;
			}
			IOLockUnlock(p->lock);
			spareFreeAll(all);
			return;
		}
		IOLockUnlock(p->lock);
		return;
	}
}

void spareReclaimWork(thread_call_param_t, thread_call_param_t)
{
	while (reclaimParkedStep(memoryPressure(), spareRoom()))
		;
}

void spareWork(thread_call_param_t param, thread_call_param_t)
{
	SparePool *p = static_cast<SparePool *>(param);
	switch_gpu_spare_books &b = p->books;
	if (!p->recordRead) {
		p->recordRead = true;
		sparesSeed(p);
	}
	for (;;) {
		IOLockLock(p->lock);
		if (memoryPressure()) {
			bool hasReady = false;
			for (const uint16_t ready : b.ready)
				hasReady = hasReady || ready != 0;
			IOLockUnlock(p->lock);
			if (hasReady)
				thread_call_enter(p->recycleCall);
			return;
		}
		const uint32_t slot = p->fillFailed || !spareRoom()
					      ? SWITCH_GPU_SPARE_NONE
					      : switch_gpu_spare_books_refill(b, kSpareCapBytes);
		const uint64_t bytes =
			slot != SWITCH_GPU_SPARE_NONE ? switch_gpu_spare_slot_bytes(slot) : 0;
		Spare *given = nullptr;
		if (slot != SWITCH_GPU_SPARE_NONE) {
			given = evictLocked(p, slot, bytes);
			// counted from now, on the dirty list it goes to: neither this loop nor a take's kick makes it twice
			b.held += bytes;
			++b.dirty[slot];
		}
		IOLockUnlock(p->lock);
		spareFreeAll(given);
		if (slot == SWITCH_GPU_SPARE_NONE)
			return;
		Spare *s = spareMake(p, slot);
		IOLockLock(p->lock);
		if (!s) {
			--b.dirty[slot];
			b.held -= bytes;
#if TGM_STATS
			++p->refillRefused;
#endif
			IOLockUnlock(p->lock);
			return;
		}
		s->next = p->dirty;
		p->dirty = s;
		IOLockUnlock(p->lock);
		thread_call_enter(p->recycleCall);
	}
}
}  // namespace

namespace tgm_uc
{

SparePool *sparesExisting()
{
	return __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
}

SparePool *spares()
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	if (p)
		return p;
	SparePool *n = IONew(SparePool, 1);
	if (!n)
		return nullptr;
	bzero(n, sizeof(*n));
	switch_gpu_spare_books_init(n->books);
	n->lock = IOLockAlloc();
	// The workers allocate, wire, free and (when the copy engine cannot) zero memory on the CPU: at user priority, so
	// they never take a core from app or render threads.
	if (n->lock) {
		n->call = thread_call_allocate_with_options(spareWork, n, THREAD_CALL_PRIORITY_USER,
							    THREAD_CALL_OPTIONS_ONCE);
		n->recycleCall = thread_call_allocate_with_options(
			spareRecycleWork, n, THREAD_CALL_PRIORITY_USER, THREAD_CALL_OPTIONS_ONCE);
		n->reclaimCall = thread_call_allocate_with_options(
			spareReclaimWork, n, THREAD_CALL_PRIORITY_USER, THREAD_CALL_OPTIONS_ONCE);
		n->recordCall = thread_call_allocate_with_options(
			spareRecordWork, n, THREAD_CALL_PRIORITY_USER, THREAD_CALL_OPTIONS_ONCE);
	}
	if (!n->call || !n->recycleCall || !n->reclaimCall || !n->recordCall) {
		if (n->recordCall)
			thread_call_free(n->recordCall);
		if (n->reclaimCall)
			thread_call_free(n->reclaimCall);
		if (n->recycleCall)
			thread_call_free(n->recycleCall);
		if (n->call)
			thread_call_free(n->call);
		if (n->lock)
			IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return nullptr;
	}
	if (!__atomic_compare_exchange_n(&gSpares, &p, n, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		thread_call_free(n->recordCall);
		thread_call_free(n->reclaimCall);
		thread_call_free(n->recycleCall);
		thread_call_free(n->call);
		IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return p;
	}
	thread_call_enter(n->call);
	return n;
}

Spare *spareTake(uint64_t bytes, switch_gpu_spare_kind kind, bool critical)
{
	SparePool *p = spares();
	const uint32_t c = classUp(bytes);
	if (!p || c >= kClasses)
		return nullptr;
	const uint32_t asked = switch_gpu_spare_slot(kind, c);
	switch_gpu_spare_books &b = p->books;
	IOLockLock(p->lock);
	const uint32_t d = switch_gpu_spare_books_take(b, asked, critical);
	switch_gpu_spare_books_asked(b, asked, d);
	Spare *s = nullptr;
	if (d != SWITCH_GPU_SPARE_NONE) {
		s = p->ready[d];
		p->ready[d] = s->next;
		--b.ready[d];
		b.held -= s->bytes;
	} else {
		const switch_gpu_spare_miss why =
			switch_gpu_spare_books_why(b, asked, critical, memoryPressure());
		switch_gpu_spare_books_learn(b, asked, why, kLearnMaxBytes, kSpareCapBytes);
#if TGM_STATS
		++p->misses[why];
#endif
	}
	const bool pressure = memoryPressure();
	const bool room = spareRoom();
	const bool kickRecycle = pressure && b.held != 0;
	const bool kickFill =
		!pressure && room && switch_gpu_spare_books_wants_refill(b, kSpareCapBytes);
	if (kickFill || kickRecycle)
		p->fillFailed = false;
	IOLockUnlock(p->lock);
	bool kickReclaim = false;
	// the parked BOs a demand reclaims are cached ones
	if (!s && kind == SWITCH_GPU_SPARE_CACHED && !pressure && room &&
	    parkedNotifyDemand(classBytes(c)))
		kickReclaim = true;
	else if (pressure && parkedHasAny())
		kickReclaim = true;
	if (kickRecycle)
		thread_call_enter(p->recycleCall);
	if (kickFill)
		thread_call_enter(p->call);
	if (kickReclaim)
		thread_call_enter(p->reclaimCall);
	return s;
}

// A process released its idle memory (method 43: memory pressure, or gone to the background): what a take does under
// pressure, without waiting for one, since nothing reads the pressure level while the GPU allocates nothing and the pool
// holds up to its cap wired; and this boot's demand record set again for the next boot.
void sparesPressureLook()
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	if (!p)
		return;
	thread_call_enter(p->recordCall);
	if (!memoryPressure())
		return;
	thread_call_enter(p->recycleCall);
	if (parkedHasAny())
		thread_call_enter(p->reclaimCall);
}

bool sparesPressure()
{
	return memoryPressure();
}

bool sparesHasRoom()
{
	return spareRoom();
}

void kickSpareWork()
{
	if (SparePool *p = spares())
		thread_call_enter(p->reclaimCall);
}

// Debug kernels: a BoAlloc that no spare can serve by its flags (contiguous).
void spareIneligible()
{
#if TGM_STATS
	if (SparePool *p = spares()) {
		IOLockLock(p->lock);
		++p->misses[SWITCH_GPU_SPARE_INELIGIBLE];
		IOLockUnlock(p->lock);
	}
#endif
}

// A freed cached or system BO (its prepared buffer, which nothing else holds, with its `bytes` and page list) to the
// spares of its kind: true if taken, then the caller must not touch the buffer, pas or its persistent mapping again.
bool spareKeep(const SwitchGpuCachedBuffer &buffer, uint64_t *pas, uint64_t bytes,
	       switch_gpu_spare_kind kind, switch_gpu_cached_mapping *mapping)
{
	if (memoryPressure() || !spareRoom())
		return false;
	if (bytes > kSpareCapBytes || classUp(bytes) >= kClasses)
		return false;
	SparePool *p = spares();
	Spare *s = p && switch_gpu_cached_buffer_alone(buffer) ? IONew(Spare, 1) : nullptr;
	if (!s)
		return false;
	*s = {nullptr, buffer, pas, bytes, kind, mapping};
	IOLockLock(p->lock);
	s->next = p->dirty;
	p->dirty = s;
	p->books.held += bytes;
	++p->books.dirty[spareSlot(s)];
	IOLockUnlock(p->lock);
	thread_call_enter(p->recycleCall);
	return true;
}
}  // namespace tgm_uc

void switch_gpu_cached_retire(SwitchGpuCachedBuffer buffer, uint64_t *pages, uint32_t count,
			      switch_gpu_spare_kind kind, switch_gpu_cached_mapping *mapping)
{
	if (buffer.cpu && spareKeep(buffer, pages, uint64_t(count) << 12, kind, mapping))
		return;
	switch_gpu_cached_mapping_drop(&mapping);
	switch_gpu_cached_buffer_free(&buffer);
	IODelete(pages, uint64_t, count);
}
