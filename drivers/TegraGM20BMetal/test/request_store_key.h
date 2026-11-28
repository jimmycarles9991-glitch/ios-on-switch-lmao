// SPDX-License-Identifier: MIT
// The store key a test's stand-in compiler files request `r` of CacheKey `k` under (store_key.h), each stage's AIR and facts
// read off its library as the system compiler reads them: a data library's bytes, a system library at its path; `scope`:
// the asker's, as the kernel model gives it (none unless a test sets one)
#pragma once
#include <map>
#include "../metallib.h"
#include "../store_key.h"

namespace harness {

inline gm20b::StoreKey requestStoreKey(const gm20b::CacheKey &k, const gm20b::CompileRequest &r,
                                       const gm20b::StoreScope &scope = {})
{
    std::vector<gm20b::StageContent> stages;
    for (const gm20b::RequestStage &s : r.stages) {
        std::map<std::string, gm20b::AirHash> hashes;
        const gm20b::RequestLibrary &l = r.libraries[s.library];
        const auto *bytes = reinterpret_cast<const uint8_t *>(l.bytes.data());
        gm20b::StageContent stage;
        if (l.system) {
            gm20b::metallibAirHashes(l.path, hashes);
            stage.facts = gm20b::metallibListedFacts(l.path, s.function);
        } else {
            gm20b::metallibAirHashes(bytes, l.bytes.size(), hashes);
            stage.facts = gm20b::metallibListedFacts(bytes, l.bytes.size(), s.function);
        }
        if (const auto found = hashes.find(s.function); found != hashes.end())
            stage.air = found->second;
        stages.push_back(std::move(stage));
    }
    return gm20b::storeKeyOf(k, r, stages, scope);
}

}  // namespace harness
