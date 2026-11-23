// SPDX-License-Identifier: MIT
// A library the system's Metal front end made, as the system shader compiler stores it (compile_request.h
// RequestKind::Library): the store entry head (cache_io.h), then the front end's warnings and the metallib, or the
// front end's error log. The compiler makes a refused one again at every ask (compiler_service.mm storedResult): a
// refusal is kept only for the asker to read.
#pragma once
#include <cstdint>
#include <string>
#include "gm20b_cache.h"
#include "cache_io.h"
#include "compute_facts_entry.h"
#include "pack.h"

namespace gm20b {

constexpr uint32_t kKindLibrary = 7, kKindLibraryRefusal = 8;
static_assert(kKindLibrary > kKindFactsRefusal, "an entry kind of its own");

struct MadeLibrary {
    std::string metallib, warnings;
};

inline std::string encodeLibraryEntry(const CacheKey &k, const MadeLibrary *made, const std::string &refusal)
{
    Writer w;
    writeEntryHead(w, k, made ? kKindLibrary : kKindLibraryRefusal);
    if (made) {
        w.text(made->warnings);
        w.text(made->metallib);
    } else {
        w.text(refusal);
    }
    w.value(Pack::fnv(w.out.data(), w.out.size()));
    return w.out;
}

inline RecordCache::Found decodeLibraryEntry(const std::string &bytes, const CacheKey &k, MadeLibrary &made,
                                             std::string &refusal)
{
    Reader r(bytes);
    uint32_t kind = 0;
    if (!readEntryHead(bytes, r, k, kind))
        return RecordCache::Found::None;
    if (kind == kKindLibraryRefusal) {
        refusal = r.text();
        return r.ok() && r.at() == bytes.size() - 8 ? RecordCache::Found::Refusal : RecordCache::Found::None;
    }
    if (kind != kKindLibrary)
        return RecordCache::Found::None;
    MadeLibrary read;
    read.warnings = r.text();
    read.metallib = r.text();
    if (!r.ok() || r.at() != bytes.size() - 8 || read.metallib.empty())
        return RecordCache::Found::None;
    made = std::move(read);
    return RecordCache::Found::Record;
}

}  // namespace gm20b
