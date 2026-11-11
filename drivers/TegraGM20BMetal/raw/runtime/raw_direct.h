// SPDX-License-Identifier: MIT
// A state's record (raw_record.h) made directly: its programs compiled by NVK from m2v's SPIR-V and layout for the pack
// generator's probe draws (bridge/NativeMetalDirect.h), its set-0 entries and push words from m2v's binding plans, its blob from
// raw_emit. No pipeline, command buffer or capture. test/directcheck holds the records to the pack's.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "raw_compiler.h"
#include "raw_record.h"

struct NmbDirectState;

namespace raw {

// The NVK exports the direct compile calls (nvk_switch_compile.h), found in the NVK the device opened
struct DirectNvk {
    void *compileStage = nullptr, *stageFinish = nullptr, *setLayoutBinding = nullptr, *setLayoutSize = nullptr,
         *immutableSamplerHeader = nullptr, *bufferDescriptorSize = nullptr, *samplerHeader = nullptr,
         *computeQmd = nullptr, *viewHeaderWord0 = nullptr, *splitParts = nullptr;
    bool abiMatches = false;  // raw_direct.cpp's copies of NVK's structs are NVK's size (switch_nvk_compile_abi)

    static DirectNvk find(void *(*symbol)(const char *name));
    bool complete() const;
    bool kernelComplete() const;
};

// The record of the state `state` describes, of pack key `key`; `attributes`: its vertex function's [[attribute(n)]] inputs
// (the reflection's active vertexAttributes). Refused: a state the raw path does not draw from a record (`why`); Failed: NVK
// could not compile it. `registers`: the 3D registers the record is made from (raw_emit's over the rules' baseline and context).
// `split`: with its fragment program's split (RuntimeRecord::splitTried), which costs about one more compile of that stage
Compiler::Outcome directRecord(const NmbDirectState &state, const DirectNvk &nvk, const PackRules &rules,
                               const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                               std::string &why, std::map<uint32_t, uint32_t> *registers = nullptr, bool split = false);

}  // namespace raw
