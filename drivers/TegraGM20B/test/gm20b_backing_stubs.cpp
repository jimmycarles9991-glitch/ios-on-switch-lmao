// SPDX-License-Identifier: MIT
#include <stdint.h>
#include "drivers/TegraGM20B/SwitchGpuPathStats.h"
#include "drivers/TegraGM20B/SwitchGpuBacking.h"
#include "gm20b_record_model.h"
uint64_t gBackingContent;
bool gSurfaceUnwritten = true;

extern "C" void tgm_record_park(tgm_record *record, tgm_coh *c)
{
	record->deferred = c;
}

extern "C" tgm_coh *tgm_record_take(tgm_record *record)
{
	tgm_coh *c = record->deferred;
	record->deferred = nullptr;
	return c;
}

extern "C" int tgm_record_is_surface(const tgm_record *record, uint32_t id)
{
	return static_cast<int>(record->id == id);
}

extern "C" int tgm_surface_unwritten(void *)
{
	return static_cast<int>(gSurfaceUnwritten);
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *, switch_gpu_backing_info *info)
{
	*info = {};
	info->content_generation = gBackingContent;
}

extern "C" switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t)
{
	return nullptr;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *)
{
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *)
{
}

#if SWITCH_GPU_PATH_STATS
extern "C" void switch_gpu_path_taken(enum switch_gpu_path, uint32_t)
{
}
#endif
