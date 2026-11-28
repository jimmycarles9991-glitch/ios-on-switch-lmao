// SPDX-License-Identifier: MIT
#pragma once
#include <os/log.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "../compile_request.h"
#include "../metallib.h"

namespace switch_compiler {

struct MappedClientLibrary {
    uint64_t client = 0;
    uint64_t id = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
    std::map<std::string, gm20b::AirHash> air;
};

// Maps each client-registered library handle read-only once and parses its AIR function hashes once, and keeps
// mappings LRU-bounded while safely deferring UnmapLibrary until every in-flight compile and cached MTLLibrary
// holding the mapping drops its reference. A mapping is the client's own (client, handle), and nothing made from it is
// shared beyond it, so its bytes are never hashed to check what the client says of them.
class ClientLibraries {
public:
    static constexpr size_t kDefaultMaxEntries = 64;
    static constexpr size_t kDefaultMaxBytes = 512u << 20;

    using MapFn =
        std::function<bool(uint64_t client, uint64_t id, const uint8_t *&data, size_t &size, std::string &why)>;
    using UnmapFn = std::function<void(uint64_t client, uint64_t id)>;

    ClientLibraries(MapFn map, UnmapFn unmap, size_t maxEntries = kDefaultMaxEntries,
                    size_t maxBytes = kDefaultMaxBytes)
        : state_(std::make_shared<State>(std::move(map), std::move(unmap), maxEntries, maxBytes))
    {}

    ~ClientLibraries()
    {
        trim();
    }

    ClientLibraries(const ClientLibraries &) = delete;
    ClientLibraries &operator=(const ClientLibraries &) = delete;

    // The digest a request names its library by goes unread
    bool resolve(uint64_t client, uint64_t id, const gm20b::Digest & /*named*/,
                 std::shared_ptr<const MappedClientLibrary> &out, std::string &why)
    {
        if (!client || !id) {
            why = "an invalid client library handle";
            return false;
        }
        const Key key{client, id};
        std::shared_ptr<const MappedClientLibrary> hit;
        {
            std::unique_lock<std::mutex> l(state_->lock);
            for (;;) {
                const auto lruIt = state_->lruIndex.find(key);
                if (lruIt != state_->lruIndex.end()) {
                    state_->lru.splice(state_->lru.begin(), state_->lru, lruIt->second);
                    hit = lruIt->second->second;
                    break;
                }
                const auto liveIt = state_->live.find(key);
                if (liveIt != state_->live.end()) {
                    hit = liveIt->second.lock();
                    if (hit)
                        break;
                    // Deleter is unmapping this (client, id); wait so a fresh map cannot race it.
                    state_->cv.wait(l);
                    continue;
                }
                if (state_->inflight.count(key)) {
                    state_->cv.wait(l);
                    continue;
                }
                state_->inflight.insert(key);
                break;
            }
        }

        if (hit) {
            std::vector<std::shared_ptr<const MappedClientLibrary>> evicted;
            {
                std::lock_guard<std::mutex> l(state_->lock);
                promoteLocked(key, hit, evicted);
            }
            out = std::move(hit);
            return true;
        }

        const auto cancelInflight = [&] {
            std::lock_guard<std::mutex> l(state_->lock);
            state_->inflight.erase(key);
            state_->cv.notify_all();
        };

        const uint8_t *data = nullptr;
        size_t size = 0;
        if (!state_->map(client, id, data, size, why) || !data || !size) {
            cancelInflight();
            if (why.empty())
                why = "client library map failed";
            return false;
        }

        std::map<std::string, gm20b::AirHash> air;
        if (!gm20b::metallibAirHashes(data, size, air)) {
            state_->unmap(client, id);
            cancelInflight();
            why = "a library that is not a metallib";
            return false;
        }

        auto *raw = new MappedClientLibrary{client, id, data, size, std::move(air)};
        std::shared_ptr<State> state = state_;
        std::shared_ptr<const MappedClientLibrary> made(raw, [state, key](const MappedClientLibrary *p) {
            delete p;
            state->unmap(key.first, key.second);
            {
                std::lock_guard<std::mutex> l(state->lock);
                state->live.erase(key);
            }
            state->cv.notify_all();
        });

        std::vector<std::shared_ptr<const MappedClientLibrary>> evicted;
        {
            std::lock_guard<std::mutex> l(state_->lock);
            state_->live[key] = made;
            state_->inflight.erase(key);
            promoteLocked(key, made, evicted);
            state_->cv.notify_all();
        }
        out = std::move(made);
        return true;
    }

    void trim()
    {
        std::vector<std::shared_ptr<const MappedClientLibrary>> dropped;
        {
            std::lock_guard<std::mutex> l(state_->lock);
            dropped.reserve(state_->lru.size());
            for (auto &entry : state_->lru)
                dropped.push_back(std::move(entry.second));
            state_->lru.clear();
            state_->lruIndex.clear();
            state_->lruBytes = 0;
        }
    }

private:
    using Key = std::pair<uint64_t, uint64_t>;
    using LruList = std::list<std::pair<Key, std::shared_ptr<const MappedClientLibrary>>>;

    struct State {
        State(MapFn m, UnmapFn u, size_t maxE, size_t maxB)
            : map(std::move(m)), unmap(std::move(u)), maxEntries(maxE), maxBytes(maxB)
        {}

        const MapFn map;
        const UnmapFn unmap;
        const size_t maxEntries;
        const size_t maxBytes;
        std::mutex lock;
        std::condition_variable cv;
        std::map<Key, std::weak_ptr<const MappedClientLibrary>> live;
        std::set<Key> inflight;
        LruList lru;
        std::map<Key, LruList::iterator> lruIndex;
        size_t lruBytes = 0;
    };

    std::shared_ptr<State> state_;

    void promoteLocked(const Key &key, const std::shared_ptr<const MappedClientLibrary> &entry,
                       std::vector<std::shared_ptr<const MappedClientLibrary>> &evicted)
    {
        const auto existing = state_->lruIndex.find(key);
        if (existing != state_->lruIndex.end()) {
            state_->lru.splice(state_->lru.begin(), state_->lru, existing->second);
            return;
        }
        if (!state_->maxEntries || entry->size > state_->maxBytes)
            return;
        state_->lru.emplace_front(key, entry);
        state_->lruIndex[key] = state_->lru.begin();
        state_->lruBytes += entry->size;
        while (state_->lru.size() > state_->maxEntries || state_->lruBytes > state_->maxBytes) {
            auto back = std::prev(state_->lru.end());
            state_->lruBytes -= back->second->size;
            evicted.push_back(std::move(back->second));
            state_->lruIndex.erase(back->first);
            state_->lru.erase(back);
        }
    }
};

}  // namespace switch_compiler
