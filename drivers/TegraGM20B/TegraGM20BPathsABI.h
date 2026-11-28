/* SPDX-License-Identifier: MIT */
/* The calling process's GPU path counts, debug kernels only (a release kernel answers kIOReturnUnsupported): a call on a
 * TegraGM20B connection (TegraGM20BABI.h's wire form, direct or tunnelled) answering kern.switch_gpu_paths' line
 * (SwitchGpuPathStats.h), which an app's sandbox may deny it as a sysctl, or the push scan's target peaks, which no
 * sysctl carries. Kept out of TegraGM20BABI.h, which NVK, the GPU
 * seed and the stack key build from: the method number is far from theirs. */
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
	/* "ready_kib=N floor_kib=N held_kib=N clean_chunks=N": the GPU pool's ready bytes (what a BoAlloc can take now), the
	 * floor no compressible BoAlloc goes below (TegraGM20BPoolPolicy.h), the pool bytes this connection holds, and the
	 * 128 KiB chunks a compressible BoAlloc could take now */
	TegraGM20BPathsOpPool = 4,
	/* "lines=N": the refusal lines this connection's GPU job calls wrote (SwitchGpuRefusalSites.h) */
	TegraGM20BPathsOpRefusals = 5,
	/* "foreign_jobs=N foreign_fences=N foreign_promises=N foreign_events=N foreign_host1x=N": monotonic compositor
	 * foreign-wait totals since boot (TGM_STATS) */
	TegraGM20BPathsOpForeignWaits = 6,
	/* "ids=a,b,c values=x,y,z": three host1x syncpoints for HW Test probes' GPU increments (gpu.x1.clean_l2_syncpoint,
	 * gpu.x1.shadow_ram), taken from host1x at the first ask and never waited on, and their values now. A process
	 * without SWITCH_GPU_PROBES_ENTITLEMENT gets kIOReturnNotPrivileged. */
	TegraGM20BPathsOpProbeSyncpts = 7,
	/* "jobless=N": host1x consumers' bridges that waited on the syncpoint of a channel of a process with the caller's
	 * name, live or closed, with no GPU job of their own (TegraGM20BGpu.h tgm_chan_jobless; HW Test
	 * gpu.x1.bridge_jobless) */
	TegraGM20BPathsOpBridge = 8,
	/* "colour=U/N depth=U/N c=FF:DS0,DS1,DS2,DS3;... d=FF:DS;...": the GPU's ZBC tables, every process's: entries held of
	 * the slots, then each held entry's NV9097 format and SET_*_CLEAR_VALUE words in hex (TegraGM20BABI.h method 19) */
	TegraGM20BPathsOpZbc = 9,
};

/* HW Test's alone: TegraGM20BPathsOpProbeSyncpts, and the facade's raw 3D-class methods (TegraGM20BMetalProbe.mm), which
 * pass none of the encoders' checks and can increment any host1x syncpoint */
#define SWITCH_GPU_PROBES_ENTITLEMENT "com.brunostjohn.switch.gpu-probes"

struct TegraGM20BAbiPaths {
	uint32_t op;
	uint32_t reserved;
};

/* The note-span op's input: the span as span_marks.h formats it, NUL-terminated */
struct TegraGM20BAbiPathsSpan {
	uint32_t op;
	uint32_t reserved;
	char text[112];
};

/* Counts: "flags=0x... pid=N name=N ...", NUL-terminated */
struct TegraGM20BAbiPathsReply {
	char line[1024];
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiPaths) == 8, "TegraGM20BAbiPaths layout");
static_assert(sizeof(TegraGM20BAbiPathsReply) == 1024, "TegraGM20BAbiPathsReply layout");
static_assert(sizeof(TegraGM20BAbiPathsSpan) == 120, "TegraGM20BAbiPathsSpan layout");
#endif
#endif
