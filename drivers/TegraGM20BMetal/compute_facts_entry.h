// SPDX-License-Identifier: MIT
// A compute state's facts as the system shader compiler stores them (compile_request.h RequestKind::Facts): the store
// entry head (cache_io.h), then maxTotalThreadsPerThreadgroup, threadExecutionWidth and staticThreadgroupMemoryLength
// (pack.h PackComputeFacts), or why the compiler made none.
#pragma once
#include <cstdint>
#include <string>
#include "gm20b_cache.h"
#include "cache_io.h"
#include "pack.h"

namespace gm20b {

constexpr uint32_t kKindFacts = 5, kKindFactsRefusal = 6;

inline std::string encodeFactsEntry(const CacheKey &k, const uint32_t *facts, const std::string &refusal)
{
    Writer w;
    writeEntryHead(w, k, facts ? kKindFacts : kKindFactsRefusal);
    if (facts)
        for (int i = 0; i < 3; i++)
            w.value(facts[i]);
    else
        w.text(refusal);
    return w.out;
}

inline RecordCache::Found decodeFactsEntry(std::string_view bytes, const CacheKey &k, PackComputeFacts &facts,
                                           std::string &refusal)
{
    Reader r(bytes);
    uint32_t kind = 0;
    if (!readEntryHead(bytes, r, k, kind))
        return RecordCache::Found::None;
    if (kind == kKindFactsRefusal) {
        refusal = r.text();
        return r.ok() && r.at() == bytes.size() ? RecordCache::Found::Refusal : RecordCache::Found::None;
    }
    if (kind != kKindFacts)
        return RecordCache::Found::None;
    PackComputeFacts made = {};
    made.keyHash = Pack::fnv(k.key.data(), k.key.size());
    made.maxThreads = r.value<uint32_t>();
    made.executionWidth = r.value<uint32_t>();
    made.staticMemory = r.value<uint32_t>();
    if (!r.ok() || r.at() != bytes.size())
        return RecordCache::Found::None;
    facts = made;
    return RecordCache::Found::Record;
}

}  // namespace gm20b
