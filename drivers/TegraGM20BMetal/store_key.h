// SPDX-License-Identifier: MIT
// A request's key in the system shader compiler's store (compiler/compiler_store.h): what its record is made from and
// nothing that says where the request came from. Each stage's function is its name, its AIR (the HASH tag Apple's compiler
// wrote into the metallib's function list), what the list and the public metadata say of the function outside its
// bitcode, and the constants it was made with; every library's path or bytes are left out. A system
// library's records are every process's: one shader of a framework is one entry. Tags of any other library are the
// client's word, unchecked, so its records are filed under the asking process's scope (StoreScope) and reach no other.
// A linked function keeps the name it was made under, which its caller links by. A render state adds the descriptor's fields as its key names them and its variant; a
// compute request its CacheKey's text (its AIR, constants, stage input, linked functions, limits and the client's
// binding text, which its record is made by) and a kernel its dispatch.
//
// The entry's head holds `content` and every reader compares it whole (gm20b_cache.cpp readEntryHead), so an entry is
// taken only by a request of the same content byte for byte. `name` only routes a lookup: a fixed-size combine of the
// content's words, one multiply for each 8 bytes and no digest. Two contents that meet in a name cost a compile, never a
// wrong record.
#pragma once
#include <vector>
#include "compile_request.h"
#include "metallib.h"

namespace gm20b {

struct StoreKey {
    CacheKey content;
    Digest name{};
};

// A request stage's function as the store keys it: its AIR, and what its library says of it outside the bitcode the AIR
// names (function_tags.h listedFacts)
struct StageContent {
    AirHash air{};
    std::string facts;
};

// The process a request is made for, as the kernel read it (TegraGM20BAbiCompilerScope): kind 0 is none
struct StoreScope {
    uint32_t kind = 0;
    std::string identity;
};

// Whether `r`'s record is filed under its asker's scope: it names a library the compiler does not read off the sealed
// system volume. Such a library's tags are the client's word alone, so its records reach no other process; a system
// library's records are every process's.
bool scoped(const CompileRequest &r);

// function_tags.h listedFacts of `name` in the AIR slice Metal loads of a metallib file's bytes, or of one at `path`
std::string metallibListedFacts(const uint8_t *file, size_t size, const std::string &name);
std::string metallibListedFacts(const std::string &path, const std::string &name);

// The name is a fold any client can steer onto another content's (each lane is invertible in one free word). So an
// entry is filed under whichever of kStoreCandidates names holds its content, else the first free one, else the last,
// and every reader probes them all, comparing heads. Steering costs compiles, never a wrong record. It can still evict:
// four contents steered onto one name replace its last candidate, and because the candidates are a fixed XOR, another
// name's last candidate can be this one's first.
constexpr uint32_t kStoreCandidates = 4;
// Candidate `k` of `name` (0: `name`)
Digest candidateName(const Digest &name, uint32_t k);

// `k`: the request's CacheKey as the compiler makes it (compiler/compiler_request.h prepare); `stages[i]`: r.stages[i]'s
// function; `scope`: the asker's, which a scoped request is filed under
StoreKey storeKeyOf(const CacheKey &k, const CompileRequest &r, const std::vector<StageContent> &stages,
                    const StoreScope &scope);

// The store's name of a program slot's bytes (record.h programSlot), apart from every record's name: its size and a fixed
// number of its words, its first 256 bytes (its header and first instructions) and words spread over the rest. Like a
// record's name it only routes: whoever runs a slot found under it compares the bytes whole first (remote.cpp findSlot),
// and the compiler leaves out a slot whose name another holds (compiler_store.cpp putProgramSlots).
Digest programSlotName(const void *slot, size_t size);

// A render record a reader took from the store, given the key hashes of its own key (record.h packKeyHash of `key`, the
// reader's CacheKey text, and its stride twin's). An entry another process made carries that process's: alike-named
// states of two processes would otherwise share a hash in a reader that has both (Device::addRecord files by it).
inline void rekeyRecord(RuntimeRecord &record, const std::string &key)
{
    record.pipeline.keyHash = packKeyHash(key);
    if (!record.strideTwin)
        return;
    auto twin = std::make_shared<RuntimeRecord>(*record.strideTwin);
    twin->pipeline.keyHash = packKeyHash(strideTwinKey(key));
    record.pipeline.strideTwin = twin->pipeline.keyHash;
    record.strideTwin = std::move(twin);
}

}  // namespace gm20b
