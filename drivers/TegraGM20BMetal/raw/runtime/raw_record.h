// SPDX-License-Identifier: MIT
// A render state's raw record by pack/mkpack.py's rules, for a state the pack lacks: made directly (raw_direct.h) or from the
// pack generator's probe draws (makeRecord, test/recordcheck holds it to the pack), as the pack generator would have.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

// What making a state's or a kernel's record came to (Compiler::Outcome). Here and not in raw_compiler.h: the record makers
// include no part of the compile scheduler, which the raw stack key does not hash.
enum class CompileOutcome : uint8_t {
    Made,
    Refused,  // the state's own reason (kept on disk: the same probe would refuse it again)
    Failed    // the probe could not run (not kept: a later process may)
};

// A program as a record holds it
struct CapturedProgram {
    std::string id;  // nvdec's name: the FNV-1a of its header, code and data
    // header + code, NAK's shader data, each constant buffer's nvk_cbuf_type
    std::vector<uint8_t> image, data, cbufTypes;
    uint8_t gprs = 0, boundTextures = 0;  // PackProgram's
};

// What every record of a pack shares
struct PackRules {
    std::set<uint32_t> varying;  // the registers the pack's states set apart (their blobs')
    std::map<uint32_t, uint32_t> baseline;
    std::map<uint32_t, uint32_t> context;  // NVK's 3D registers after its context init that are not 0

    static PackRules of(const Pack &pack, std::map<uint32_t, uint32_t> context);
    // The context the pack's init template leaves: its 3D methods that take no address
    static PackRules of(const Pack &pack);
};

struct RuntimeRecord {
    PackPipeline pipeline = {};  // vs 0, fs 1: programs[]; blob and keyString: below
    std::string keyString;
    CapturedProgram programs[2];
    CapturedProgram tessPrograms[2];  // a tessellation state's (PackPipeline tessellates): control, evaluation
    std::vector<uint32_t> blob;       // (method, value) pairs over the pack's varying registers
    // Registers outside them the state sets (its draws set them), and what every pack state leaves there (the draws after its
    // set them back)
    std::vector<std::pair<uint32_t, uint32_t>> extras, restore;
    // Its constexpr sampler headers. A constexpr entry's sampler byte indexes them, whichever tool made the record (the
    // runtime, the native seed, the pack generator), so a record does not depend on the pack's sampler order.
    std::vector<std::array<uint32_t, 8>> samplers;
    // An amplifying state's push words + 1 of m2v's AmplificationLayerOffset and AmplificationViewportOffset (0: none),
    // which each amplification writes: a runtime state's, outside the pack's PackPipeline
    uint8_t amplificationLayerWord = 0, amplificationViewportWord = 0;
    // its fragment program's split (raw_pack.h PackSplit, whose program indices it does not use): the per-pixel program,
    // then the prologue (an 80-byte header of zeros before its code, as programs.bin holds a kernel's). splitTried: the
    // split was compiled for, and is here where it saves time
    bool splitTried = false;
    bool split = false;
    PackSplit splitRecord = {};
    CapturedProgram splitPrograms[2];

    // pipeline.variantCount[0] vertex then pipeline.variantCount[1] fragment variants (raw_pack.h PackVariant)
    struct Variant {
        uint32_t assume = 0;
        CapturedProgram program;
    };

    std::vector<Variant> variants;
    // its argument tables' words (PackPipeline::spillWords), the vertex table's first: in a pack, its blob section's
    std::vector<uint32_t> spill;
    // A state of bind-stride vertex streams: the same state with every per-bind layout pulled in the shader, made with it
    // at its creation (RawDirect.mm), which a draw of a bind stride past SET_VERTEX_STREAM_A_FORMAT's 12 bits takes. Its
    // keyHash is its own (strideTwinKey); it has no twin.
    std::shared_ptr<const RuntimeRecord> strideTwin;
};

// The key a stride twin of the state of key `key` is hashed by
std::string strideTwinKey(const std::string &key);

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

// The record's constexpr entries renumbered as the device's samplers: `place(header, index)` gives a header's index (false:
// no room). False when an entry names no header of the record, with `unknown` set, or when `place` fails.
template <typename Place>
bool placeConstexprSamplers(PackPipeline &pipeline, const std::vector<std::array<uint32_t, 8>> &headers, Place place,
                            bool &unknown)
{
    unknown = false;
    for (uint32_t k = 0; k < pipeline.entries && k < kPackEntries; k++) {
        PackEntry &e = pipeline.entry[k];
        if (e.kind == EntryUbo || e.samplerKind != SamplerConstexpr)
            continue;
        uint32_t index = 0;
        if (e.sampler >= headers.size()) {
            unknown = true;
            return false;
        }
        if (!place(headers[e.sampler], index))
            return false;
        e.sampler = uint8_t(index);
    }
    return true;
}

// The hash a state's record is found by (packfile.py pipeline_bytes): its key's, or the raw path's own states' by their name
uint64_t packKeyHash(const std::string &key);

// A program's heap slot as the pack lays one out (mkpack.py lay_out_programs) and its PackProgram, at heap offset 0
std::vector<uint8_t> programSlot(const CapturedProgram &p, PackProgram &info);

// The programs of a record that take heap slots of their own: its stages', each variant's and its split's per-pixel one
inline std::vector<const CapturedProgram *> slotPrograms(const RuntimeRecord &r)
{
    std::vector<const CapturedProgram *> programs = {&r.programs[0], &r.programs[1]};
    for (const RuntimeRecord::Variant &v : r.variants)
        programs.push_back(&v.program);
    if (r.split)
        programs.push_back(&r.splitPrograms[0]);
    return programs;
}

}  // namespace raw
