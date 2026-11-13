/* SPDX-License-Identifier: MIT */
/* A GR TRAP whose only cause is TEX exceptions: nvgpu (gr_gk20a_handle_tex_exception, through gk20a_gr_isr) reports the
 * TEX HWW ESR, writes it back to clear it and lets the channel go on, as it does for no other TPC exception. nouveau's
 * gf100_gr_intr reads the same registers as this snapshot and hands a TEX-only trap here before it would report a TRAP;
 * every other trap keeps its report, and so the kill or panic that follows it. The includer provides snprintf. */
#ifndef TEGRA_GM20B_TEX_TRAP_H
#define TEGRA_GM20B_TEX_TRAP_H

#include <stdint.h>

#define TGM_TRAP_GPCS 4
#define TGM_TRAP_TPCS 4
/* PGRAPH_TRAP (0x400108): a GPC's exception */
#define TGM_TRAP_GPC 0x01000000u
/* GPCCS_GPC_EXCEPTION (GPC 0x2c90): one bit per TPC from 16 */
#define TGM_GPC_EXCEPTION_TPCS 0x00ff0000u
/* TPCCS_TPC_EXCEPTION (TPC 0x508) */
#define TGM_TPC_EXCEPTION_TEX 0x1u
/* TEX_M_HWW_ESR (TPC 0x224): INTR in hw_gr_gm20b.h, RESET_ACTIVE in hw_gr_gp10b.h; EN reads set from reset (L4T, ours) */
#define TGM_TEX_ESR_INTR 0x1u
#define TGM_TEX_ESR_RESET_ACTIVE 0x40000000u
#define TGM_TEX_ESR_EN 0x80000000u

/* 0x400108, 0x400118, each flagged GPC's exception and each flagged TPC's */
struct tgm_gr_trap_snapshot {
	uint32_t trap;
	uint32_t gpcs;
	uint32_t gpc[TGM_TRAP_GPCS];
	uint32_t tpc[TGM_TRAP_GPCS][TGM_TRAP_TPCS];
};

static inline int tgm_gr_trap_tex_only(const struct tgm_gr_trap_snapshot *s, unsigned int gpc_nr,
				       const uint8_t *tpc_nr)
{
	unsigned int gpc = 0;
	unsigned int tpc = 0;

	if (s->trap != TGM_TRAP_GPC || !s->gpcs || gpc_nr > TGM_TRAP_GPCS || (s->gpcs >> gpc_nr))
		return 0;
	for (gpc = 0; gpc < gpc_nr; gpc++) {
		const uint32_t tpcs = (s->gpc[gpc] & TGM_GPC_EXCEPTION_TPCS) >> 16;

		if (!(s->gpcs & (1u << gpc)))
			continue;
		if (tpc_nr[gpc] > TGM_TRAP_TPCS || !tpcs ||
		    (s->gpc[gpc] & ~TGM_GPC_EXCEPTION_TPCS) || (tpcs >> tpc_nr[gpc]))
			return 0;
		for (tpc = 0; tpc < tpc_nr[gpc]; tpc++) {
			if ((tpcs & (1u << tpc)) && s->tpc[gpc][tpc] != TGM_TPC_EXCEPTION_TEX)
				return 0;
		}
	}
	return 1;
}

/* Only the fields NVIDIA's headers name; the rest of the word is printed whole. */
static inline void tgm_tex_esr_describe(uint32_t esr, char *buf, unsigned long len)
{
	const uint32_t named = TGM_TEX_ESR_EN | TGM_TEX_ESR_RESET_ACTIVE | TGM_TEX_ESR_INTR;

	snprintf(buf, len, "%s%s%s, other bits %#x", (esr & TGM_TEX_ESR_EN) ? "EN" : "-",
		 (esr & TGM_TEX_ESR_RESET_ACTIVE) ? " RESET_ACTIVE" : "",
		 (esr & TGM_TEX_ESR_INTR) ? " INTR" : "", esr & ~named);
}

/* Each channel's faults with one ESR are numbered, and the 1st, 10th, 100th... are logged: a fault on every draw costs a
 * line per decade, and the rate shows in the numbers. Channels beyond TGM_TEX_CHANNELS take the oldest slot. */
#define TGM_TEX_CHANNELS 8

struct tgm_tex_reports {
	struct {
		int chid;
		uint32_t esr;
		uint32_t count;
		int used;
	} ch[TGM_TEX_CHANNELS];

	unsigned int next;
};

/* 1 to log this fault; *count is its number for this channel and ESR */
static inline int tgm_tex_report_due(struct tgm_tex_reports *r, int chid, uint32_t esr,
				     uint32_t *count)
{
	unsigned int i = 0;
	uint32_t decade = 1;

	for (i = 0; i < TGM_TEX_CHANNELS && !(r->ch[i].used && r->ch[i].chid == chid); i++)
		;
	if (i == TGM_TEX_CHANNELS) {
		i = r->next;
		r->next = (r->next + 1) % TGM_TEX_CHANNELS;
		r->ch[i].used = 1;
		r->ch[i].chid = chid;
		r->ch[i].count = 0;
		r->ch[i].esr = esr;
	}
	if (r->ch[i].esr != esr) {
		r->ch[i].esr = esr;
		r->ch[i].count = 0;
	}
	*count = ++r->ch[i].count;
	while (decade < *count && decade <= UINT32_MAX / 10)
		decade *= 10;
	return decade == *count;
}

#endif /* TEGRA_GM20B_TEX_TRAP_H */
