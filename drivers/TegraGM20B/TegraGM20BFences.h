/* SPDX-License-Identifier: MIT */
/* Implicit GPU order between processes over host imports (TegraGM20BABI.h method 17), as decisions apart from the locks and
 * lists they run under (TegraGM20BExecSurfaces.cpp, gpu_sync.c tgm_chan_follow), so a host test runs them.
 * A writer is one submission that may have written a surface, as the coherency registry keeps it: channel uid << 32 | seqno,
 * 0 none. An Exec with a list counts as a writer of the surfaces it lists for writing only, and waits on the GPU for every
 * listed surface's listed writers on other channels that have not completed. One without (or with
 * TegraGM20BExecSurfacesAll) waits for nothing and counts as a writer of every writable import of its connection, for the
 * CPU lock's wait only: it names surfaces it never wrote (an app's next frame would be the writer of the front buffer the
 * render server composites), so a GPU wait on it would hold the reader for that whole submission. The registry keeps the
 * two apart (tgm_coherency_wrote, tgm_coherency_wrote_listed), and only listed writers are ever followed. */
#ifndef TEGRA_GM20B_FENCES_H
#define TEGRA_GM20B_FENCES_H
#include <stdint.h>
#include "TegraGM20BABI.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t tgm_fence_uid(uint64_t writer)
{
	return (uint32_t)(writer >> 32);
}

static inline uint32_t tgm_fence_seqno(uint64_t writer)
{
	return (uint32_t)writer;
}

/* Adds `writer` to the distinct source channels a submission of channel `self` follows, acc[0..*n) (one entry per channel,
 * its later seqno), unless it is none or self's own (a channel's submissions run in order). 0, or -1 when acc holds `max`
 * other channels already. */
static inline int tgm_fence_merge(uint64_t *acc, uint32_t *n, uint32_t max, uint64_t writer,
				  uint32_t self)
{
	uint32_t i = 0;

	if (!writer || tgm_fence_uid(writer) == self)
		return 0;
	for (i = 0; i < *n; i++) {
		if (tgm_fence_uid(acc[i]) != tgm_fence_uid(writer))
			continue;
		if ((int32_t)(tgm_fence_seqno(writer) - tgm_fence_seqno(acc[i])) > 0)
			acc[i] = writer;
		return 0;
	}
	if (*n == max)
		return -1;
	acc[(*n)++] = writer;
	return 0;
}

/* How a kernel copy on channel `self` (the copy engine's, a tiling move's) follows a surface writer `uid` still running:
 * its own channel runs it in order; another channel's fence in its space's fence view (every channel's is, the kernel's own
 * client channels' included: gpu_chan.c tgm_fence_kernel) is a semaphore acquire in its push. One it cannot acquire on is a
 * refusal, never a CPU wait: the copy goes ahead unordered and says so. */
enum tgm_writer_order {
	TGM_WRITER_IN_CHANNEL,
	TGM_WRITER_GPU_ACQUIRE,
	TGM_WRITER_REFUSED,
};

static inline enum tgm_writer_order tgm_writer_order(uint32_t self, uint32_t uid, int acquirable)
{
	if (uid == self)
		return TGM_WRITER_IN_CHANNEL;
	return acquirable != 0 ? TGM_WRITER_GPU_ACQUIRE : TGM_WRITER_REFUSED;
}

/* A writer's channel as gpu_sync.c finds it under tgm_live_lock (a channel that is gone needs no wait): the follower itself, killed
 * (its fence word passed every seqno it gave out), and its completed seqno. */
struct tgm_fence_source {
	int self;
	int killed;
	uint32_t completed;
};

/* Whether a submission starts with a semaphore acquire on `writer`'s fence. */
static inline int tgm_fence_wanted(uint64_t writer, const struct tgm_fence_source *src)
{
	return (writer != 0 && src->self == 0 && src->killed == 0 &&
		(int32_t)(src->completed - tgm_fence_seqno(writer)) < 0)
		       ? 1
		       : 0;
}

/* Whether a wait on `seqno` of a channel whose fence word is `fence` passed only because the channel was killed with that
 * submission unfinished (its kill record, gpu_hang.c tgm_chan_killed): what the waiter ordered after never ran to its end. */
static inline int tgm_fence_lost(const volatile struct TegraGM20BAbiFence *fence, uint32_t seqno)
{
	return (fence->error != 0 && (int32_t)(seqno - fence->killed_first) >= 0 &&
		(int32_t)(fence->killed_last - seqno) >= 0)
		       ? 1
		       : 0;
}

/* Whether an Exec follows its list: one was given for it, not marked as touching every import, and the Exec submits. */
static inline int tgm_fence_listed(int given, uint32_t flags, uint32_t pushes)
{
	return (given != 0 && (flags & (uint32_t)TegraGM20BExecSurfacesAll) == 0 && pushes != 0)
		       ? 1
		       : 0;
}

/* Whether a submission counts as a writer of an import: with a list, a writable one it lists for writing; without, every
 * writable one. */
static inline int tgm_fence_marks(int listed, uint32_t access, int writable)
{
	return (writable != 0 && (listed == 0 || (access & (uint32_t)TegraGM20BSurfaceWrite) != 0))
		       ? 1
		       : 0;
}

/* Whether that mark marks even a surface TegraGM20BImportGpuReads said the GPU only samples: a listed write does (and is a
 * listed writer, tgm_coherency_wrote_listed); without a list, only once the connection's pushes said every import is
 * written. */
static inline int tgm_fence_always(int listed, int imports_written)
{
	return (listed != 0 || imports_written != 0) ? 1 : 0;
}

#ifdef __cplusplus
}
#endif
#endif
