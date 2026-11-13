// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BGpu.h"

// Whether a job of these submit flags acquires every CPU write (its chan_exec invalidates the GPU L2 first) whatever its
// uses name: one submitted without explicit visibility. An explicit one acquires them where a use is host-written.
inline bool switch_gpu_submit_acquires_host_writes(uint32_t submit)
{
	return !(submit & TegraGM20BSubmitVisibilityExplicit);
}

// The chan_exec visibility of a SubmitAddress job. An Internal job completes with its writes possibly still in the GPU's
// L2, so a job ends External when a CPU reader can see its completion without anything waiting for the L2: an event
// signal's listener, or a write into memory its owner reads with no lock (an IOSurface over its creator's own pages).
inline uint32_t switch_gpu_job_visibility(uint32_t submit, bool signals, bool unlockedWrites,
					  bool hostWritten)
{
	const bool external =
		(submit & TegraGM20BSubmitExternalVisibility) || signals || unlockedWrites;
	return ((submit & TegraGM20BSubmitVisibilityExplicit)
			? uint32_t(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT)
			: 0u) |
	       (external ? uint32_t(SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY) : 0u) |
	       ((submit & TegraGM20BSubmitHasEvents) ? uint32_t(SWITCH_GPU_SUBMIT_FORCE_FENCE)
						     : 0u) |
	       (hostWritten ? uint32_t(SWITCH_GPU_SUBMIT_HOST_WRITTEN) : 0u);
}
