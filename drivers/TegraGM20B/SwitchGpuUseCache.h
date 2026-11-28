// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <string.h>
#include "SwitchGpuBacking.h"
#include "SwitchGpuSurfaceRefresh.h"

template <typename Memory> struct SwitchGpuUseCache {
	switch_gpu_backing_use *uses = nullptr;
	switch_gpu_backing_write_range *writes = nullptr;
	SwitchGpuSurfaceRefresh *surfaces = nullptr;
	tgm_coh **written = nullptr;
	uint32_t capacity = 0;
	uint64_t channelUid = 0;
	uint64_t bindGen = 0;
	uint64_t boGen = 0;
	uint32_t claim = 0;
	uint32_t set = 0;
	uint32_t useTotal = 0;
	uint32_t writeCount = 0;
	uint32_t surfaceCount = 0;
	uint32_t writtenCount = 0;
	uint32_t rangedUses = 0;
	uint32_t named =
		0;  // the shared uses the saved job named (TegraGM20BAbiJob.shared_count): a reuse's count
	bool unlockedWrites = false;
	bool hostWritten = false;
	bool valid = false;

	void invalidate()
	{
		valid = false;
	}

	void release()
	{
		Memory::release(uses, capacity);
		Memory::release(writes, capacity);
		Memory::release(surfaces, capacity);
		Memory::release(written, capacity);
		*this = {};
	}

	bool reserve(uint32_t needed)
	{
		if (needed <= capacity)
			return true;
		uint32_t cap = capacity ? capacity : 64u;
		while (cap < needed && cap <= UINT32_MAX / 2)
			cap *= 2;
		if (cap < needed)
			return false;
		auto *u = Memory::template allocate<switch_gpu_backing_use>(cap);
		auto *w = Memory::template allocate<switch_gpu_backing_write_range>(cap);
		auto *s = Memory::template allocate<SwitchGpuSurfaceRefresh>(cap);
		auto **c = Memory::template allocate<tgm_coh *>(cap);
		if (!u || !w || !s || !c) {
			Memory::release(u, cap);
			Memory::release(w, cap);
			Memory::release(s, cap);
			Memory::release(c, cap);
			return false;
		}
		Memory::release(uses, capacity);
		Memory::release(writes, capacity);
		Memory::release(surfaces, capacity);
		Memory::release(written, capacity);
		uses = u;
		writes = w;
		surfaces = s;
		written = c;
		capacity = cap;
		valid = false;
		return true;
	}

	bool matches(uint64_t uid, uint64_t bind, uint64_t bo, uint32_t claimFlags,
		     uint32_t setId) const
	{
		return valid && channelUid == uid && bindGen == bind && boGen == bo &&
		       claim == claimFlags && set == setId;
	}

	void save(uint64_t uid, uint64_t bind, uint64_t bo, uint32_t claimFlags, uint32_t setId,
		  const switch_gpu_backing_use *srcUses, uint32_t nUses,
		  const switch_gpu_backing_write_range *srcWrites, uint32_t nWrites,
		  const SwitchGpuSurfaceRefresh *srcSurfaces, uint32_t nSurfaces,
		  tgm_coh *const *srcWritten, uint32_t nWritten, uint32_t ranged, bool unlocked,
		  bool host)
	{
		uint32_t maxCount = nUses;
		if (nWrites > maxCount)
			maxCount = nWrites;
		if (nSurfaces > maxCount)
			maxCount = nSurfaces;
		if (nWritten > maxCount)
			maxCount = nWritten;
		if (maxCount && !reserve(maxCount)) {
			valid = false;
			return;
		}
		if (nUses)
			memcpy(uses, srcUses, nUses * sizeof(*uses));
		if (nWrites)
			memcpy(writes, srcWrites, nWrites * sizeof(*writes));
		if (nSurfaces)
			memcpy(surfaces, srcSurfaces, nSurfaces * sizeof(*surfaces));
		if (nWritten)
			memcpy(written, srcWritten, nWritten * sizeof(*written));
		channelUid = uid;
		bindGen = bind;
		boGen = bo;
		claim = claimFlags;
		set = setId;
		useTotal = nUses;
		writeCount = nWrites;
		surfaceCount = nSurfaces;
		writtenCount = nWritten;
		rangedUses = ranged;
		unlockedWrites = unlocked;
		hostWritten = host;
		valid = true;
	}

	void restore(switch_gpu_backing_use *dstUses, uint32_t &nUses,
		     switch_gpu_backing_write_range *dstWrites, uint32_t &nWrites,
		     SwitchGpuSurfaceRefresh *dstSurfaces, uint32_t &nSurfaces,
		     tgm_coh **dstWritten, uint32_t &nWritten, uint32_t &ranged, bool &unlocked,
		     bool &host) const
	{
		if (useTotal)
			memcpy(dstUses, uses, useTotal * sizeof(*uses));
		if (writeCount)
			memcpy(dstWrites, writes, writeCount * sizeof(*writes));
		if (surfaceCount)
			memcpy(dstSurfaces, surfaces, surfaceCount * sizeof(*surfaces));
		if (writtenCount)
			memcpy(dstWritten, written, writtenCount * sizeof(*written));
		nUses = useTotal;
		nWrites = writeCount;
		nSurfaces = surfaceCount;
		nWritten = writtenCount;
		ranged = rangedUses;
		unlocked = unlockedWrites;
		host |= hostWritten;
	}
};
