/* SPDX-License-Identifier: MIT */
/* The register dump into the kernel log, debug kernels only (a release kernel answers kIOReturnUnsupported): a call on
 * a TegraGM20B connection (TegraGM20BABI.h's wire form, direct or tunnelled) through which hw_test_app has the kernel
 * read TegraGM20BRegDump.h's registers and log them, chunked, as "tegra-gm20b: regdump v1" lines (regdump.c). Each tag
 * dumps once a boot. Kept out of TegraGM20BABI.h, which NVK, the GPU seed and the raw stack key build from: the method
 * number is far from theirs. */
#ifndef TEGRA_GM20B_REG_DUMP_ABI_H
#define TEGRA_GM20B_REG_DUMP_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodRegdump = 0x5244, /* 'RD' */

	/* TegraGM20BAbiRegdump.tag */
	TegraGM20BRegdumpQuiet = 0,
	TegraGM20BRegdumpHotStates = 1,
	TegraGM20BRegdumpTags = 2,

	/* TegraGM20BAbiRegdumpReply.status */
	TegraGM20BRegdumpLogged = 0,
	TegraGM20BRegdumpAlready = 1,  /* this tag's dump is in this boot's log */
	TegraGM20BRegdumpNotReady = 2, /* the GPU's bring-up has not reached M3 */
	TegraGM20BRegdumpNoMemory = 3,
};

struct TegraGM20BAbiRegdump {
	uint32_t tag;
	uint32_t reserved;
};

/* Logged: what the dump's done line also carries (TegraGM20BRegDump.h's tgm_regdump_how). */
struct TegraGM20BAbiRegdumpReply {
	int32_t status;
	uint32_t count; /* registers logged */
	uint32_t lines;
	uint32_t badf;
	uint32_t bus_fault;
	uint32_t engine_before, engine_after; /* FIFO engine 0's (GR's) status around the reads */
	uint32_t read_us;
	uint64_t list_id;
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiRegdump) == 8, "TegraGM20BAbiRegdump layout");
static_assert(sizeof(TegraGM20BAbiRegdumpReply) == 40, "TegraGM20BAbiRegdumpReply layout");
#endif
#endif
