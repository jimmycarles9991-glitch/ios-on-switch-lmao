// SPDX-License-Identifier: BSD-2-Clause-Patent
// The engine steps of TegraNVDEC.cpp that a stalled job's cancellation (TegraNVDECCancel.cpp) runs, each under
// tegra_nvdec_power_lock.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The cancellation's thread call, at bring-up: false if it could not be allocated.
bool switch_nvdec_cancel_start(uint32_t syncpt);

// False once the idle gate or sleep has powered the engine off: its registers must not be read then.
bool switch_nvdec_engine_powered();
// The falcon (switch_falcon_stall) and the memory controller's error capture.
void switch_nvdec_engine_describe(char *text, size_t size);
// NVDEC's memory client flushed (drained: within the flush's bound), then the engine reset with its partition kept on.
bool switch_nvdec_engine_reset(bool *drained);
// The falcon booted again from the kept image and proven by an IMMEDIATE and an OP_DONE increment, as at power-on.
bool switch_nvdec_engine_boot();
// After a reset or boot that failed: the engine back in reset with its clock off, NVDEC refusing work. Returns why.
const char *switch_nvdec_engine_wedge();
