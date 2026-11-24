// SPDX-License-Identifier: MIT
// The libraries every request kind of the engine's (compiler_engine.h) is made from, made by the translator alone
// (TegraGM20BMetalOffline.h). A system library is read once per process: its file is mapped and read in place, its
// functions' HASH tags taken as the sealed volume states them, so a request reads its function list and the bitcode
// it compiles, and nothing of it is copied or hashed (an MPS library is 84 MB). A library of a client's data is kept
// while compiles come, so a burst of one library's states reads it once, and let go once none ran for a while: its
// bytes are a heap copy of the request's, which a compiler at rest has no use for.
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
#include <string>
#include "../compile_request.h"

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
    id<MTLLibrary> library(const gm20b::RequestLibrary &l, std::string &why);

private:
    static constexpr int64_t kKeptForNs = 5 * int64_t(NSEC_PER_SEC);
    static constexpr size_t kDataLibraries = 16;

    // One library's read: `making` is held for it (a read of an MPS library's function list takes milliseconds)
    struct Slot {
        std::mutex making;
        id<MTLLibrary> made = nil;
    };

    const std::string root_;
    std::mutex lock_;
    uint32_t running_ = 0;
    uint64_t generation_ = 0;  // compiles begun, which a pending release checks
    std::map<std::string, std::shared_ptr<Slot>> system_;
    std::map<gm20b::Digest, std::shared_ptr<Slot>> data_;
    std::deque<gm20b::Digest> dataOrder_;

    void release(uint64_t generation);
};

}  // namespace switch_compiler
