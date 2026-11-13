/*
 * The PBDMA's semaphore acquire timeout (gpu_acquire.c), as nvgpu programs it into RAMFC ACQUIRE and answers its
 * PBDMA_INTR_0 ACQUIRE interrupt, and the acquire a channel is parked on, as nvgpu reads it from RAMFC
 * (gk20a_dump_channel_status_ramfc). Nouveau writes ACQUIRE 0xfffff902 (fifo/gk104.c gk104_chan_ramfc_write): the
 * timeout's mantissa and exponent at their maximum, about 37 minutes, so it never fires.
 *
 * tgm_acquire_word is gk20a_fifo_pbdma_acquire_val (switch-l4t-kernel-nvgpu 1ae0167d, gk20a/fifo_gk20a.c), the
 * field layout hw_pbdma_gm20b.h's and the RAMFC words hw_ram_gm20b.h's:
 *
 * Copyright (c) 2011-2020, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#ifndef TEGRA_GM20B_ACQUIRE_H
#define TEGRA_GM20B_ACQUIRE_H
#include <stdint.h>

#define TGM_PBDMA_ACQUIRE(i) (0x040030 + (i) * 0x2000)
#define TGM_PBDMA_INTR_ACQUIRE 0x04000000u
#define TGM_ACQUIRE_RETRY 0x00000102u /* retry_man 2, retry_exp 2 */
#define TGM_ACQUIRE_TIMEOUT_EN 0x80000000u

/* RAMFC byte offsets; the PBDMA register a word saves and restores is 0x040000 plus the same offset */
#define TGM_RAMFC_ACQUIRE 0x30
#define TGM_RAMFC_SEMAPHOREA 0x38
#define TGM_RAMFC_SYNCPOINTA 0xa4
#define TGM_RAMFC_SYNCPOINTB 0xa8

/* NV906F host methods at PBDMA METHOD0's address (13:2, in bytes) */
#define TGM_HOST_SEMAPHORED 0x001c
#define TGM_HOST_SYNCPOINTB 0x0074

/* The PBDMA counts the timeout in units of 1024 PTIMER nanoseconds, and PTIMER runs at 19.2/31.25 of real time on this
 * Tegra (recover.c ENG_TIMEOUT). */
static inline uint64_t tgm_acquire_ptimer_ns(uint64_t real_ns)
{
	return real_ns * 1920u / 3125u;
}

static inline uint32_t tgm_acquire_fls64(uint64_t v)
{
	uint32_t n = 0;

	while (v) {
		n++;
		v >>= 1;
	}
	return n;
}

static inline uint32_t tgm_acquire_word(uint64_t ptimer_ns)
{
	uint64_t units = ptimer_ns / 1024u;
	uint32_t val = TGM_ACQUIRE_RETRY;
	uint32_t len = tgm_acquire_fls64(units);
	uint32_t exp = 0;
	uint32_t man = 0;

	if (!ptimer_ns)
		return val;
	if (len > 16u + 15u) {
		exp = 15u;
		man = 0xffffu;
	} else if (len > 16u) {
		exp = len - 16u;
		man = (uint32_t)(units >> exp);
	} else {
		man = (uint32_t)units;
	}
	return val | (exp & 0xfu) << 11 | (man & 0xffffu) << 15 | TGM_ACQUIRE_TIMEOUT_EN;
}

/* The PTIMER nanoseconds an ACQUIRE word waits before it interrupts; 0 when its timeout is off. */
static inline uint64_t tgm_acquire_timeout(uint32_t word)
{
	if (!(word & TGM_ACQUIRE_TIMEOUT_EN))
		return 0;
	return ((uint64_t)((word >> 15) & 0xffffu) << ((word >> 11) & 0xfu)) * 1024u;
}

/* CCSR_CHANNEL status 27:24 (gk20a_decode_ccsr_chan_status): off the PBDMA because its acquire has not passed
 * (pending_acquire, pending_acq_ctx_reload, on_eng_pending_acquire, on_eng_pending_acq_ctx_reload). */
#define TGM_CCSR_CHANNEL(chid) (0x800004 + (chid) * 8)

static inline int tgm_ccsr_acquiring(uint32_t ccsr)
{
	const uint32_t status = (ccsr >> 24) & 0xfu;

	return status == 3 || status == 4 || status == 8 || status == 14;
}

/* Debug kernels' ACQUIRE probe (gpu_acquire.c): where a channel is that stayed parked on an acquire TGM_ACQUIRE_NS with
 * no ACQUIRE interrupt. Switched out (CCSR pending_acquire), the PBDMA was not timing the acquire. On PBDMA0 at the
 * acquire with its timeout armed, the PBDMA timed it, and its interrupt was lost. */
enum tgm_acquire_where {
	TGM_ACQUIRE_SWITCHED_OUT,
	TGM_ACQUIRE_LOST,
	TGM_ACQUIRE_UNTIMED,   /* on PBDMA0 at the acquire with its timeout off */
	TGM_ACQUIRE_ELSEWHERE, /* neither on PBDMA0 at an acquire nor switched out on one */
};

/* pbdma_chid: PBDMA0's channel; at_acquire: PBDMA0 at a semaphore acquire that has not passed (tgm_pbdma_acquiring); acquire:
 * PBDMA0's ACQUIRE word */
static inline enum tgm_acquire_where tgm_acquire_probe_where(uint32_t chid, uint32_t ccsr,
							     uint32_t pbdma_chid, int at_acquire,
							     uint32_t acquire)
{
	if (pbdma_chid == chid && at_acquire)
		return acquire & TGM_ACQUIRE_TIMEOUT_EN ? TGM_ACQUIRE_LOST : TGM_ACQUIRE_UNTIMED;
	return tgm_ccsr_acquiring(ccsr) ? TGM_ACQUIRE_SWITCHED_OUT : TGM_ACQUIRE_ELSEWHERE;
}

enum tgm_acquire_kind {
	TGM_ACQUIRE_NONE,
	TGM_ACQUIRE_SEMAPHORE, /* NV906F SEMAPHORED with ACQUIRE, ACQ_GEQ or ACQ_AND */
	TGM_ACQUIRE_SYNCPOINT, /* SYNCPOINTB operation WAIT (a host1x engine's syncpoint) */
};

struct tgm_acquire_wait {
	uint8_t kind;
	uint8_t operation; /* SEMAPHORED 3:0 */
	uint32_t payload;  /* SEMAPHOREC, or SYNCPOINTA's threshold */
	uint64_t va;       /* SEMAPHOREA 7:0 (address 39:32) and SEMAPHOREB 31:2 */
	uint32_t syncpoint;
};

/* The semaphore operation of RAMFC SEMAPHOREA..D (sem) when it is an acquire; else the syncpoint wait of SYNCPOINTA/B
 * when `parked` (CCSR says the channel waits on an acquire). Words a channel left from an earlier acquire decode the
 * same: the caller checks that the condition still fails. SYNCPOINTB 0 is a channel that never used a syncpoint. */
static inline struct tgm_acquire_wait tgm_acquire_decode(const uint32_t sem[4], uint32_t syncpointa,
							 uint32_t syncpointb, int parked)
{
	struct tgm_acquire_wait w = {0, 0, 0, 0, 0};
	const uint32_t operation = sem[3] & 0xfu;

	if (operation == 0x1 || operation == 0x4 || operation == 0x8) {
		w.kind = TGM_ACQUIRE_SEMAPHORE;
		w.operation = (uint8_t)operation;
		w.va = (uint64_t)(sem[0] & 0xffu) << 32 | (sem[1] & ~3u);
		w.payload = sem[2];
	} else if (parked && syncpointb && !(syncpointb & 0x3u)) {
		w.kind = TGM_ACQUIRE_SYNCPOINT;
		w.syncpoint = (syncpointb >> 8) & 0xffu;
		w.payload = syncpointa;
	}
	return w;
}

/* Whether an acquire with this operation still waits on a word at `value` */
static inline int tgm_acquire_blocked(uint8_t operation, uint32_t value, uint32_t payload)
{
	switch (operation) {
	case 0x1:
		return value != payload;
	case 0x4:
		return (int32_t)(value - payload) < 0;
	case 0x8:
		return !(value & payload);
	default:
		return 0;
	}
}

/* The offset of `va` in a mapping [base, base + bytes), or -1 outside it */
static inline int64_t tgm_acquire_offset(uint64_t va, uint64_t base, uint64_t bytes)
{
	return base && va >= base && va - base < bytes ? (int64_t)(va - base) : -1;
}
#endif
