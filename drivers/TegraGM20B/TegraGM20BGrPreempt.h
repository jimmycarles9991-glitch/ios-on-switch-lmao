/* SPDX-License-Identifier: MIT */
/* Quiesced write of GM20B's FECS ctxsw main image preemption_options word (offset 0x60, nvgpu
 * hw_ctxsw_prog_gm20b.h:463-472). While the channel is active, FECS holds its own copy of the
 * context image and saves it back over memory on context switch, so the write runs through
 * tgm_ramfc_write_quiesced after disabling and preempting the channel. */
#ifndef TEGRA_GM20B_GR_PREEMPT_H
#define TEGRA_GM20B_GR_PREEMPT_H

#include <stdint.h>
#include "TegraGM20BRamfcOrder.h"

#define TGM_GR_CTX_PREEMPT_OPTIONS 0x60u
#define TGM_GR_CTX_PREEMPT_WFI 0u
#define TGM_GR_CTX_PREEMPT_CTA 1u
#define TGM_GR_CTX_PREEMPT_MIN_SIZE (TGM_GR_CTX_PREEMPT_OPTIONS + 4u)

struct tgm_gr_preempt_io {
	void *arg;
	uint32_t (*read)(void *arg, uint32_t offset);
	void (*write)(void *arg, uint32_t offset, uint32_t value);
};

struct tgm_gr_preempt_ctx {
	const struct tgm_ramfc_steps *base;
	const struct tgm_gr_preempt_io *io;
	uint32_t mode;
	uint32_t old_mode;
	uint32_t readback;
};

static inline int tgm_gr_preempt_own(void *arg)
{
	const struct tgm_gr_preempt_ctx *ctx = (const struct tgm_gr_preempt_ctx *)arg;

	return ctx->base->own(ctx->base->arg);
}

static inline void tgm_gr_preempt_release(void *arg)
{
	const struct tgm_gr_preempt_ctx *ctx = (const struct tgm_gr_preempt_ctx *)arg;

	ctx->base->release(ctx->base->arg);
}

static inline void tgm_gr_preempt_disable(void *arg)
{
	const struct tgm_gr_preempt_ctx *ctx = (const struct tgm_gr_preempt_ctx *)arg;

	ctx->base->disable(ctx->base->arg);
}

static inline void tgm_gr_preempt_enable(void *arg)
{
	const struct tgm_gr_preempt_ctx *ctx = (const struct tgm_gr_preempt_ctx *)arg;

	ctx->base->enable(ctx->base->arg);
}

static inline int tgm_gr_preempt_preempt(void *arg)
{
	const struct tgm_gr_preempt_ctx *ctx = (const struct tgm_gr_preempt_ctx *)arg;

	return ctx->base->preempt(ctx->base->arg);
}

static inline void tgm_gr_preempt_write_cb(void *arg)
{
	struct tgm_gr_preempt_ctx *ctx = (struct tgm_gr_preempt_ctx *)arg;

	ctx->old_mode = ctx->io->read(ctx->io->arg, TGM_GR_CTX_PREEMPT_OPTIONS);
	ctx->io->write(ctx->io->arg, TGM_GR_CTX_PREEMPT_OPTIONS, ctx->mode);
	ctx->readback = ctx->io->read(ctx->io->arg, TGM_GR_CTX_PREEMPT_OPTIONS);
}

static inline int tgm_gr_preempt_write_quiesced(const struct tgm_ramfc_steps *base,
						const struct tgm_gr_preempt_io *io, uint32_t mode,
						uint32_t *old_mode, uint32_t *readback)
{
	struct tgm_gr_preempt_ctx ctx;
	struct tgm_ramfc_steps steps;
	int ret = 0;

	if (mode > TGM_GR_CTX_PREEMPT_CTA)
		return -EINVAL;
	ctx.base = base;
	ctx.io = io;
	ctx.mode = mode;
	ctx.old_mode = 0;
	ctx.readback = 0;
	steps.arg = &ctx;
	steps.own = tgm_gr_preempt_own;
	steps.release = tgm_gr_preempt_release;
	steps.disable = tgm_gr_preempt_disable;
	steps.enable = tgm_gr_preempt_enable;
	steps.preempt = tgm_gr_preempt_preempt;
	steps.write = tgm_gr_preempt_write_cb;
	ret = tgm_ramfc_write_quiesced(&steps);
	if (ret)
		return ret;
	if (old_mode)
		*old_mode = ctx.old_mode;
	if (readback)
		*readback = ctx.readback;
	return 0;
}

#endif
