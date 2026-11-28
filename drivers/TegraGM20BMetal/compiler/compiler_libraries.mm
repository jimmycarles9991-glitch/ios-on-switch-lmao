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
    std::map<DataKey, std::shared_ptr<Slot>, DataOrder> data;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (running_ || generation != generation_)
            return;
        data.swap(data_);
        dataOrder_.clear();
    }
}

uint64_t LibraryCache::clientScopeLocked(uint64_t client)
{
    if (!client)
        return nmb_offline_scope_new();
    uint64_t &own = scopes_[client];
    if (!own) {
        own = nmb_offline_scope_new();
        scopeOrder_.push_back(client);
        if (scopeOrder_.size() > kClientScopes) {
            scopes_.erase(scopeOrder_.front());
            scopeOrder_.pop_front();
        }
    }
    return own;
}

id<MTLLibrary> LibraryCache::library(const gm20b::RequestLibrary &l, std::string &why,
                                     const std::shared_ptr<const MappedClientLibrary> &mapped)
{
    std::string real;
    if (l.system && !SystemLibraries::resolve(root_, l.path, real, why))
        return nil;
    if (!l.system && l.libraryId && !mapped)
        return why = "a library handle with no mapping", nil;
    const DataView view = mapped ? DataView{mapped->client, mapped->id, {}} : DataView{0, 0, l.bytes};
    std::shared_ptr<Slot> slot;
    std::shared_ptr<const std::string> bytes;
    uint64_t scope = 0;
    {
        std::lock_guard<std::mutex> g(lock_);
        if (!l.system && mapped)
            scope = clientScopeLocked(mapped->client);
        std::shared_ptr<Slot> *kept = nullptr;
        if (l.system) {
            kept = &system_[real];
        } else if (const auto found = data_.find(view); found != data_.end()) {
            kept = &found->second;
            bytes = found->first.bytes;
        } else {
            DataKey key{view.client, view.id, mapped ? nullptr : std::make_shared<const std::string>(l.bytes)};
            bytes = key.bytes;
            kept = &data_[key];
            dataOrder_.push_back(std::move(key));
            if (dataOrder_.size() > kDataLibraries) {
                data_.erase(dataOrder_.front());
                dataOrder_.pop_front();
            }
        }
        if (!*kept)
            *kept = std::make_shared<Slot>();
        slot = *kept;
    }
    std::lock_guard<std::mutex> making(slot->making);
    if (slot->made)
        return slot->made;
    NSError *error = nil;
    GM20BOfflineDevice *device = [GM20BOfflineDevice new];
    if (l.system) {
        slot->made = [device newSealedLibraryWithURL:[NSURL fileURLWithPath:@(real.c_str())] error:&error];
    } else if (mapped) {
        std::shared_ptr<const MappedClientLibrary> keep = mapped;
        dispatch_data_t bytes =
            dispatch_data_create(mapped->data, mapped->size, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                (void)keep;
            });
        slot->made = [device newLibraryWithData:bytes scope:scope error:&error];
    } else {
        std::shared_ptr<const std::string> keep = bytes;
        dispatch_data_t data =
            dispatch_data_create(keep->data(), keep->size(), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                (void)keep;
            });
        slot->made = [device newLibraryWithData:data error:&error];
    }
    if (!slot->made)
        why = std::string("the translator refused the library ") + (l.path.empty() ? "of its data" : l.path) +
              (error ? std::string(": ") + error.localizedDescription.UTF8String : "");
    return slot->made;
}

}  // namespace switch_compiler
