// SPDX-License-Identifier: MIT
// Where a library of the facade came from, as a compile request names it (raw_compile_request.h): a system volume's by its
// path, any other by its bytes, hashed once at the first request that needs it. The bytes are the AIR slice Metal loads
// (MetallibSlice.h), the facade's own where it read the metallib itself, else the m2v library's.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include "raw_compile_request.h"

namespace raw {

struct LibrarySource {
    explicit LibrarySource(std::string from, const uint8_t *slice = nullptr, size_t sliceBytes = 0,
                           std::shared_ptr<const void> keep = {})
        : path(std::move(from)), system(systemLibraryPath(path)), bytes(slice), size(sliceBytes), keep_(std::move(keep))
    {}

    const std::string path;  // "": made from data, or from source
    const bool system;
    // The AIR slice as the facade read it (a file's read-only mapping or the caller's data, held as long as this);
    // null: the library is m2v's (made from source, stitched, Core Image's linking)
    const uint8_t *const bytes;
    const size_t size;

    const LibraryIdentity &identity(const void *from, size_t fromBytes)
    {
        std::call_once(once_, [&] {
            identity_.system = system;
            identity_.digest = system ? sha256(path.data(), path.size()) : sha256(from, fromBytes);
        });
        return identity_;
    }

private:
    const std::shared_ptr<const void> keep_;
    std::once_flag once_;
    LibraryIdentity identity_;
};

}  // namespace raw
