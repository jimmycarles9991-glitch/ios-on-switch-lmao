// SPDX-License-Identifier: MIT
// The spare pool's counts for the kernel's stats lines.
#include "TegraGM20BUserClientInternal.h"

using namespace tgm_uc;

extern "C" void tgm_spare_counts(uint64_t *kept_bytes, uint64_t *ready,
				 uint64_t *pool_zeroing_bytes)
{
	const SparePool *p = sparesExisting();
	uint64_t n = 0;
	if (p) {
		IOLockLock(p->lock);
		for (const uint16_t ready : p->books.ready)
			n += ready;
		*kept_bytes = p->books.held;
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
	SparePool *p = sparesExisting();
	unsigned long at = 0;
	if (!TGM_STATS || !p || !size)
		return 0;
	out[0] = 0;
	uint64_t parked = 0, unparkResident = 0, unparkReclaimed = 0, reclaimedCached = 0,
		 reclaimedSystem = 0;
	parkedStatsTake(&parked, &unparkResident, &unparkReclaimed, &reclaimedCached,
			&reclaimedSystem);
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
		missed ? snprintf(
				 out + at, size - at,
				 "%sspare misses: first %u, zeroing %u, drained %u, pressure %u, "
				 "ineligible %u, reserved %u; copy-engine zero busy %u, failed %u; "
				 "refill refused %u, not in 128 KiB units %u",
				 at ? "; " : "", p->misses[SWITCH_GPU_SPARE_FIRST],
				 p->misses[SWITCH_GPU_SPARE_ZEROING],
				 p->misses[SWITCH_GPU_SPARE_DRAINED],
				 p->misses[SWITCH_GPU_SPARE_PRESSURE],
				 p->misses[SWITCH_GPU_SPARE_INELIGIBLE],
				 p->misses[SWITCH_GPU_SPARE_RESERVED], p->zeroBusy, p->zeroFailed,
				 p->refillRefused, p->notUnits)
		       : 0;
	if (n > 0)
		at = at + size_t(n) < size ? at + size_t(n) : size - 1;
	uint64_t wcSpares = 0, wcBytes = 0;
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++) {
		const uint32_t slot = switch_gpu_spare_slot(SWITCH_GPU_SPARE_WC, c);
		const uint64_t held = uint64_t(p->books.ready[slot]) + p->books.zeroing[slot] +
				      p->books.dirty[slot];
		wcSpares += held;
		wcBytes += held * switch_gpu_spare_slot_bytes(slot);
	}
	if (wcSpares) {
		const int wn =
			snprintf(out + at, size - at, "%swrite-combined spares %llu KiB in %llu",
				 at ? "; " : "", wcBytes >> 10, wcSpares);
		if (wn > 0)
			at = at + size_t(wn) < size ? at + size_t(wn) : size - 1;
	}
	if (parked || unparkResident || unparkReclaimed || reclaimedCached || reclaimedSystem) {
		const int pn = snprintf(out + at, size - at,
					"%sparked %llu (res %llu, rec %llu, cached %llu, sys %llu)",
					at ? "; " : "", parked, unparkResident, unparkReclaimed,
					reclaimedCached, reclaimedSystem);
		if (pn > 0)
			at = at + size_t(pn) < size ? at + size_t(pn) : size - 1;
	}
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
