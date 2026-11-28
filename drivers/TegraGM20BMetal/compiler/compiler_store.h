// SPDX-License-Identifier: MIT
// The system's record store as its one writer keeps it (kits/SwitchSeedPack/SwitchSeedStore.h):
// <root>/raw-<stack>-<rules>/store, named by the build's stack key and the installed pack's rules (a device-made record
// holds registers by the pack's varying set and baseline, which no stack key names), every other directory under <root>
// removed when it opens. Its entries are the runtime's own (gm20b_cache.h encodeEntry, encodeKernelEntry: a record or
// its refusal, checked against the full key by every reader), each under its storeName. A full store is written again
// with its newest half (makeRoom), never refusing an entry. persist() makes what was published durable for a later boot.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "../compile_request.h"

namespace switch_compiler {

class Store {
public:
    static constexpr uint32_t kSlots = 16384;
    static constexpr uint64_t kDataBytes = uint64_t(256) << 20;

    // False with `error`: nothing can be kept (the compiler then serves no client)
    bool open(const std::string &root, uint64_t stack, const gm20b::Digest &rules, std::string &error);

    // The entry under `name`, copied
    bool find(const gm20b::Digest &name, std::string &bytes) const;
    // The entry of content `key` under any of `name`'s candidates (store_key.h candidateName), copied, with `at` its
    // name. False with `at` where a put of `key` would file it now: the first free candidate, else the last.
    bool find(const gm20b::Digest &name, const gm20b::CacheKey &key, std::string &bytes, gm20b::Digest &at) const;
    // A damaged entry stepped over from now on, so it is made again
    void retire(const gm20b::Digest &name);
    // The entry under `name` retired only if it is of content `key`, checked and retired under one lock: a worker may
    // have filed another content there since the caller looked
    void retire(const gm20b::Digest &name, const gm20b::CacheKey &key);
    enum class Put : uint8_t {
        Added,
        Full,  // makeRoom, then put again
        Failed,
        Kept  // the candidate's entry of the content was one `keep` keeps
    };

    // Published for every reader (an entry already under `name` retired once this one is in)
    Put put(const gm20b::Digest &name, const std::string &entry);
    // `entry` of content `key` under the candidate of `name` that find(name, key) gives, `at` the one taken. Chosen and
    // written under one lock, so two contents compiled at once under one name each keep an entry. `keep`: an entry of
    // `key` already there that it answers true for stays, decided under the same lock.
    Put put(const gm20b::Digest &name, const gm20b::CacheKey &key, const std::string &entry, gm20b::Digest &at,
            const std::function<bool(const std::string &held)> &keep = nullptr);
    // Each program slot of a page or more that the record or kernel `entry` of `key` holds (record.h programSlot,
    // compute_slot.h computeSlot), published ahead of the entry as an entry of its own pages (program_pages.h
    // programSlotEntryBytes) under its programSlotName: a client that takes the entry runs those slots in place from its
    // mapping of the store. A slot the store holds already is left out. Full: a slot found no room (makeRoom, then put
    // the slots again).
    Put putProgramSlots(const gm20b::CacheKey &key, const std::string &entry);
    // The store written again with its newest entries in half of it, renamed over the old file (which a client still
    // mapping keeps reading whole): its new mapping is to be registered again. False with `error`.
    bool makeRoom(std::string &error);
    // Every entry published so far synced, and the store marked so (SwitchSeedStore.h StoreWriter::persist). Its syncs
    // hold no lock a put or find takes; a makeRoom waits for one.
    bool persist();

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
    bool findViewLocked(const gm20b::Digest &name, const gm20b::CacheKey &key, std::string_view &bytes,
                        gm20b::Digest &at) const;
    bool holdsLocked(const gm20b::Digest &name, const gm20b::CacheKey &key) const;

    // persistLock_ then lock_: a persist syncs the file outside lock_, so the file it syncs must stay open meanwhile.
    // makeRoom (a full store, on a compile's publish) waits for at most one persist's msync and two barrier fsyncs,
    // a fraction of the half-store rewrite it then does itself.
    std::mutex persistLock_;
    mutable std::mutex lock_;
    switch_seed_pack::StoreWriter writer_;
    std::string path_;
    uint8_t identity_[switch_seed_pack::kIdentityBytes] = {};
    // This boot's id, read once at open (before the compiler's sandbox), for every later open of the file
    char boot_[switch_seed_pack::kStoreBootBytes] = {};
};

// The store's directory name under its root for a stack and pack rules
std::string storeDirectory(uint64_t stack, const gm20b::Digest &rules);

}  // namespace switch_compiler
