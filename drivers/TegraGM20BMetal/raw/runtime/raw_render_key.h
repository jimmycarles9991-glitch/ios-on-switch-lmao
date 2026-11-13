// SPDX-License-Identifier: MIT
#pragma once
#include "raw_cache.h"
#include "raw_metallib.h"
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace raw {

struct LinkedStageAir {
    char stage;
    AirHash air;
};

inline CacheKey renderCacheKey(uint64_t stack, std::string key, const AirHash &vertex, const AirHash &fragment,
                               const std::vector<LinkedStageAir> &linked, bool own)
{
    CacheKey result;
    result.stack = stack;
    std::memcpy(result.vertexAir, vertex.data(), vertex.size());
    std::memcpy(result.fragmentAir, fragment.data(), fragment.size());
    // Linked functions can share an exported name while carrying different AIR.
    if (!linked.empty()) {
        key += " | linked";
        for (const auto &function : linked) {
            uint64_t hash;
            std::memcpy(&hash, function.air.data(), sizeof hash);
            char word[24];
            std::snprintf(word, sizeof word, " %c%016llx", function.stage, static_cast<unsigned long long>(hash));
            key += word;
        }
    }
    if (own) {
        uint64_t v, f;
        std::memcpy(&v, vertex.data(), sizeof v);
        std::memcpy(&f, fragment.data(), sizeof f);
        char tail[48];
        std::snprintf(tail, sizeof tail, " | air %016llx %016llx", static_cast<unsigned long long>(v),
                      static_cast<unsigned long long>(f));
        key += tail;
    }
    result.key = std::move(key);
    return result;
}

}  // namespace raw
