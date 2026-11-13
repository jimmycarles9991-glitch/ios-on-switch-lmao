// SPDX-License-Identifier: MIT
// Development builds: how often each pipeline key hash left a path, in a fixed open-addressed table with no lock, so the
// device log can name the states still leaving it. A key arriving when every slot holds another is counted only in
// `overflow`.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace raw {

class KeyTally {
public:
    static constexpr size_t kSlots = 128;

    void note(uint64_t key)
    {
        if (!key)
            key = 1;
        for (size_t i = 0; i < kSlots; i++) {
            Slot &s = slots_[(size_t(key) + i) % kSlots];
            uint64_t held = s.key.load(std::memory_order_acquire);
            if (!held && s.key.compare_exchange_strong(held, key, std::memory_order_acq_rel))
                held = key;
            if (held == key) {
                s.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        overflow.fetch_add(1, std::memory_order_relaxed);
    }

    // The `n` keys counted most since the last take, most first, and their counts; the counts start again from zero
    std::vector<std::pair<uint64_t, uint64_t>> take(size_t n)
    {
        std::vector<std::pair<uint64_t, uint64_t>> counted;
        for (Slot &s : slots_)
            if (const uint64_t count = s.count.exchange(0, std::memory_order_relaxed))
                counted.emplace_back(s.key.load(std::memory_order_acquire), count);
        std::sort(counted.begin(), counted.end(), [](const auto &a, const auto &b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        if (counted.size() > n)
            counted.resize(n);
        return counted;
    }

    std::atomic<uint64_t> overflow{0};

private:
    struct Slot {
        std::atomic<uint64_t> key{0}, count{0};
    };

    std::array<Slot, kSlots> slots_{};
};

}  // namespace raw
