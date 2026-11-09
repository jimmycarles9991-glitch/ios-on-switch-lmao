// SPDX-License-Identifier: MIT
// A render state's raw record by pack/mkpack.py's rules, for a state the pack lacks: made directly (raw_direct.h) or from the
// pack generator's probe draws (makeRecord, test/recordcheck holds it to the pack), as the pack generator would have.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../pack/probe_bind.h"
#include "raw_pack.h"

namespace nvdec {
struct ProbeSubmission;
}  // namespace nvdec

namespace raw {

// A program as a record holds it
struct CapturedProgram {
    std::string id;  // nvdec's name: the FNV-1a of its header, code and data
    // header + code, NAK's shader data, each constant buffer's nvk_cbuf_type
    std::vector<uint8_t> image, data, cbufTypes;
};

// What every record of a pack shares
struct PackRules {
    std::set<uint32_t> varying;  // the registers the pack's states set apart (their blobs')
    std::map<uint32_t, uint32_t> baseline;
    std::map<uint32_t, uint32_t> context;           // NVK's 3D registers after its context init that are not 0
    std::vector<std::array<uint32_t, 8>> samplers;  // the pack's constexpr sampler headers

    static PackRules of(const Pack &pack, std::map<uint32_t, uint32_t> context);
    // The context the pack's init template leaves: its 3D methods that take no address
    static PackRules of(const Pack &pack);
};

struct RuntimeRecord {
    PackPipeline pipeline = {};  // vs 0, fs 1: programs[]; blob and keyString: below
    std::string keyString;
    CapturedProgram programs[2];
    std::vector<uint32_t> blob;  // (method, value) pairs over the pack's varying registers
    // Registers outside them the state sets (its draws set them), and what every pack state leaves there (the draws after its
    // set them back)
    std::vector<std::pair<uint32_t, uint32_t>> extras, restore;
    std::vector<std::array<uint32_t, 8>> samplers;  // constexpr sampler headers the pack lacks, numbered after its own
};

// One state's probe: its submissions with tagged payloads of 256, 16 and 4096 bytes, the index its payloads are tagged with, and
// its key; `bound`: with 256-byte payloads each bound at its slot's own offset (pack/probe_bind.h), whose push words name each
// buffer slot's SlotBindOffset residual (none: the record takes only offsets on m2v's base alignment)
struct ProbeRuns {
    const nvdec::ProbeSubmission *bytes256, *bytes16, *bytes4096;
    uint32_t index;
    std::string key;
    const nvdec::ProbeSubmission *bound = nullptr;
};

using ProgramLookup = std::function<const CapturedProgram *(const std::string &id)>;

// False: the state is not one the raw path draws from a record (`why`, as mkpack.py sets one aside)
bool makeRecord(const ProbeRuns &runs, const PackRules &rules, const ProgramLookup &program, RuntimeRecord &out,
                std::string &why);

// The hash a state's record is found by (packfile.py pipeline_bytes): its key's, or the raw path's own states' by their name
uint64_t packKeyHash(const std::string &key);

// A program's heap slot as the pack lays one out (mkpack.py lay_out_programs) and its PackProgram, at heap offset 0
std::vector<uint8_t> programSlot(const CapturedProgram &p, PackProgram &info);

}  // namespace raw
