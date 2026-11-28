// SPDX-License-Identifier: MIT
// What a state's full form (RequestForm::Full) holds the same of as the record its state was placed from
// (Device::attachFullForm): everything but the variants and split it adds. Its variants' assumptions index the full
// record's uniform ranges and its split binds the full record's entries, while draws read the placed record's.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "record.h"

namespace gm20b {

struct RecordLayout {
    PackPipeline pipeline = {};  // its variants as none
    std::vector<uint32_t> blob, far, spill;
    std::vector<std::pair<uint32_t, uint32_t>> extras;
    std::vector<std::array<uint32_t, 8>> samplers;
    std::string programKeys[2];  // programSlotKey: what makes two of a program's heap slots one
};

inline RecordLayout layoutOf(const RuntimeRecord &r)
{
    RecordLayout l;
    l.pipeline = r.pipeline;
    l.pipeline.variants = 0;
    l.pipeline.variantCount[0] = l.pipeline.variantCount[1] = 0;
    l.blob = r.blob;
    l.far = r.far;
    l.spill = r.spill;
    l.extras = r.extras;
    l.samplers = r.samplers;
    for (uint32_t k = 0; k < 2; k++)
        l.programKeys[k] = programSlotKey(r.programs[k]);
    return l;
}

// The part of `full`'s layout that is not `placed`'s; null where none is. PackPipeline has no padding (pack.h's layout
// asserts), so its bytes are its fields.
inline const char *layoutDifference(const RecordLayout &placed, const RecordLayout &full)
{
    if (memcmp(&placed.pipeline, &full.pipeline, sizeof placed.pipeline))
        return "pipeline record";
    if (placed.blob != full.blob)
        return "register list";
    if (placed.extras != full.extras)
        return "extra register list";
    if (placed.samplers != full.samplers)
        return "constexpr sampler list";
    if (placed.far != full.far)
        return "far block";
    if (placed.spill != full.spill)
        return "argument table";
    if (placed.programKeys[0] != full.programKeys[0] || placed.programKeys[1] != full.programKeys[1])
        return "program pair";
    return nullptr;
}

}  // namespace gm20b
