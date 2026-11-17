// SPDX-License-Identifier: MIT
// TegraGM20BRecord.h's record of a surface's memory, modelled: the entry deferred on it, the surface ID a lock names it
// by, and whether the surface reads unwritten (its seed, its creation)
#pragma once
#include <cstdint>
#include "drivers/TegraGM20B/TegraGM20BRecord.h"

struct tgm_record {
	tgm_coh *deferred = nullptr;
	uint32_t id = 0;
};

extern bool gSurfaceUnwritten;
