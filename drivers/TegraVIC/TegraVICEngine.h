// SPDX-License-Identifier: BSD-2-Clause-Patent
// The engine steps of TegraVIC.cpp that a stalled job's cancellation (TegraVICCancel.cpp) runs, each under
// switch_vic_power_lock.
#pragma once
#include <IOKit/IOLocks.h>
#include <stddef.h>
#include <stdint.h>

// The cancellation's thread call, at bring-up: false if it could not be allocated.
bool switch_vic_cancel_start(uint32_t syncpt);

// The idle gate's lock: power transitions against the cancellation.
IOLock *switch_vic_power_lock();
// False once sleep has powered the engine off: its registers must not be read then.
bool switch_vic_engine_powered();
// The falcon (switch_falcon_stall) and the memory controller's error capture.
void switch_vic_engine_describe(char *text, size_t size);
// VIC's memory client flushed (drained: within the flush's bound), then the engine reset with its partition kept on.
bool switch_vic_engine_reset(bool *drained);
// The falcon booted again from the kept image and proven with V2's stream, as at power-on.
bool switch_vic_engine_boot();
// After a reset or boot that failed: the engine back in reset with its clock off, VIC refusing work. Returns why.
const char *switch_vic_engine_wedge();
