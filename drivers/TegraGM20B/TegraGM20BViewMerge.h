/* SPDX-License-Identifier: MIT */
/* A block-linear IOSurface's CPU write lock when the GPU wrote the surface while it was held (TegraGM20BViews.cpp). On
 * Apple's GPUs the CPU and the GPU write the same memory: a lock does not hold the GPU back, and what each wrote stays
 * where the other did not write. Here the CPU writes rows in a scratch while the GPU writes the texels, so the unlock
 * lays every byte the CPU changed since the lock (rows against base, the texels as the lock found them, read back before
 * the GPU's first write under it) over the texels as the GPU left them (gpu, as rows); a byte both wrote keeps the
 * CPU's, and one the CPU wrote with the value it already held is the GPU's. */
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline void tgm_view_merge(uint64_t *gpu, const uint64_t *rows, const uint64_t *base,
				  size_t words)
{
	for (size_t i = 0; i < words; i++) {
		uint64_t changed = rows[i] ^ base[i];

		changed |= changed >> 4;
		changed |= changed >> 2;
		changed |= changed >> 1;
		const uint64_t bytes = (changed & 0x0101010101010101ull) * 0xffu;
		gpu[i] = (rows[i] & bytes) | (gpu[i] & ~bytes);
	}
}
