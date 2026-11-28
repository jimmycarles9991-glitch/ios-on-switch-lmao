// SPDX-License-Identifier: MIT
// Where a library of the facade came from, as a compile request names it (compile_request.h): a system volume's by its
// path, any other by its bytes, hashed once at the first request that needs it. The bytes are the AIR slice Metal loads
// (MetallibSlice.h), as the facade read it.
#pragma once
#include <cstdint>
#include <memory>
#include <atomic>
#include <string>
#include "compile_request.h"

namespace gm20b {

struct LibrarySource {
    explicit LibrarySource(std::string from, const uint8_t *slice = nullptr, size_t sliceBytes = 0,
                           std::shared_ptr<const void> keep = {})
        : path(std::move(from)), system(systemLibraryPath(path)), bytes(slice), size(sliceBytes), keep_(std::move(keep))
    {}

    const std::string path;  // "": made from data, or from source
    const bool system;
    // The AIR slice as the facade read it (a file's read-only mapping, the caller's data or the shader compiler's reply,
    // held as long as this)
    const uint8_t *const bytes;
    const size_t size;

    // Never another source's in this process: what a registered library is cached by (remote.h registerLibrary), where
    // a freed library's address may hold another
    const uint64_t id = nextId();

private:
    const std::shared_ptr<const void> keep_;

    static uint64_t nextId()
    {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
};

}  // namespace gm20b
