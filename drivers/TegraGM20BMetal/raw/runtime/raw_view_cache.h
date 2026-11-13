// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace raw {

// The encoding thread's answers of Device::viewTic: a texture's header and a view key (word 0, and above it the sample grid
// of a storage view) to the view header made for them,
// each kept with the generation its own header had when it was made. Freeing a header moves its generation (under the
// device's lock), and a texture that takes the freed number again is made after that free, so a view kept for an
// earlier texture of the same header number is never answered.
class ViewCache {
public:
    explicit ViewCache(uint32_t headers) : generations_(std::make_unique<std::atomic<uint32_t>[]>(headers))
    {}

    uint32_t find(uint32_t own, uint64_t key) const
    {
        const Seen &s = seen_[slot(own, key)];
        if (!s.tic || s.own != own || s.key != key)
            return 0;
        return s.generation == generations_[own].load(std::memory_order_acquire) ? s.tic : 0;
    }

    // Under the device's lock, as viewTic made or found `tic`
    void keep(uint32_t own, uint64_t key, uint32_t tic)
    {
        seen_[slot(own, key)] = {key, own, tic, generations_[own].load(std::memory_order_relaxed)};
    }

    // Under the device's lock: header `own` and its views were freed
    void freed(uint32_t own)
    {
        generations_[own].fetch_add(1, std::memory_order_release);
    }

private:
    struct Seen {
        uint64_t key = 0;
        uint32_t own = 0, tic = 0, generation = 0;
    };

    static constexpr size_t kSlots = 64;

    static size_t slot(uint32_t own, uint64_t key)
    {
        return ((own * 0x9e3779b1u) ^ (uint32_t(key ^ key >> 32) * 0x85ebca6bu)) >> 26;
    }

    std::array<Seen, kSlots> seen_{};
    std::unique_ptr<std::atomic<uint32_t>[]> generations_;
};

}  // namespace raw
