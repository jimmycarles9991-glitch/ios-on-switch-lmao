/* SPDX-License-Identifier: MIT */
/* GPU PMC_ENABLE (0x000200), one reset bit per engine (nvkm's mc reset maps), has no set/clear alias on GM20B, and its
 * changes come from different contexts: an engine's first use and last (nvkm_subdev_init/fini under that subdev's
 * use.mutex, reached from chan_new under tgm_gpu.lock), a recovery's engine reset (tgm_engine_reset_locked under its
 * runlist's mutex), the PMU's and GR's own resets, the mc subdev's init, the PRI ring's reset (privring/gk20a.c, at its
 * init and from nvkm's interrupt pass on a ring error). Two read-modify-writes that interleave lose one engine's bit,
 * leaving it in reset (or out of it) until its next init. Every change takes one spinlock (gpu_pmc.c), as nvgpu's
 * mc_enable_lock does (common/mc/mc_gm20b.c:223-241): these are setup, teardown, recovery and error steps, never per
 * frame, and their contexts are nvkm's own, so no single thread owns them all. test/gm20b_pmc_enable_test.cpp. */
#ifndef TEGRA_GM20B_PMC_ENABLE_H
#define TEGRA_GM20B_PMC_ENABLE_H

#include <stdint.h>

struct tgm_pmc_enable_io {
	void *ctx;
	uint32_t (*read)(void *ctx);
	void (*write)(void *ctx, uint32_t value);
	unsigned long (*lock)(void *ctx);
	void (*unlock)(void *ctx, unsigned long flags);
};

/* PMC_ENABLE's `mask` bits to `value`'s, the write read back (posted) before the lock goes. */
static inline void tgm_pmc_enable_change(const struct tgm_pmc_enable_io *io, uint32_t mask,
					 uint32_t value)
{
	const unsigned long flags = io->lock(io->ctx);

	io->write(io->ctx, (io->read(io->ctx) & ~mask) | (value & mask));
	(void)io->read(io->ctx);
	io->unlock(io->ctx, flags);
}

#endif
