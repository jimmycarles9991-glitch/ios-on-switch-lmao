// SPDX-License-Identifier: MIT
// A mask of a binding table's slots, bit k slot k. A table of 32 slots or fewer keeps a uint32_t, so its masks compile to
// the word operations they always were; a wider table (24B119's 128 render textures a stage) keeps a word per 32 slots.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace raw {

template <size_t Words> struct WideSlotMask {
    std::array<uint32_t, Words> words{};

    WideSlotMask &operator|=(const WideSlotMask &o)
    {
        for (size_t w = 0; w < Words; ++w)
            words[w] |= o.words[w];
        return *this;
    }

    WideSlotMask &operator&=(const WideSlotMask &o)
    {
        for (size_t w = 0; w < Words; ++w)
            words[w] &= o.words[w];
        return *this;
    }

    friend WideSlotMask operator|(WideSlotMask a, const WideSlotMask &b)
    {
        return a |= b;
    }

    friend WideSlotMask operator&(WideSlotMask a, const WideSlotMask &b)
    {
        return a &= b;
    }

    bool operator==(const WideSlotMask &o) const
    {
        return words == o.words;
    }

    bool operator!=(const WideSlotMask &o) const
    {
        return words != o.words;
    }
};

template <size_t N> using SlotMask = std::conditional_t<(N <= 32), uint32_t, WideSlotMask<(N + 31) / 32>>;

template <typename Mask> struct SlotMaskWords;

template <> struct SlotMaskWords<uint32_t> {
    static constexpr size_t kWords = 1;

    static uint32_t &word(uint32_t &mask, size_t)
    {
        return mask;
    }

    static uint32_t word(const uint32_t &mask, size_t)
    {
        return mask;
    }
};

template <size_t Words> struct SlotMaskWords<WideSlotMask<Words>> {
    static constexpr size_t kWords = Words;

    static uint32_t &word(WideSlotMask<Words> &mask, size_t w)
    {
        return mask.words[w];
    }

    static uint32_t word(const WideSlotMask<Words> &mask, size_t w)
    {
        return mask.words[w];
    }
};

// The slots a mask can name
template <typename Mask> constexpr uint32_t kMaskSlots = uint32_t(32 * SlotMaskWords<Mask>::kWords);

template <typename Mask> inline bool hasSlot(const Mask &mask, uint32_t slot)
{
    return SlotMaskWords<Mask>::word(mask, slot / 32) >> slot % 32 & 1;
}

template <typename Mask> inline void addSlot(Mask &mask, uint32_t slot)
{
    SlotMaskWords<Mask>::word(mask, slot / 32) |= 1u << slot % 32;
}

template <typename Mask> inline Mask oneSlot(uint32_t slot)
{
    Mask mask{};
    addSlot(mask, slot);
    return mask;
}

template <typename Mask> inline Mask everySlot()
{
    Mask mask{};
    for (size_t w = 0; w < SlotMaskWords<Mask>::kWords; ++w)
        SlotMaskWords<Mask>::word(mask, w) = ~0u;
    return mask;
}

// Slots 0 to `count` - 1
template <typename Mask> inline Mask firstSlots(size_t count)
{
    Mask mask{};
    for (size_t w = 0; w < SlotMaskWords<Mask>::kWords; ++w) {
        const size_t below = count > 32 * w ? count - 32 * w : 0;
        SlotMaskWords<Mask>::word(mask, w) = below >= 32 ? ~0u : (1u << below) - 1;
    }
    return mask;
}

template <typename Mask> inline bool anySlot(const Mask &mask)
{
    for (size_t w = 0; w < SlotMaskWords<Mask>::kWords; ++w)
        if (SlotMaskWords<Mask>::word(mask, w))
            return true;
    return false;
}

template <typename Mask> inline uint32_t slotCount(const Mask &mask)
{
    uint32_t count = 0;
    for (size_t w = 0; w < SlotMaskWords<Mask>::kWords; ++w)
        count += uint32_t(__builtin_popcount(SlotMaskWords<Mask>::word(mask, w)));
    return count;
}

// `visit(slot)` for each slot of `mask`, lowest first
template <typename Mask, typename Visit> inline void forEachSlot(const Mask &mask, Visit visit)
{
    for (size_t w = 0; w < SlotMaskWords<Mask>::kWords; ++w)
        for (uint32_t bits = SlotMaskWords<Mask>::word(mask, w); bits; bits &= bits - 1)
            visit(uint32_t(32 * w + size_t(__builtin_ctz(bits))));
}

}  // namespace raw
