// SPDX-License-Identifier: MIT
// See compiler_libraries.h.
#import "compiler_libraries.h"

#include <dispatch/dispatch.h>
#include "NativeBridgeMetalFrontend.h"
#include "compiler_request.h"

namespace switch_compiler {

void LibraryCache::began()
{
    std::lock_guard<std::mutex> l(lock_);
    running_++;
    generation_++;
}

void LibraryCache::ended()
{
    std::lock_guard<std::mutex> l(lock_);
    if (--running_)
        return;
    const uint64_t generation = generation_;
    const std::weak_ptr<LibraryCache> cache = weak_from_this();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kKeptForNs), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if (const std::shared_ptr<LibraryCache> kept = cache.lock())
            kept->release(generation);
    });
}

void LibraryCache::release(uint64_t generation)
{
    std::map<std::string, id<MTLLibrary>> system;
    std::map<raw::Digest, id<MTLLibrary>> data;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (running_ || generation != generation_)
            return;
        system.swap(system_);
        data.swap(data_);
        dataOrder_.clear();
    }
}

id<MTLLibrary> LibraryCache::library(const raw::RequestLibrary &l, std::string &why)
{
    std::string real;
    if (l.system && !SystemLibraries::resolve(root_, l.path, real, why))
        return nil;
    const raw::Digest digest = l.system ? raw::Digest{} : raw::sha256(l.bytes.data(), l.bytes.size());
    {
        std::lock_guard<std::mutex> g(lock_);
        if (l.system) {
            if (const auto found = system_.find(real); found != system_.end())
                return found->second;
        } else if (const auto found = data_.find(digest); found != data_.end()) {
            return found->second;
        }
    }
    NSError *error = nil;
    id<MTLLibrary> made = nil;
    if (l.system) {
        made = native_bridge_metal_compile_library(real.c_str(), nil, &error);
    } else {
        dispatch_data_t bytes =
            dispatch_data_create(l.bytes.data(), l.bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        made = native_bridge_metal_compile_library(nullptr, bytes, &error);
    }
    if (!made) {
        why = std::string("m2v refused the library ") + (l.path.empty() ? "of its data" : l.path) +
              (error ? std::string(": ") + error.localizedDescription.UTF8String : "");
        return nil;
    }
    std::lock_guard<std::mutex> g(lock_);
    if (l.system) {
        system_[real] = made;
    } else if (data_.emplace(digest, made).second) {
        dataOrder_.push_back(digest);
        if (dataOrder_.size() > kDataLibraries) {
            data_.erase(dataOrder_.front());
            dataOrder_.pop_front();
        }
    }
    return made;
}

}  // namespace switch_compiler
