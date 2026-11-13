// SPDX-License-Identifier: MIT
// The scanout read-back (TegraGM20BReadbackABI.h), debug kernels only. The display's path (tgm_readback_take in
// tegra_gm20b_scanout_copy, tgm_readback_done from the GPU interrupt pass) touches only atomics; a connection's calls
// take the read-back's own lock.
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BReadbackABI.h"

#if TGM_STATS
IOReturn tgm_readback_call(const void *owner, const TegraGM20BAbiReadback *in,
			   TegraGM20BAbiReadbackReply *out);
// The owner's buffer behind a Done reply's token, retained; null for anything else.
IOMemoryDescriptor *tgm_readback_memory(const void *owner, uint32_t token);
void tgm_readback_closed(const void *owner);

// The reads the present of `copy` runs when armed (0: none), from its mapped source and target; the read-back is then in
// flight. A present that is refused gives it back with tgm_readback_refused; one that is queued completes through
// tgm_readback_done, which completes the display's offer.
uint32_t tgm_readback_take(const TegraScanoutCopy *copy, uint64_t source, int32_t sourceBlockLog2,
			   uint64_t target, tgm_present_read (&reads)[TGM_PRESENT_READS]);
void tgm_readback_refused();
void tgm_readback_done(void *display, int status);
#endif
