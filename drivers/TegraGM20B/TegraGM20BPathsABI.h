/* SPDX-License-Identifier: MIT */
/* The calling process's GPU path counts, debug kernels only (a release kernel answers kIOReturnUnsupported): a call on a
 * TegraGM20B connection (TegraGM20BABI.h's wire form, direct or tunnelled) answering kern.switch_gpu_paths' line
 * (SwitchGpuPathStats.h), which an app's sandbox may deny it as a sysctl, or the push scan's target peaks, which no
 * sysctl carries. Kept out of TegraGM20BABI.h, which NVK, the GPU
 * seed and the raw stack key build from: the method number is far from theirs. */
#ifndef TEGRA_GM20B_PATHS_ABI_H
#define TEGRA_GM20B_PATHS_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodPaths = 0x5050, /* 'PP' */
};

enum {
	TegraGM20BPathsOpCounts = 0,
	/* every process's largest colour-target image since boot (SwitchGpuTargetPeaks.h): "owner=WxHxS;...", largest first */
	TegraGM20BPathsOpTargetPeaks = 1,
	/* a render server's raw path notes one of its long GPU spans (TegraGM20BAbiPathsSpan; SwitchGpuSpanNotes.h) */
	TegraGM20BPathsOpNoteSpan = 2,
	/* "job_max_us=N spans=K; <owner>: <span>; ...": the compositor's longest job and the spans noted since the last ask */
	TegraGM20BPathsOpSpans = 3,
};

struct TegraGM20BAbiPaths {
	uint32_t op;
	uint32_t reserved;
};

/* The note-span op's input: the span as raw_span_marks.h formats it, NUL-terminated */
struct TegraGM20BAbiPathsSpan {
	uint32_t op;
	uint32_t reserved;
	char text[112];
};

/* Counts: "flags=0x... pid=N name=N ...", NUL-terminated */
struct TegraGM20BAbiPathsReply {
	char line[512];
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiPaths) == 8, "TegraGM20BAbiPaths layout");
static_assert(sizeof(TegraGM20BAbiPathsReply) == 512, "TegraGM20BAbiPathsReply layout");
static_assert(sizeof(TegraGM20BAbiPathsSpan) == 120, "TegraGM20BAbiPathsSpan layout");
#endif
#endif
