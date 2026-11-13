/* SPDX-License-Identifier: MIT */
/* Debug kernels: a connection's last VmBind maps and unmaps, for the line a channel killed by an
 * MMU fault gets (TegraGM20BUserClient.cpp getStatus). A PTE fault names only an address; this
 * says whether the address space had a page there lately, what was bound there, what removed it
 * and how long before the kill, and whether the failed job was already submitted when it went
 * (memory freed under queued work) or only after (work recorded against a stale address). The
 * includer provides snprintf. */
#ifndef TEGRA_GM20B_BIND_HISTORY_H
#define TEGRA_GM20B_BIND_HISTORY_H
#include <stdint.h>

enum tgm_bind_op {
	TGM_BIND_MAP,
	TGM_BIND_UNMAP,    /* a VmBind Unmap */
	TGM_BIND_REPLACED, /* a VmBind Map over it */
	TGM_BIND_FREED,    /* a BoFree of its BO */
	TGM_BIND_REFUSED,  /* a VmBind Map the kernel refused: nothing is bound there */
	TGM_BIND_GONE, /* the PTEs of a retired binding removed, once the work that could use them completed */
};

enum tgm_bind_origin {
	TGM_BIND_ALLOC,
	TGM_BIND_POOL,
	TGM_BIND_COMPRESSIBLE,
	TGM_BIND_IMPORT,
};

#define TGM_BIND_CHANNELS 8
#define TGM_BIND_NOTES 512u
/* a fault this far past either end of a note's range is near it: a write past a binding's end */
#define TGM_BIND_SLACK 0x10000ull

struct tgm_bind_note {
	uint64_t va, range, handle, bo_bytes, bo_offset, ns;
	uint32_t surface;                      /* the BO's IOSurface, 0: none */
	uint32_t submitted[TGM_BIND_CHANNELS]; /* each channel slot's submitted seqno then */
	uint8_t op;                            /* tgm_bind_op */
	uint8_t origin;                        /* tgm_bind_origin */
	uint8_t kind;
};

struct tgm_bind_history {
	struct tgm_bind_note notes[TGM_BIND_NOTES];
	uint32_t written;
};

static inline void tgm_bind_note_add(struct tgm_bind_history *h, const struct tgm_bind_note *n)
{
	h->notes[h->written++ % TGM_BIND_NOTES] = *n;
}

static inline int tgm_bind_note_near(const struct tgm_bind_note *n, uint64_t va, uint64_t slack)
{
	const uint64_t from = n->va > slack ? n->va - slack : 0;
	const uint64_t to = n->va + n->range + slack;

	return va >= from && (to < n->va || va < to);
}

/* The notes near va, newest first, at most `capacity` of them (indices into h->notes); how many */
static inline uint32_t tgm_bind_history_near(const struct tgm_bind_history *h, uint64_t va,
					     uint64_t slack, uint32_t *out, uint32_t capacity)
{
	const uint32_t kept = h->written < TGM_BIND_NOTES ? h->written : TGM_BIND_NOTES;
	uint32_t n = 0;

	for (uint32_t back = 1; back <= kept && n < capacity; back++) {
		const uint32_t at = (h->written - back) % TGM_BIND_NOTES;

		if (tgm_bind_note_near(&h->notes[at], va, slack))
			out[n++] = at;
	}
	return n;
}

/* How far back the notes kept go, from now_ns: a VA no note is near has had no map or unmap in that
 * time, or had them before it. */
static inline uint64_t tgm_bind_history_span_ns(const struct tgm_bind_history *h, uint64_t now_ns)
{
	if (!h->written)
		return 0;
	const uint32_t oldest = h->written < TGM_BIND_NOTES ? 0 : h->written % TGM_BIND_NOTES;

	return now_ns - h->notes[oldest].ns;
}

static inline const char *tgm_bind_op_name(uint8_t op)
{
	switch (op) {
	case TGM_BIND_MAP:
		return "mapped";
	case TGM_BIND_UNMAP:
		return "unmapped";
	case TGM_BIND_REPLACED:
		return "replaced by a map";
	case TGM_BIND_FREED:
		return "unmapped by its BoFree";
	case TGM_BIND_REFUSED:
		return "map refused";
	case TGM_BIND_GONE:
		return "PTEs removed (retired)";
	default:
		return "?";
	}
}

static inline const char *tgm_bind_origin_name(uint8_t origin)
{
	switch (origin) {
	case TGM_BIND_ALLOC:
		return "alloc";
	case TGM_BIND_POOL:
		return "pool alloc";
	case TGM_BIND_COMPRESSIBLE:
		return "compressible alloc";
	case TGM_BIND_IMPORT:
		return "import";
	default:
		return "?";
	}
}

/* Where the failed job (seqno `failed` of channel slot `slot`, 0: unknown) stood at the note:
 * already submitted (the note came while it was queued or running) or not yet. */
static inline const char *tgm_bind_note_job(const struct tgm_bind_note *n, uint32_t slot,
					    uint32_t failed)
{
	if (slot >= TGM_BIND_CHANNELS || !failed || !n->submitted[slot])
		return "";
	return (int32_t)(n->submitted[slot] - failed) >= 0 ? ", the failed job submitted"
							   : ", before the failed job";
}

/* "12 ms before the kill: unmapped by its BoFree 0x3ef3e80000+0x400000 of BO 1234 (4194304 bytes,
 * alloc at 0x0), kind 0xfe, the failed job submitted". Where the failed job stood says nothing of a
 * note after the kill; with no kill time (kill_ns 0) the note's age is from now_ns and the job is
 * left out. */
static inline int tgm_bind_note_format(char *buf, unsigned long len, const struct tgm_bind_note *n,
				       uint64_t now_ns, uint64_t kill_ns, uint32_t slot,
				       uint32_t failed)
{
	const int after = kill_ns && n->ns > kill_ns;
	const uint64_t from = kill_ns ? kill_ns : now_ns;
	char surface[24] = "";

	if (n->surface)
		snprintf(surface, sizeof(surface), " of surface %u", n->surface);
	return snprintf(buf, len,
			"%llu ms %s: %s 0x%llx+0x%llx of BO %llu (%llu bytes, %s%s at 0x%llx), "
			"kind 0x%x%s",
			(unsigned long long)((after ? n->ns - from : from - n->ns) / 1000000),
			!kill_ns ? "ago"
			: after  ? "after the kill"
				 : "before the kill",
			tgm_bind_op_name(n->op), (unsigned long long)n->va,
			(unsigned long long)n->range, (unsigned long long)n->handle,
			(unsigned long long)n->bo_bytes, tgm_bind_origin_name(n->origin), surface,
			(unsigned long long)n->bo_offset, n->kind,
			after || !kill_ns ? "" : tgm_bind_note_job(n, slot, failed));
}
#endif
