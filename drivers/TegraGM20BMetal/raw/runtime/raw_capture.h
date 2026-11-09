// SPDX-License-Identifier: MIT
// A render state's draws read off NVK's own emission, in process: the commands NVK recorded for one command buffer, never
// submitted, decoded on the CPU by pack/nvdec (Mesa's MME simulator for NVK's macros) as the pack generator decodes its probe's.
// Each command buffer is decoded from NVK's context as the pack holds it (PackInit: NVK's queue init of this build, its MME
// macros among it) with this process's addresses where the pack marks them: what NVK assumes at a command buffer's start.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../pack/probe_record.h"
#include "raw_record.h"

namespace nvdec {
class Decoder;
struct ProgramInfo;
}  // namespace nvdec

namespace raw {

class Pack;
struct InitAddresses;

class Capture {
public:
    // [va, va + bytes) of the recording driver's GPU VM as its CPU left it, or null
    using Map = std::function<const uint8_t *(uint64_t va, uint64_t bytes)>;
    // The program a draw binds at SET_PIPELINE_PROGRAM offset `offset`, as the recording driver uploaded it; false: none
    using ProgramAt = std::function<bool(uint32_t offset, nvdec::ProgramInfo &out)>;

    Capture(Map map, ProgramAt programAt);
    ~Capture();

    bool prime(const Pack &pack, const InitAddresses &here, std::string &err);
    // One command buffer's push segments, in order
    nvdec::ProbeSubmission decode(const std::vector<std::pair<const uint32_t *, uint64_t>> &segments);
    // The 3D registers the context leaves that are not 0
    std::map<uint32_t, uint32_t> context() const;
    // A program a decoded draw bound, by its id; null: none
    const CapturedProgram *program(const std::string &id);

private:
    Map map_;
    ProgramAt programAt_;
    FILE *sink_;  // the decoder's canonical records, which nothing reads
    std::unique_ptr<nvdec::Decoder> primed_;
    std::map<std::string, CapturedProgram> programs_;
};

}  // namespace raw
