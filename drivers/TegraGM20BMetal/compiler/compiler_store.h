// SPDX-License-Identifier: MIT
// The system's record store as its one writer keeps it (kits/SwitchSeedPack/SwitchSeedStore.h):
// <root>/raw-<stack>-<rules>/store, named by the build's stack key and the installed pack's rules (a device-made record
// holds registers by the pack's varying set and baseline, which no stack key names), every other directory under <root>
// removed when it opens. Its entries are the raw runtime's own (raw_cache.h encodeEntry, encodeKernelEntry: a record or
// its refusal, checked against the full key and a checksum by every reader), each under its storeName. A full store is
// written again with its newest half (makeRoom), never refusing an entry.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "../raw/runtime/raw_compile_request.h"

namespace switch_compiler {

class Store {
public:
    static constexpr uint32_t kSlots = 16384;
    static constexpr uint64_t kDataBytes = uint64_t(256) << 20;

    // False with `error`: nothing can be kept (the compiler then serves no client)
    bool open(const std::string &root, uint64_t stack, const raw::Digest &rules, std::string &error);

    // The entry under `name`, copied
    bool find(const raw::Digest &name, std::string &bytes) const;
    // A damaged entry stepped over from now on, so it is made again
    void retire(const raw::Digest &name);
    enum class Put : uint8_t {
        Added,
        Full,  // makeRoom, then put again
        Failed
    };

    // Published for every reader (an entry already under `name` retired once this one is in)
    Put put(const raw::Digest &name, const std::string &entry);
    // Each program slot of a page or more that the record or kernel `entry` of `key` holds (raw_record.h programSlot,
    // raw_compute_slot.h computeSlot), published ahead of the entry as an entry of its own pages (raw_program_pages.h
    // programSlotEntryBytes) under its programSlotName: a client that takes the entry runs those slots in place from its
    // mapping of the store. A slot the store holds already is left out. Full: a slot found no room (makeRoom, then put
    // the slots again).
    Put putProgramSlots(const raw::CacheKey &key, const std::string &entry);
    // The store written again with its newest entries in half of it, renamed over the old file (which a client still
    // mapping keeps reading whole): its new mapping is to be registered again. False with `error`.
    bool makeRoom(std::string &error);

    const uint8_t *base() const
    {
        return writer_.base();
    }

    uint64_t mappedBytes() const
    {
        return writer_.mappedBytes();
    }

    std::string path() const
    {
        return path_;
    }

    uint32_t entries() const
    {
        std::lock_guard<std::mutex> l(lock_);
        return writer_.used();
    }

private:
    mutable std::mutex lock_;
    switch_seed_pack::StoreWriter writer_;
    std::string path_;
    uint8_t identity_[switch_seed_pack::kIdentityBytes] = {};
};

// The store's directory name under its root for a stack and pack rules
std::string storeDirectory(uint64_t stack, const raw::Digest &rules);

}  // namespace switch_compiler
