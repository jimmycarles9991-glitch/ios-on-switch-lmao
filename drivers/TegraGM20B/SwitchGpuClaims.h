/* SPDX-License-Identifier: MIT */
/* What a SubmitJob tells the ownership layer about the BOs it uses. A use it names is a claim. No CPU owner refuses a
 * claim: the submission ABI has no such refusal and raw takes one as fatal. A job that took an admission
 * (TegraGM20BJobAdmission*) was ordered behind CPU owners by its readiness observer and keeps begin's exclusion for its
 * writes. */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"

/* The ownership flags a named use carries beside its SWITCH_GPU_BACKING_READ/WRITE access. */
static inline uint32_t switch_gpu_claim_flags(bool admitted)
{
	return admitted ? 0u : uint32_t(SWITCH_GPU_BACKING_NO_CPU_EXCLUSION);
}

/* One use's, from its submission's `claim`: a read goes on beside CPU owners even in an admission, as an IOSurface lock
 * lets any reader (IOSurfaceRef.h: the lock only bounds CPU access); an admitted write keeps begin's exclusion. */
static inline uint32_t switch_gpu_claim_use(uint32_t claim, uint32_t access)
{
	return access & SWITCH_GPU_BACKING_WRITE ? claim
						 : uint32_t(SWITCH_GPU_BACKING_NO_CPU_EXCLUSION);
}
