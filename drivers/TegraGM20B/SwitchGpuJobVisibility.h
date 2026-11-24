// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "TegraGM20BJobABI.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BVolatileABI.h"

// The chan_exec visibility of a SubmitJob of these TegraGM20BJob* flags. An Internal job completes with its writes possibly
// still in the GPU's L2, so a job ends External when a CPU reader can see its completion without anything waiting for the
// L2: an event signal's listener, or a write into memory its owner reads with no lock (an IOSurface over its creator's
// own pages). Volatile-only host writes (TegraGM20BJobVolatileWrites) take WFI + FB_FLUSH without L2_FLUSH_DIRTY.
inline uint32_t switch_gpu_job_visibility(uint32_t job, bool signals, bool unlockedWrites,
					  bool hostWritten)
{
	const bool external = (job & TegraGM20BJobExternal) || signals || unlockedWrites;
	const bool volatileWrites = !external && (job & TegraGM20BJobVolatileWrites);
	return uint32_t(SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT) |
	       (external ? uint32_t(SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY) : 0u) |
	       (volatileWrites ? uint32_t(SWITCH_GPU_SUBMIT_VOLATILE_VISIBILITY) : 0u) |
	       ((job & TegraGM20BJobHasEvents) ? uint32_t(SWITCH_GPU_SUBMIT_FORCE_FENCE) : 0u) |
	       (hostWritten ? uint32_t(SWITCH_GPU_SUBMIT_HOST_WRITTEN) : 0u);
}
