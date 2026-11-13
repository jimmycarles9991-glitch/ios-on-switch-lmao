// SPDX-License-Identifier: MIT
// A state's fetch-free form (fetch_free.h) made from its corpus line, as directpack.mm packs it and directcheck.mm dumps it.
// A form that still reads other colours it writes nothing of (a memoryless image its texture function reads: QuartzCore's
// M1 images) reads them behind a pixel barrier, as the state's own barrier twin does, beside the ROP's blend of its colour;
// a form that fetches otherwise is refused, so the state keeps its storage form.
#pragma once
#import <Metal/Metal.h>
#include <cstdio>
#include <map>
#include <string>
#include "../runtime/RawDirect.h"
#include "direct_states.h"
#include "fetch_free.h"

namespace probe {

enum class FreeOutcome : uint8_t {
    None,     // the line names no form
    Made,     // `rec` is the form's record
    Fetches,  // the form fetches its own colour, a colour it writes, or in the storage form
    Refused,  // the form's state did not compile (`why`)
};

// The colours a record's programs read through a fetch (raw_gpu.cpp Pipeline::fetchBits)
inline uint32_t fetchedColours(const raw::PackPipeline &p)
{
    uint32_t colours = 0;
    for (uint32_t k = 0; k < p.entries && k < raw::kPackEntries; k++)
        if (p.entry[k].kind == raw::EntryFetch && p.entry[k].slot < 8)
            colours |= 1u << p.entry[k].slot;
    return colours;
}

// Whether the line's state writes no channel of any colour of `colours` (" c<n>=<format>:<write mask>:...")
inline bool writesNone(const std::string &line, uint32_t colours)
{
    const std::string head = line.substr(0, line.find(" | "));
    for (uint32_t c = 0; c < 8; c++) {
        if (!(colours >> c & 1))
            continue;
        const size_t at = head.find(" c" + std::to_string(c) + "=");
        unsigned format = 0, mask = 1;
        if (at == std::string::npos || sscanf(head.c_str() + at + 4, "%u:%u:", &format, &mask) != 2 || mask)
            return false;
    }
    return true;
}

// `barrier`: the form reads its other colours behind a barrier (a barrier form's record)
inline FreeOutcome makeFetchFree(raw::Direct &direct, id<MTLLibrary> qc, const std::string &line,
                                 raw::ProbeTextures textures, FetchFree &form, raw::RuntimeRecord &rec,
                                 std::map<uint32_t, uint32_t> *registers, std::string &why, bool &barrier)
{
    State s;
    barrier = false;
    if (!fetchFree(line, form) || !parseState(qc, form.line, s) || form.colour >= 8)
        return FreeOutcome::None;
    // made under its own key, whose colour fields give its registers their blend, and filed under the state's
    raw::CompileOutcome outcome = direct.make(s.d, s.key, rec, why, textures, registers, true, raw::PackForm::Default);
    const auto readsOthersOnly = [&] {
        const uint32_t fetched = fetchedColours(rec.pipeline);
        return fetched && !(fetched >> form.colour & 1) && writesNone(form.line, fetched);
    };
    if (outcome != raw::CompileOutcome::Made)
        return FreeOutcome::Refused;
    if (!(rec.pipeline.flags & (raw::PipelineFetches | raw::PipelineBarrierForm)))
        return FreeOutcome::Made;
    if (!readsOthersOnly())
        return FreeOutcome::Fetches;
    barrier = true;
    outcome = direct.make(s.d, s.key, rec, why, textures, registers, true, raw::PackForm::Barrier);
    if (outcome != raw::CompileOutcome::Made)
        return FreeOutcome::Refused;
    return (rec.pipeline.flags & raw::PipelineStorageMetadata) || !readsOthersOnly() ? FreeOutcome::Fetches
                                                                                     : FreeOutcome::Made;
}

}  // namespace probe
