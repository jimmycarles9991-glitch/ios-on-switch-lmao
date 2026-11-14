// SPDX-License-Identifier: MIT
// The spare-page pool, its refill and zeroing, and the owner counters.
#include "TegraGM20BUserClientInternal.h"

namespace
{
constexpr uint64_t kSpareCapBytes = 96ULL << 20, kLearnMaxBytes = 32ULL << 20;

struct WarmClass {
	uint64_t bytes;
	uint16_t ready;
};

constexpr WarmClass kWarm[] = {
	{64ULL << 10, 8}, {4ULL << 20, 2}, {8ULL << 20, 1}, {16ULL << 20, 2}};
}  // namespace

namespace tgm_uc
{

uint64_t classBytes(uint32_t c)
{
	return c < 5 ? 4096ULL << c : ((64ULL << 10) << ((c - 5) / 4)) * (5 + (c - 5) % 4) / 4;
}

// The smallest class of at least `bytes`; kClasses past the largest.
uint32_t classUp(uint64_t bytes)
{
	uint32_t c = 0;
	while (c < kClasses && classBytes(c) < bytes)
		c++;
	return c;
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

// The class a spare of `bytes` serves: the largest class not past it.
uint32_t spareClass(uint64_t bytes)
{
	const uint32_t c = classUp(bytes);
	return c < kClasses && classBytes(c) == bytes ? c : c - 1;
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

// The page list of a wired kernel buffer.
bool kernelPages(const void *va, uint64_t bytes, uint64_t *pas)
{
	for (uint64_t off = 0; off < bytes; off += 4096) {
		const uint64_t pa = ml_vtophys(reinterpret_cast<vm_offset_t>(va) + off);
		if (!pa || (pa & 0xfff) || pa + 4096 > (1ULL << 34))
			return false;
		pas[off >> 12] = pa;
	}
	return true;
}

// A wired, zeroed, clean cached BO of `bytes`, of 128 KiB units where its layout has them (each one GMMU big page), or
// nullptr. The refill thread alone makes these: BoAlloc's own path on the allocating thread never searches for units.
Spare *spareMake(SparePool *p, uint64_t bytes)
{
	Spare *s = IONew(Spare, 1);
	uint64_t *pas = IONew(uint64_t, bytes >> 12);
	SwitchGpuCachedBuffer buffer = {};
	bool made = switch_gpu_cached_buffer_units(bytes, &buffer);
	if (!made && switch_gpu_spare_layout(bytes).units) {
#if TGM_STATS
		IOLockLock(p->lock);
		++p->notUnits;
		IOLockUnlock(p->lock);
#else
		(void)p;
#endif
	}
	made = made || switch_gpu_cached_buffer_pages(bytes, &buffer);
	if (s && pas && made) {
		// KMA_ZERO zero-filled it through the cacheable aperture: clean the zeros out
		cleanZeros(buffer.cpu, bytes);
		if (kernelPages(buffer.cpu, bytes, pas)) {
			*s = {nullptr, buffer, pas, bytes, nullptr};
			return s;
		}
	}
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
	const uint32_t c = spareClass(s->bytes);
	IOLockLock(p->lock);
	--p->zeroing[c];
	p->cleaningBytes -= s->bytes;
	const bool keep = !result && spareRoom();
	if (keep) {
		s->next = p->ready[c];
		p->ready[c] = s;
		++p->count[c];
	} else {
		p->clean -= s->bytes;
	}
	IOLockUnlock(p->lock);
	if (!keep)
		spareFree(s);
	thread_call_enter(p->call);
}

void spareWork(thread_call_param_t param, thread_call_param_t)
{
	SparePool *p = static_cast<SparePool *>(param);
	for (;;) {
		IOLockLock(p->lock);
		if (PoolZero *z = p->poolDirty) {  // first: pool pages are the scarcer
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
		if (Spare *s = p->dirty) {
			p->dirty = s->next;
			p->dirtyBytes -= s->bytes;
			const bool keep =
				!pressure && spareRoom() && p->clean + s->bytes <= kSpareCapBytes;
			if (keep) {
				p->clean += s->bytes;
				p->cleaningBytes += s->bytes;
				++p->zeroing[spareClass(s->bytes)];
			}
			IOLockUnlock(p->lock);
			if (!keep) {
				spareFree(
					s);  // past the cap or under pressure: back to the VM, unzeroed
				continue;
			}
			const int result =
				switch_gpu_cached_zero(s->pas, uint32_t(s->bytes >> 12),
						       s->buffer.cpu, &s->mapping, spareZeroed, s);
			if (result == -16) {
				// every scrub slot busy: a retiring scrub's completion runs this again
				IOLockLock(p->lock);
#if TGM_STATS
				++p->zeroBusy;
#endif
				--p->zeroing[spareClass(s->bytes)];
				p->cleaningBytes -= s->bytes;
				p->clean -= s->bytes;
				s->next = p->dirty;
				p->dirty = s;
				p->dirtyBytes += s->bytes;
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
		if (pressure) {  // give every spare back; refill nothing while it lasts
			Spare *all = nullptr;
			for (uint32_t c = 0; c < kClasses; c++) {
				while (Spare *s = p->ready[c]) {
					p->ready[c] = s->next;
					s->next = all;
					all = s;
				}
				p->count[c] = 0;
			}
			p->clean = p->cleaningBytes;
			IOLockUnlock(p->lock);
			while (all) {
				Spare *next = all->next;
				spareFree(all);
				all = next;
			}
			return;
		}
		Spare *evicted = nullptr;
		uint32_t c = 0;
		for (; c < kClasses; c++) {
			if (p->count[c] + p->zeroing[c] >= p->target[c])
				continue;
			uint64_t over = 0;
			for (uint32_t d = 0; d < kClasses; d++)
				if (p->count[d] > p->target[d])
					over += uint64_t(p->count[d] - p->target[d]) *
						classBytes(d);
			if (!switch_gpu_spare_fits_after(p->clean, over, classBytes(c),
							 kSpareCapBytes))
				continue;
			while (p->clean + classBytes(c) > kSpareCapBytes) {
				const uint32_t d =
					switch_gpu_spare_over_depth(p->count, p->target, kClasses);
				if (d == kClasses || !p->ready[d])
					break;
				Spare *s = p->ready[d];
				p->ready[d] = s->next;
				p->count[d]--;
				p->clean -= s->bytes;
				s->next = evicted;
				evicted = s;
			}
			if (p->clean + classBytes(c) <= kSpareCapBytes)
				break;
		}
		if (c < kClasses)
			p->clean += classBytes(c);
		IOLockUnlock(p->lock);
		while (evicted) {
			Spare *next = evicted->next;
			spareFree(evicted);
			evicted = next;
		}
		if (c == kClasses)
			return;
		Spare *s = spareMake(p, classBytes(c));
		IOLockLock(p->lock);
		if (!s) {  // the VM could not give it: try again at the next event, not now
			p->clean -= classBytes(c);
#if TGM_STATS
			++p->refillRefused;
#endif
			IOLockUnlock(p->lock);
			return;
		}
		s->next = p->ready[c];
		p->ready[c] = s;
		p->count[c]++;
		IOLockUnlock(p->lock);
	}
}
}  // namespace

namespace tgm_uc
{

SparePool *spares()
{
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	if (p)
		return p;
	SparePool *n = IONew(SparePool, 1);
	if (!n)
		return nullptr;
	bzero(n, sizeof(*n));
	for (const WarmClass &w : kWarm)
		n->target[classUp(w.bytes)] = w.ready;
	n->lock = IOLockAlloc();
	n->call =
		n->lock ? thread_call_allocate_with_options(spareWork, n, THREAD_CALL_PRIORITY_USER,
							    THREAD_CALL_OPTIONS_ONCE)
			: nullptr;
	if (!n->call) {
		if (n->lock)
			IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return nullptr;
	}
	if (!__atomic_compare_exchange_n(&gSpares, &p, n, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		thread_call_free(n->call);
		IOLockFree(n->lock);
		IODelete(n, SparePool, 1);
		return p;
	}
	thread_call_enter(n->call);  // the warm classes, made before anyone waits for them
	return n;
}

// A ready spare for a cached BO of `bytes`: of its class, or of a larger one up to about twice its size; nullptr when
// none is ready, which marks the class to keep warm. Either way a class below its mark wakes the thread call.
Spare *spareTake(uint64_t bytes)
{
	SparePool *p = spares();
	const uint32_t c = classUp(bytes);
	if (!p || c >= kClasses)
		return nullptr;
	const uint32_t last = c + (c < 5 ? 1 : 4);
	IOLockLock(p->lock);
	Spare *s = nullptr;
	for (uint32_t d = c; d < kClasses && d <= last && !s; d++) {
		s = p->ready[d];
		if (s) {
			p->ready[d] = s->next;
			p->count[d]--;
			p->clean -= s->bytes;
		}
	}
	if (!s) {
		const switch_gpu_spare_miss why =
			switch_gpu_spare_miss_of(p->target[c], p->zeroing[c], memoryPressure());
		const uint16_t learnt = switch_gpu_spare_depth_after_miss(
			p->target[c], classBytes(c) <= kLearnMaxBytes, why);
		const uint16_t most = switch_gpu_spare_depth_cap(classBytes(c), kSpareCapBytes);
		p->target[c] = learnt < most ? learnt : most;
#if TGM_STATS
		++p->misses[why];
#endif
	}
	const bool kick =
		memoryPressure() ? p->clean != 0 : spareRoom() && p->count[c] < p->target[c];
	IOLockUnlock(p->lock);
	if (kick)
		thread_call_enter(p->call);
	return s;
}

// Debug kernels: a cached BoAlloc that no spare can serve by its flags (contiguous, write-combined).
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

// A freed cached BO (its prepared buffer, which nothing else holds, with its `bytes` and page list) to the spares:
// true if taken, then the caller must not touch the buffer, pas or its persistent mapping again.
bool spareKeep(const SwitchGpuCachedBuffer &buffer, uint64_t *pas, uint64_t bytes,
	       switch_gpu_cached_mapping *mapping)
{
	SparePool *p = spares();
	Spare *s = p && switch_gpu_cached_buffer_alone(buffer) ? IONew(Spare, 1) : nullptr;
	if (!s)
		return false;
	*s = {nullptr, buffer, pas, bytes, mapping};
	IOLockLock(p->lock);
	s->next = p->dirty;
	p->dirty = s;
	p->dirtyBytes += bytes;
	IOLockUnlock(p->lock);
	thread_call_enter(p->call);
	return true;
}
}  // namespace tgm_uc

void switch_gpu_cached_retire(SwitchGpuCachedBuffer buffer, uint64_t *pages, uint32_t count,
			      switch_gpu_cached_mapping *mapping)
{
	if (buffer.cpu && spareKeep(buffer, pages, uint64_t(count) << 12, mapping))
		return;
	switch_gpu_cached_mapping_drop(&mapping);
	switch_gpu_cached_buffer_free(&buffer);
	IODelete(pages, uint64_t, count);
}

extern "C" void tgm_spare_counts(uint64_t *kept_bytes, uint64_t *ready,
				 uint64_t *pool_zeroing_bytes)
{
	const SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	uint64_t n = 0;
	if (p) {
		IOLockLock(p->lock);
		for (const uint16_t count : p->count)
			n += count;
		*kept_bytes = p->clean + p->dirtyBytes;
		*pool_zeroing_bytes = p->poolZeroing;
		IOLockUnlock(p->lock);
	} else {
		*kept_bytes = *pool_zeroing_bytes = 0;
	}
	*ready = n;
}

// The window's BoAllocs by process ("name n (KiB, mean/max us, made m)"), then cleared; 0 when there were none.
extern "C" unsigned long tgm_owner_stats(char *out, unsigned long size)
{
#if TGM_STATS
	SparePool *p = __atomic_load_n(&gSpares, __ATOMIC_ACQUIRE);
	unsigned long at = 0;
	if (!TGM_STATS || !p || !size)
		return 0;
	out[0] = 0;
	IOLockLock(p->lock);
	for (uint32_t i = 0; i < kOwners && p->owners[i].n; i++) {
		const OwnerStat &o = p->owners[i];
		const int n = snprintf(out + at, size - at,
				       "%s%s %llu (%llu KiB, %llu/%llu us, made %llu)",
				       at ? "; " : "", o.name, o.n, o.bytes >> 10,
				       o.ns / o.n / 1000, o.max_ns / 1000, o.made);
		if (n > 0)
			at = at + size_t(n) < size ? at + size_t(n) : size - 1;
	}
	uint32_t missed = p->zeroBusy + p->zeroFailed + p->refillRefused + p->notUnits;
	for (const uint32_t m : p->misses)
		missed += m;
	const int n =
		missed ? snprintf(out + at, size - at,
				  "%sspare misses: first %u, zeroing %u, drained %u, pressure %u, "
				  "ineligible %u; copy-engine zero busy %u, failed %u; refill "
				  "refused %u, not in 128 KiB units %u",
				  at ? "; " : "", p->misses[SWITCH_GPU_SPARE_FIRST],
				  p->misses[SWITCH_GPU_SPARE_ZEROING],
				  p->misses[SWITCH_GPU_SPARE_DRAINED],
				  p->misses[SWITCH_GPU_SPARE_PRESSURE],
				  p->misses[SWITCH_GPU_SPARE_INELIGIBLE], p->zeroBusy,
				  p->zeroFailed, p->refillRefused, p->notUnits)
		       : 0;
	if (n > 0)
		at = at + size_t(n) < size ? at + size_t(n) : size - 1;
	bzero(p->owners, sizeof(p->owners));
	bzero(p->misses, sizeof(p->misses));
	p->zeroBusy = p->zeroFailed = p->refillRefused = p->notUnits = 0;
	IOLockUnlock(p->lock);
	return at;
#else
	(void)out;
	(void)size;
	return 0;
#endif
}
