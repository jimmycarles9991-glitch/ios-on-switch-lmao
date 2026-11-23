// SPDX-License-Identifier: MIT
// A kernel's record (compute_record.h) made directly at one key (compute_key.h): NAK's program and NVK's QMD for m2v's SPIR-V at
// the key's threadgroup shape, its set-0 entries and push words from m2v's plans of the pack generator's three probe dispatches
// (bridge/NativeMetalDirect.h nmb_compute_pipeline_direct). No pipeline, dispatch or capture.
#pragma once
#include <string>
#include "compute_record.h"
#include "direct.h"

struct NmbDirectKernel;

namespace gm20b {

// Refused: a kernel the driver does not dispatch from a record (`why`); Failed: the stage compile could not compile it
CompileOutcome directKernel(const NmbDirectKernel &kernel, const StageCompile &compiler, const std::string &key,
                            ComputeRecord &out, std::string &why);

}  // namespace gm20b
