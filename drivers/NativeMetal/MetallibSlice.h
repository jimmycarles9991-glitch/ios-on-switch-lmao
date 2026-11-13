// SPDX-License-Identifier: MIT
// The AIR slice of a universal metallib (big-endian fat header: slices of cputype AIR64 and of Apple GPUs), as the selected
// IPSW kind's Metal picks it in MTLLibraryDataWithArchive::parseArchiveSync. An AIR64 slice's subtype is its AIR version
// (Metal's mapAirVersionToMachOSubType: AIR 2.4 is 8, AIR 2.9 is 0xd); 24B119's CoreImage.metallib holds both.
//   21H461: the first AIR64 slice.
//   24B119: shouldPickCandidateAirOverCurrentAir over the AIR64 slices in file order. A device below featureProfile 8 (the
//           identity's: J181's A13 is 7) takes the newest slice older than AIR 2.9, else the first of AIR 2.9; a newer one the
//           newest. No slice above the AIR ceiling of MTLCompiler 32023 (2.9, compared as Metal compares it, with the subtype).
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include "MetalABI.h"

namespace switch_metal {

struct AirSliceRule {
    std::string_view kind;
    bool first;
    uint32_t ceiling;
    bool belowProfile8;
    uint32_t air29;
};

inline constexpr AirSliceRule kAirSliceRules[] = {
    {"21H461", true, 0, false, 0},
    {"24B119", false, 0x20009, true, 0xd},
};

inline constexpr size_t airSliceRuleIndex()
{
    for (size_t i = 0; i < std::size(kAirSliceRules); i++)
        if (kAirSliceRules[i].kind == SWITCH_METAL_KIND)
            return i;
    return std::size(kAirSliceRules);
}

static_assert(airSliceRuleIndex() < std::size(kAirSliceRules), "no AIR slice rule for this kind (MetallibSlice.h)");
inline constexpr const AirSliceRule &kAirSliceRule = kAirSliceRules[airSliceRuleIndex()];

constexpr uint32_t kAir64CpuType = 0x01000017;

inline bool pickAirSlice(const AirSliceRule &rule, bool have, uint32_t current, uint32_t candidate)
{
    if (candidate > rule.ceiling)
        return false;
    if (!rule.belowProfile8)
        return !have || candidate > current;
    if (!have)
        return candidate <= rule.air29;
    const bool currentOlder = current < rule.air29, candidateOlder = candidate < rule.air29;
    return candidateOlder && (!currentOlder || candidate > current);
}

// [offset, offset + size) of `file`: the slice `rule`'s Metal loads, the whole file when it is thin. False: a universal file
// with no AIR64 slice that Metal takes.
inline bool airSlice(const uint8_t *file, size_t length, size_t &offset, size_t &size,
                     const AirSliceRule &rule = kAirSliceRule)
{
    const auto word = [&](size_t at) {
        return uint32_t(file[at]) << 24 | uint32_t(file[at + 1]) << 16 | uint32_t(file[at + 2]) << 8 | file[at + 3];
    };
    if (!file || length < 8 || word(0) != 0xcafebabe) {
        offset = 0;
        size = length;
        return file != nullptr;
    }
    bool have = false;
    uint32_t current = 0;
    for (uint32_t i = 0, count = word(4); i < count && 8 + 20 * size_t(i + 1) <= length; i++) {
        const size_t arch = 8 + 20 * size_t(i);
        if (word(arch) != kAir64CpuType)
            continue;
        const uint32_t subtype = word(arch + 4);
        if (!rule.first && !pickAirSlice(rule, have, current, subtype))
            continue;
        have = true;
        current = subtype;
        offset = word(arch + 8);
        size = word(arch + 12);
        if (rule.first)
            break;
    }
    return have && offset <= length && size <= length - offset;
}

}  // namespace switch_metal
