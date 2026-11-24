// SPDX-License-Identifier: MIT
// See compiler_libraries.h.
#import "compiler_libraries.h"

#include <dispatch/dispatch.h>
#include "compiler_request.h"
#include "../TegraGM20BMetalOffline.h"

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
    std::map<gm20b::Digest, std::shared_ptr<Slot>> data;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (running_ || generation != generation_)
            return;
        data.swap(data_);
        dataOrder_.clear();
    }
}

id<MTLLibrary> LibraryCache::library(const gm20b::RequestLibrary &l, std::string &why)
{
    std::string real;
    if (l.system && !SystemLibraries::resolve(root_, l.path, real, why))
        return nil;
    const gm20b::Digest digest = l.system ? gm20b::Digest{} : gm20b::sha256(l.bytes.data(), l.bytes.size());
    std::shared_ptr<Slot> slot;
    {
        std::lock_guard<std::mutex> g(lock_);
        std::shared_ptr<Slot> &kept = l.system ? system_[real] : data_[digest];
        if (!kept) {
            kept = std::make_shared<Slot>();
            if (!l.system) {
                dataOrder_.push_back(digest);
                if (dataOrder_.size() > kDataLibraries) {
                    data_.erase(dataOrder_.front());
                    dataOrder_.pop_front();
                }
            }
        }
        slot = kept;
    }
    std::lock_guard<std::mutex> making(slot->making);
    if (slot->made)
        return slot->made;
    NSError *error = nil;
    GM20BOfflineDevice *device = [GM20BOfflineDevice new];
    if (l.system) {
        slot->made = [device newSealedLibraryWithURL:[NSURL fileURLWithPath:@(real.c_str())] error:&error];
    } else {
        dispatch_data_t bytes =
            dispatch_data_create(l.bytes.data(), l.bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        slot->made = [device newLibraryWithData:bytes error:&error];
    }
    if (!slot->made)
        why = std::string("the translator refused the library ") + (l.path.empty() ? "of its data" : l.path) +
              (error ? std::string(": ") + error.localizedDescription.UTF8String : "");
    return slot->made;
}

}  // namespace switch_compiler
