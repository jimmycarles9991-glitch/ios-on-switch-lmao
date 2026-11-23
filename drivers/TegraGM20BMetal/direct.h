// SPDX-License-Identifier: MIT
// A state's record (record.h) made directly: its programs compiled by NVK from m2v's SPIR-V and layout for the pack
// generator's probe draws (bridge/NativeMetalDirect.h), its set-0 entries and push words from m2v's binding plans, its blob from
// emit. No pipeline, command buffer or capture. test/directcheck holds the records to the pack's.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "record.h"

struct NmbDirectState;

namespace gm20b {

// The stage compile the direct compile calls (third_party/mesa/src/nouveau/switch_compile), linked in; it makes no device
struct StageCompile {
    bool ranked = false;           // switch_compile_ranked_stage in place of switch_compile_stage
    const void *target = nullptr;  // the frozen table the compile takes (nak_target.h)

    static StageCompile linked(bool ranked = false);
    // direct_nvk.h's copies of the compile's structs are its sizes (switch_compile_abi)
    static bool abiMatches();
};

// The record of the state `state` describes, of pack key `key`; `attributes`: its vertex function's [[attribute(n)]] inputs
// (the reflection's active vertexAttributes). Refused: a state the driver does not draw from a record (`why`); Failed: the
// stage compile could not compile it. `registers`: the 3D registers the record is made from (emit's over the rules' baseline and context).
// `split`: with its fragment program's split (RuntimeRecord::splitTried), which costs about one more compile of that stage.
// `variants`: with its program variants (RuntimeRecord::variants), one more compile of both stages each: the pack builder's only.
// `xrSlots`: the fragment texture slots its fragment programs read through two-plane RGB10A8_2P_XR10 header pairs
// (xr_pass.h, the record of its " | xrs=" variant)
CompileOutcome directRecord(const NmbDirectState &state, const StageCompile &compile, const PackRules &rules,
                            const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                            std::string &why, std::map<uint32_t, uint32_t> *registers = nullptr, bool split = false,
                            bool variants = false, uint16_t xrSlots = 0, const SelectorConfigs *selectors = nullptr);

// Whether directRecord splits `state`'s fragment program when asked to (RuntimeRecord::splitTried): only a state of a vertex
// and a fragment stage, never a vertex stage alone or a tessellated state
bool splitsFragment(const NmbDirectState &state);

}  // namespace gm20b
