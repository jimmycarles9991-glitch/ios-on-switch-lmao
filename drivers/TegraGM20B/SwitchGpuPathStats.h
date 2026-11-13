// SPDX-License-Identifier: MIT
// The paths behind the capability bits GM20B clients take (debug kernels): counted per process for the HW Test's
// gpu.path.* cases (kern.switch_gpu_paths: the flags GetInfo advertises and the reader's own counts) and per stats window,
// every process together, for the periodic "paths" line. A release kernel has the calls only, empty.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum switch_gpu_path {
	SWITCH_GPU_PATH_INTERNAL_JOB, /* JobVisibility: a job that ended without its L2 flush */
	SWITCH_GPU_PATH_EXTERNAL_JOB, /* JobVisibility: a job the runtime or the kernel made External */
	SWITCH_GPU_PATH_UNLOCKED_WRITE, /* External because it wrote an IOSurface over its creator's memory */
	SWITCH_GPU_PATH_VOLATILE_BIND, /* JobVisibility: a control BO bound GPU-volatile */
	SWITCH_GPU_PATH_LEAN_COPY_JOB, /* LeanCopyChannel: a job on a copy-only channel */
	SWITCH_GPU_PATH_CONTROL_BO,    /* ControlReserve: a control BO from the reserve */
	SWITCH_GPU_PATH_CPU_COMPLETE,  /* CpuComplete: a call */
	SWITCH_GPU_PATH_RANGED_USE,    /* UseRanges: a use with a byte range */
	SWITCH_GPU_PATH_VISIBLE_WAIT, /* a CPU relayout's wait for the surface's GPU writes to reach DRAM */
	SWITCH_GPU_PATH_NATIVE_VIEW, /* NativeSurfaceView: a view of a decoded picture's own memory */
	SWITCH_GPU_PATH_NATIVE_READ, /* NativeSurfaceView: a job's read of one */
	SWITCH_GPU_PATH_NATIVE_TILED, /* NativeSurfaceView: a view of a block-linear picture (not the pitch planes) */
	SWITCH_GPU_PATH_EVENT_JOB, /* SharedEvents: a job that waits for or signals a shared event */
	SWITCH_GPU_PATH_READY_ARM, /* ResourceReady: a readiness notification armed for a refused admission */
	SWITCH_GPU_PATH_SUBMIT_JOB, /* SubmitJob (TegraGM20BJobABI.h): a job it ran */
	SWITCH_GPU_PATH_SET_JOB, /* SubmitJob: a job it ran that named a resource set at its current generation */
	SWITCH_GPU_PATH_STALE_SET, /* SubmitJob: kIOReturnNotReady for a resource set's ended generation */
	SWITCH_GPU_PATH_ADDRESS_JOB, /* SubmitAddress: a job it ran */
	SWITCH_GPU_PATH_SHARED_USE, /* SubmitJob: a use a job named beside its set (no member of it) */
	SWITCH_GPU_PATH_RELAYOUT_FRESH, /* a block-linear decision that moved nothing: no one had written the surface */
	SWITCH_GPU_PATH_RELAYOUT_DEFERRED, /* a last import that left the surface block-linear for a CPU lock to untile */
	SWITCH_GPU_PATH_RELAYOUT_LAZY, /* a CPU lock that untiled a deferred surface */
	SWITCH_GPU_PATH_RELAYOUT_NEVER, /* a deferred surface whose memory went before anything asked for rows */
	SWITCH_GPU_PATH_RELAYOUT_MOVED, /* a tiling or untiling move in place */
	SWITCH_GPU_PATH_QUIET_JOB, /* SubmitJob: a job ending with no interrupt unless asked (SwitchGpuQuiet.h) */
	SWITCH_GPU_PATH_WAKE_ASK,       /* a wait that asked for a wake entry behind a quiet job */
	SWITCH_GPU_PATH_WRITER_PROMISE, /* method 34: a writer promise made (SwitchGpuWriterPromise.h) */
	SWITCH_GPU_PATH_PROMISE_WAIT, /* a job, present or VIC/NVDEC job that waits on the GPU for another's writer promise */
	SWITCH_GPU_PATH_PROMISE_CPU_RELEASE, /* a pending writer promise's word written by the CPU (cancel, kill, close, give-up) */
	SWITCH_GPU_PATH_COUNT
};

/* A direct present of a promised frame (gpu_present.c, gpu_promise.c): watched on its promise, handed to the job that
 * keeps it, or given up on (SwitchGpuPromiseGiveUp.h); kern.switch_gpu_promises */
enum {
	SWITCH_GPU_PATH_DIRECT_PROMISED,
	SWITCH_GPU_PATH_DIRECT_KEPT,
	SWITCH_GPU_PATH_DIRECT_GIVEN_UP
};

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define SWITCH_GPU_PATH_STATS 1
void switch_gpu_path_taken(enum switch_gpu_path path, uint32_t count);
void switch_gpu_path_job(uint32_t visibility, int unlocked_writes, int lean_copy,
			 uint32_t ranged_uses);
/* The window's counts as "name=N ...", zeroed for the next window */
void switch_gpu_path_window(char *line, size_t size);
/* kern.switch_gpu_paths: "flags=0x... pid=N name=N ...", the reading process's counts */
void switch_gpu_path_register(void);
/* The same line for the calling process, as kern.switch_gpu_paths and TegraGM20BMethodPaths answer it */
void switch_gpu_path_line(char *line, size_t size);
/* A writer promise given up on, whose reader had waited waited_ns: kern.switch_gpu_promises */
void switch_gpu_path_promise_abandoned(uint64_t waited_ns);
void switch_gpu_path_promise_direct(int what);
/* SwitchGpuSpanNotes.h: a render server's long span, the compositor's job times (the interrupt pass), and the line of both */
void switch_gpu_span_note(const char *owner, const char *text);
void switch_gpu_compositor_job(uint64_t ns);
void switch_gpu_span_line(char *line, size_t size);
/* SwitchGpuCompositorSync.h: a profiled compositor submission's sync pushes (the interrupt pass), and the window's line */
void switch_gpu_compositor_sync(uint32_t syncs, uint64_t ns, int behind);
void switch_gpu_compositor_sync_window(char *line, size_t size);
#else
#define SWITCH_GPU_PATH_STATS 0

static inline void switch_gpu_path_promise_abandoned(uint64_t waited_ns)
{
	(void)waited_ns;
}

static inline void switch_gpu_path_promise_direct(int what)
{
	(void)what;
}

static inline void switch_gpu_path_taken(enum switch_gpu_path path, uint32_t count)
{
	(void)path;
	(void)count;
}

static inline void switch_gpu_path_job(uint32_t visibility, int unlocked_writes, int lean_copy,
				       uint32_t ranged_uses)
{
	(void)visibility;
	(void)unlocked_writes;
	(void)lean_copy;
	(void)ranged_uses;
}

static inline void switch_gpu_compositor_job(uint64_t ns)
{
	(void)ns;
}

static inline void switch_gpu_compositor_sync(uint32_t syncs, uint64_t ns, int behind)
{
	(void)syncs;
	(void)ns;
	(void)behind;
}

static inline void switch_gpu_path_window(char *line, size_t size)
{
	if (size)
		line[0] = 0;
}
#endif

/* TegraGM20BUserClient.cpp: what GetInfo advertises */
uint32_t switch_gpu_info_flags(void);

#ifdef __cplusplus
}
#endif
