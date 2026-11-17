// SPDX-License-Identifier: MIT
// The coherency unit's hooks into surface records and the tiled scratch, which switch_gpu_program_pages_test's program
// grains never reach: records as gm20b_record_model.h models them, no scratch handed out.
#include <cstdint>
#include "drivers/TegraGM20B/TegraGM20BTiled.h"
#include "gm20b_record_model.h"

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

struct tgm_scratch *tgm_scratch_reclaim(struct tgm_scratch *, uint64_t, int *intact)
{
	*intact = 0;
	return nullptr;
}

uint64_t tgm_scratch_handout(const struct tgm_scratch *)
{
	return 0;
}
