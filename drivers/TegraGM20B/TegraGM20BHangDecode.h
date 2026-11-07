/* SPDX-License-Identifier: MIT */
/* What a GPU hang's line (gpu.c chan_describe) says beyond the registers: where GR's context switch is, and whether the
 * hung submission turned on the pixel interlock, whose ticket wait is a loop in the shader itself. */
#ifndef TEGRA_GM20B_HANG_DECODE_H
#define TEGRA_GM20B_HANG_DECODE_H
#include <stdint.h>
#include "TegraGM20BGpu.h"

#define TGM_HANG_SUBS 64   /* submissions remembered, by seqno */
#define TGM_HANG_PUSHES 64 /* a hung submission's pushes read */
#define TGM_HANG_LINE 512  /* a channel's line with the decode */

/* Each recent submission's own GPFIFO entries [first, end), by seqno (one 8-byte store a submission) */
struct tgm_hang_log {
	struct {
		uint32_t seqno;
		uint16_t first, end;
	} sub[TGM_HANG_SUBS];
};

static inline void tgm_hang_log_submit(struct tgm_hang_log *log, uint32_t seqno, uint32_t first,
				       uint32_t end)
{
	log->sub[seqno % TGM_HANG_SUBS].seqno = seqno;
	log->sub[seqno % TGM_HANG_SUBS].first = (uint16_t)first;
	log->sub[seqno % TGM_HANG_SUBS].end = (uint16_t)end;
}

/* PFIFO_ENGINE_STATUS (0x2640 + 8 engine): context id 11:0 (a TSG's when 12), ctx_status 15:13, next id 27:16 (a TSG's
 * when 28). "ch 3 -> ch 26, ctxsw switch": GR still on channel 3's context, switching to channel 26's. */
static inline void tgm_hang_engine(uint32_t eng, char *buf, unsigned long len)
{
	static const char *const status[8] = {"invalid", "valid", "2",    "3",
					      "4",       "load",  "save", "switch"};

	snprintf(buf, len, "%s %u -> %s %u, ctxsw %s", (eng & (1u << 12)) ? "tsg" : "ch",
		 eng & 0xfff, (eng & (1u << 28)) ? "tsg" : "ch", (eng >> 16) & 0xfff,
		 status[(eng >> 13) & 7]);
}

/* The hung submission (`hung`, the first one not completed; none when it is past `submitted`): its interlocked states
 * (SET_PIXEL_SHADER_INTERLOCK_CONTROL with conflict detection on), its pushes read through their connection
 * (tgm_client_interlocks) from the GPFIFO entries the log names. Entries in [kernel_va, kernel_va + kernel_bytes) are
 * the kernel's own and skipped. Empty when it has none and every push was read. */
static inline void tgm_hang_note(const struct tgm_hang_log *log, uint32_t hung, uint32_t submitted,
				 const volatile uint32_t *gpfifo, uint32_t gpmask,
				 uint64_t kernel_va, uint64_t kernel_bytes, const void *client,
				 char *buf, unsigned long len)
{
	uint64_t va[TGM_HANG_PUSHES], interlocks = 0;
	uint32_t dwords[TGM_HANG_PUSHES], n = 0, i;
	int read;

	buf[0] = 0;
	if (!client || !gpfifo || (int32_t)(submitted - hung) < 0 ||
	    log->sub[hung % TGM_HANG_SUBS].seqno != hung)
		return;
	for (i = log->sub[hung % TGM_HANG_SUBS].first;
	     i != log->sub[hung % TGM_HANG_SUBS].end && n < TGM_HANG_PUSHES; i = (i + 1) & gpmask) {
		const uint64_t at = gpfifo[2 * i] | (uint64_t)(gpfifo[2 * i + 1] & 0xff) << 32;
		if (at >= kernel_va && at < kernel_va + kernel_bytes)
			continue;
		va[n] = at;
		dwords[n++] = (gpfifo[2 * i + 1] >> 10) & 0x1fffff;
	}
	read = tgm_client_interlocks(client, va, dwords, n, &interlocks);
	if (read < 0)
		snprintf(buf, len, " (hung submit %u not read: its connection is busy)", hung);
	else if (interlocks)
		snprintf(buf, len,
			 " (interlock ticket wait: %llu interlocked states in the hung submit %u,"
			 " %d/%u pushes read)",
			 (unsigned long long)interlocks, hung, read, n);
	else if ((uint32_t)read < n)
		snprintf(buf, len, " (hung submit %u: %d of %u pushes read, no interlocked state)",
			 hung, read, n);
}
#endif
