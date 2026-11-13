// SPDX-License-Identifier: MIT
// A kernel's record (raw_compute_record.h) made directly at one key (raw_compute_key.h): NAK's program and NVK's QMD for m2v's SPIR-V at
// the key's threadgroup shape, its set-0 entries and push words from m2v's plans of the pack generator's three probe dispatches
// (bridge/NativeMetalDirect.h nmb_compute_pipeline_direct). No pipeline, dispatch or capture.
#pragma once
#include <string>
#include "raw_compute_record.h"
#include "raw_direct.h"

struct NmbDirectKernel;

namespace raw {

// Refused: a kernel the raw path does not dispatch from a record (`why`); Failed: NVK could not compile it
CompileOutcome directKernel(const NmbDirectKernel &kernel, const DirectNvk &nvk, const std::string &key,
                            ComputeRecord &out, std::string &why);

}  // namespace raw
