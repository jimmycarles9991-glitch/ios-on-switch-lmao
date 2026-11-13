/* SPDX-License-Identifier: MIT */
/* What a submission tells the ownership layer about the BOs it uses. A use it names is a claim: a native Submit or
 * SubmitAddress names every use, a legacy one (Exec, or Submit with ExecSurfaces semantics) only the imports its list
 * names, and a listed legacy job touches no import it did not list (TegraGM20BABI.h method 17). No CPU owner refuses a
 * claim: no submission ABI has such a refusal, NVK and raw take one as fatal and Busy would make NVK abort or spin.
 * A submission that took an admission (TegraGM20BSubmitAdmission*) was ordered behind CPU owners by its readiness
 * observer and keeps begin's exclusion. Everything else a legacy job may touch is UNLISTED: CPU access still orders
 * after the job. An import is listable once it has a coherency entry, as in ExecSurfaces' index. */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"
#include "TegraGM20BABI.h"

/* The ownership flags a named use carries beside its SWITCH_GPU_BACKING_READ/WRITE access. */
static inline uint32_t switch_gpu_claim_flags(bool admitted)
{
	return admitted ? 0u : uint32_t(SWITCH_GPU_BACKING_NO_CPU_EXCLUSION);
}

static inline uint32_t switch_gpu_legacy_access(bool listable, bool read_only, bool listed,
						uint32_t listed_access)
{
	const uint32_t touch = read_only ? SWITCH_GPU_BACKING_READ : SWITCH_GPU_BACKING_WRITE;
	if (!listable || !listed)
		return touch | SWITCH_GPU_BACKING_UNLISTED;
	uint32_t access = 0;
	if (listed_access & TegraGM20BSurfaceRead)
		access |= SWITCH_GPU_BACKING_READ;
	if (listed_access & TegraGM20BSurfaceWrite)
		access |= touch;
	return access ? access | switch_gpu_claim_flags(false) : 0;
}

/* bos: the connection's BO list (next, coh, readOnly, handle); listed_access(handle): the TegraGM20BSurface* access the
 * job's list gives an import; add(bo, access): records one use, returning 0 or an errno. At most one use per BO. */
template <class Bo, class ListedAccess, class Add>
int switch_gpu_legacy_uses(const Bo *bos, bool listed, ListedAccess listed_access, Add add)
{
	for (const Bo *bo = bos; bo; bo = bo->next) {
		const bool listable = bo->coh != nullptr;
		const uint32_t access = switch_gpu_legacy_access(
			listable, bo->readOnly, listed,
			listable && listed ? listed_access(bo->handle) : 0u);
		if (!access)
			continue;
		if (const int error = add(bo, access))
			return error;
	}
	return 0;
}
