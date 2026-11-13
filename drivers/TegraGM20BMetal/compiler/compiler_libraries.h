// SPDX-License-Identifier: MIT
// The libraries the engine's compiles are made from (compiler_engine.h), made over the frontend's compile context: kept
// while compiles come, so a burst of one library's states reads it once, and let go once none ran for a while. m2v's
// translator holds a copy of each library's AIR on the heap (metal2vulkan::library::Library), several MB for Core
// Animation's and RenderBox's, which a compiler at rest has no use for.
#pragma once
#import <Metal/Metal.h>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "../raw/runtime/raw_compile_request.h"

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
    id<MTLLibrary> library(const raw::RequestLibrary &l, std::string &why);

private:
    static constexpr int64_t kKeptForNs = 5 * int64_t(NSEC_PER_SEC);
    static constexpr size_t kDataLibraries = 16;

    const std::string root_;
    std::mutex lock_;
    uint32_t running_ = 0;
    uint64_t generation_ = 0;  // compiles begun, which a pending release checks
    std::map<std::string, id<MTLLibrary>> system_;
    std::map<raw::Digest, id<MTLLibrary>> data_;
    std::deque<raw::Digest> dataOrder_;

    void release(uint64_t generation);
};

}  // namespace switch_compiler
