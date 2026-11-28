// SPDX-License-Identifier: MIT
// The libraries every request kind of the engine's (compiler_engine.h) is made from, made by the translator alone
// (TegraGM20BMetalOffline.h). A system library is read once per process: its file is mapped and read in place, its
// functions' HASH tags taken as the sealed volume states them, so a request reads its function list and the bitcode
// it compiles, and nothing of it is copied or hashed (an MPS library is 84 MB). A library of a client's data is kept
// while compiles come, so a burst of one library's states reads it once, and let go once none ran for a while. One a
// client registered is found by its handle; one a request carries is found by its bytes, compared, never hashed, and
// its kept bytes are one heap copy of the request's, which a compiler at rest has no use for. A registered library is
// read in its client's translator scope, kept past idle releases so a library the client registers again or that was
// let go finds its earlier translations; no other client's compile takes them. A library a request carries is read in a
// scope of its own.
//
// The pool's workers share it by one rule: an asker finds or adds its library's slot under `lock_`, then holds that
// slot's `making` while it reads the library, so the first asker reads it and a concurrent or later asker of the same
// library waits on `making` and takes the same object; a library the translator refused is not kept (the next asker
// reads it again). A kept library is never written again and lives while any compile holds it, past its slot's
// eviction or release. `lock_` is never held across a read or a wait, and a worker holds one slot at a time.
#pragma once
#import <Metal/Metal.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <cstring>
#include <string>
#include <string_view>
#include "../compile_request.h"
#include "compiler_client_libraries.h"

namespace switch_compiler {

class LibraryCache : public std::enable_shared_from_this<LibraryCache> {
public:
    // `root`: where the system volumes are (SystemLibraries)
    explicit LibraryCache(std::string root) : root_(std::move(root))
    {}

    // A compile begins and ends: the libraries are let go `kKeptFor` after the last ends, unless another begins
    void began();
    void ended();
    // The library `l` names, nil with `why`
    id<MTLLibrary> library(const gm20b::RequestLibrary &l, std::string &why,
                           const std::shared_ptr<const MappedClientLibrary> &mapped = nullptr);

private:
    static constexpr int64_t kKeptForNs = 5 * int64_t(NSEC_PER_SEC);
    static constexpr size_t kDataLibraries = 16;
    static constexpr size_t kClientScopes = 1024;

    // One library's read: `making` is held for it (a read of an MPS library's function list takes milliseconds)
    struct Slot {
        std::mutex making;
        id<MTLLibrary> made = nil;
    };

    // A data library: a registered one by its client's handle (no bytes), a request's own by its bytes
    struct DataKey {
        uint64_t client = 0, id = 0;
        std::shared_ptr<const std::string> bytes;
    };

    struct DataView {
        uint64_t client = 0, id = 0;
        std::string_view bytes;
    };

    struct DataOrder {
        using is_transparent = void;

        static DataView view(const DataKey &k)
        {
            return {k.client, k.id, k.bytes ? std::string_view(*k.bytes) : std::string_view()};
        }

        static DataView view(const DataView &v)
        {
            return v;
        }

        template <class A, class B> bool operator()(const A &a, const B &b) const
        {
            const DataView x = view(a), y = view(b);
            if (x.client != y.client || x.id != y.id)
                return x.client != y.client ? x.client < y.client : x.id < y.id;
            if (x.bytes.size() != y.bytes.size())
                return x.bytes.size() < y.bytes.size();
            return !x.bytes.empty() && memcmp(x.bytes.data(), y.bytes.data(), x.bytes.size()) < 0;
        }
    };

    const std::string root_;
    std::mutex lock_;
    uint32_t running_ = 0;
    uint64_t generation_ = 0;  // compiles begun, which a pending release checks
    std::map<std::string, std::shared_ptr<Slot>> system_;
    std::map<DataKey, std::shared_ptr<Slot>, DataOrder> data_;
    std::deque<DataKey> dataOrder_;
    std::map<uint64_t, uint64_t> scopes_;
    std::deque<uint64_t> scopeOrder_;

    uint64_t clientScopeLocked(uint64_t client);
    void release(uint64_t generation);
};

}  // namespace switch_compiler
