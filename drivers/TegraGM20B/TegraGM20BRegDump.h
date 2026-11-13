/* Debug kernels: the GM20B registers L4T's capture.sh reads under nvgpu, in its "0xOFFSET 0xVALUE" lines, so
 * diff_regs.py can set what we program against what nvgpu programs. The list leaves out falcon memory and tag ports
 * (auto-increment), access ports whose read acts, write-only set/clear registers and the PRAMIN window. The includer
 * provides snprintf. */
#ifndef TEGRA_GM20B_REG_DUMP_H
#define TEGRA_GM20B_REG_DUMP_H

#include <stddef.h>
#include <stdint.h>
#include "TegraGM20BRegDumpList.h"

#define TGM_REGDUMP_COUNT (sizeof(tgm_regdump_offsets) / sizeof(tgm_regdump_offsets[0]))
#define TGM_REGDUMP_HEADER 192u
#define TGM_REGDUMP_LINE 20u /* "0x%06x 0x%08x\n" */
#define TGM_REGDUMP_BYTES (TGM_REGDUMP_HEADER + TGM_REGDUMP_LINE * TGM_REGDUMP_COUNT)

/* How one pass of reads went (regdump.c). A context switch can fall among the reads: FIFO engine 0's (GR's) status and
 * FECS's current context before and after them tell whether one did. */
struct tgm_regdump_how {
	uint32_t badf; /* registers that read back a PRI error, 0xbadf.... */
	uint32_t
		bus_fault; /* the first faulting address the bus latched during the reads; 0: none */
	uint32_t engine_before, engine_after;
	uint32_t ctx_before, ctx_after;
	uint32_t read_us;
};

static inline uint32_t tgm_regdump_badf(const uint32_t *values)
{
	uint32_t n = 0;

	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++)
		n += (values[i] >> 16) == 0xbadfu;
	return n;
}

/* The dump in buf, NUL-terminated: its length, or 0 when cap is short of TGM_REGDUMP_BYTES. values are the reads in
 * list order. */
static inline size_t tgm_regdump_format(char *buf, size_t cap, const uint32_t *values,
					const struct tgm_regdump_how *how)
{
	size_t len;

	if (cap < TGM_REGDUMP_BYTES)
		return 0;
	len = (size_t)snprintf(
		buf, cap,
		"# regdump v1 list=%016llx count=%u\n"
		"# read badf=%u bus_fault=%06x engine0=%08x/%08x fecs_ctx=%08x/%08x read_us=%u\n",
		(unsigned long long)TGM_REGDUMP_LIST_ID, (unsigned)TGM_REGDUMP_COUNT, how->badf,
		how->bus_fault, how->engine_before, how->engine_after, how->ctx_before,
		how->ctx_after, how->read_us);
	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++)
		len += (size_t)snprintf(buf + len, cap - len, "0x%06x 0x%08x\n",
					tgm_regdump_offsets[i], values[i]);
	return len;
}

/* The kernel log's form (regdump.c): the values alone, in list order, TGM_REGDUMP_CHUNK a line, each line naming the
 * list, the tag, its chunk and the list index of its first value; then a done line with the count and an FNV-1a sum of
 * the values' bytes, so a reader can tell a lost or cut line. Each line fits nvxnu_stats's TGM_REGDUMP_LOG_LINE bytes,
 * well inside the kernel's 1 KiB os_log message. */
#define TGM_REGDUMP_CHUNK 64u
#define TGM_REGDUMP_CHUNKS ((TGM_REGDUMP_COUNT + TGM_REGDUMP_CHUNK - 1) / TGM_REGDUMP_CHUNK)
#define TGM_REGDUMP_LOG_LINE 768u

static inline uint32_t tgm_regdump_sum(const uint32_t *values)
{
	uint32_t h = 0x811c9dc5u;

	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++) {
		for (unsigned shift = 0; shift < 32; shift += 8)
			h = (h ^ ((values[i] >> shift) & 0xffu)) * 0x01000193u;
	}
	return h;
}

/* Chunk `chunk` (< TGM_REGDUMP_CHUNKS) into buf, NUL-terminated; snprintf's length. */
static inline size_t tgm_regdump_chunk_line(char *buf, size_t cap, const uint32_t *values,
					    const char *tag, size_t chunk)
{
	const size_t first = chunk * TGM_REGDUMP_CHUNK;
	const size_t left = TGM_REGDUMP_COUNT - first;
	const size_t n = left < TGM_REGDUMP_CHUNK ? left : TGM_REGDUMP_CHUNK;
	size_t len = (size_t)snprintf(
		buf, cap, "tegra-gm20b: regdump v1 list=%016llx tag=%s chunk=%u/%u first=%u n=%u",
		(unsigned long long)TGM_REGDUMP_LIST_ID, tag, (unsigned)chunk,
		(unsigned)TGM_REGDUMP_CHUNKS, (unsigned)first, (unsigned)n);

	for (size_t i = 0; i < n && len < cap; i++)
		len += (size_t)snprintf(buf + len, cap - len, " %08x", values[first + i]);
	return len;
}

static inline size_t tgm_regdump_done_line(char *buf, size_t cap, const uint32_t *values,
					   const char *tag, const struct tgm_regdump_how *how)
{
	return (size_t)snprintf(buf, cap,
				"tegra-gm20b: regdump v1 list=%016llx tag=%s done count=%u "
				"chunks=%u sum=%08x badf=%u "
				"bus_fault=%06x engine0=%08x/%08x fecs_ctx=%08x/%08x read_us=%u",
				(unsigned long long)TGM_REGDUMP_LIST_ID, tag,
				(unsigned)TGM_REGDUMP_COUNT, (unsigned)TGM_REGDUMP_CHUNKS,
				tgm_regdump_sum(values), how->badf, how->bus_fault,
				how->engine_before, how->engine_after, how->ctx_before,
				how->ctx_after, how->read_us);
}

#endif
