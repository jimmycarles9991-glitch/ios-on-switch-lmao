// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
#include "raw_grow_list.h"
#include "raw_slot_mask.h"

namespace raw {

// The captured values of a table of N slots, as StateSnapshots keeps them, each held as its chunks of K slots in an
// append-only arena: a capture copies only the chunks with a slot edited since the capture before, and names the earlier
// capture's chunks for the rest. A draw's setBytes edit a few slots of a 31-slot table: a capture copies one chunk.
template <typename Slot, size_t N, size_t K = 4> class SlotSnapshots {
public:
    using Table = std::array<Slot, N>;
    using Chunk = std::array<Slot, K>;
    using Mask = SlotMask<N>;
    static constexpr size_t kChunks = (N + K - 1) / K;
    static_assert(kChunks <= 32 && 32 % K == 0, "a capture's chunks are one word, a mask word's slots whole chunks");
    using Header = std::array<uint32_t, kChunks>;

    class View {
    public:
        View(const Chunk *chunks, const Header &header) : chunks_(chunks), header_(&header)
        {}

        const Slot &operator[](size_t slot) const
        {
            return chunks_[(*header_)[slot / K]][slot % K];
        }

        static constexpr size_t size()
        {
            return N;
        }

        class Iterator {
        public:
            Iterator(const View &view, size_t slot) : view_(&view), slot_(slot)
            {}

            const Slot &operator*() const
            {
                return (*view_)[slot_];
            }

            Iterator &operator++()
            {
                ++slot_;
                return *this;
            }

            bool operator!=(const Iterator &other) const
            {
                return slot_ != other.slot_;
            }

        private:
            const View *view_;
            size_t slot_;
        };

        Iterator begin() const
        {
            return Iterator(*this, 0);
        }

        Iterator end() const
        {
            return Iterator(*this, N);
        }

    private:
        const Chunk *chunks_;
        const Header *header_;
    };

    const Table &current() const
    {
        return current_;
    }

    View operator[](uint32_t index) const
    {
        return View(chunks_.data(), headers_[index]);
    }

    uint64_t generation() const
    {
        return generation_;
    }

    size_t size() const
    {
        return headers_.size();
    }

    size_t storageBytes() const
    {
        return headers_.capacity() * sizeof(Header) + chunks_.capacity() * sizeof(Chunk) +
               changed_.capacity() * sizeof(Mask);
    }

    template <typename Edit> bool update(Edit edit)
    {
        if (!edit(current_))
            return false;
        ++generation_;
        pending_ = edited_ = everySlot<Mask>();
        return true;
    }

    // An edit of slot `slot` alone
    template <typename Edit> bool update(uint32_t slot, Edit edit)
    {
        if (!edit(current_))
            return false;
        ++generation_;
        const Mask bit = slot < kMaskSlots<Mask> ? oneSlot<Mask>(slot) : everySlot<Mask>();
        pending_ |= bit;
        edited_ |= bit;
        return true;
    }

    uint32_t capture()
    {
        if (captured_ == generation_)
            return index_;
        Header header;
        uint32_t chunks = kAllChunks;
        if (!headers_.empty()) {
            header = headers_.back();
            chunks = editedChunks(pending_);
        }
        for (; chunks; chunks &= chunks - 1) {
            const size_t c = size_t(__builtin_ctz(chunks));
            header[c] = static_cast<uint32_t>(chunks_.size());
            Chunk &chunk = chunks_.emplace_back();
            for (size_t k = 0; k < K; ++k)
                chunk[k] = c * K + k < N ? current_[c * K + k] : Slot{};
        }
        index_ = static_cast<uint32_t>(headers_.size());
        headers_.push_back(header);
        changed_.push_back(pending_);
        pending_ = Mask{};
        captured_ = generation_;
        return index_;
    }

    // The slots in which value `index` may differ from value `from`: exactly the edited ones where `index` was captured
    // next after `from`, every slot otherwise
    Mask changedFrom(uint32_t from, uint32_t index) const
    {
        return index == from + 1 ? changed_[index] : everySlot<Mask>();
    }

    // Only the slots edited since the last reset differ from the empty table
    void resetCurrent()
    {
        current_ = {};
        ++generation_;
        pending_ |= edited_;
        edited_ = Mask{};
    }

    void clear()
    {
        headers_.clear();
        chunks_.clear();
        changed_.clear();
        pending_ = everySlot<Mask>();
        edited_ = Mask{};
        current_ = {};
        generation_ = 0;
        captured_ = std::numeric_limits<uint64_t>::max();
        index_ = std::numeric_limits<uint32_t>::max();
    }

private:
    static constexpr uint32_t kAllChunks = kChunks >= 32 ? ~0u : (1u << kChunks) - 1;

    static constexpr uint32_t chunkSlots(size_t chunk)
    {
        return chunk * K >= 32 ? ~0u : uint32_t(((uint64_t(1) << K) - 1) << (chunk * K));
    }

    // the chunks of one mask word holding a slot of `slots`, visited by the set slots alone
    static constexpr uint32_t wordChunks(uint32_t slots)
    {
        uint32_t chunks = 0;
        while (slots) {
            const size_t c = size_t(__builtin_ctz(slots)) / K;
            chunks |= 1u << c;
            slots &= ~chunkSlots(c);
        }
        return chunks;
    }

    static constexpr uint32_t editedChunks(uint32_t slots)
    {
        return wordChunks(slots) & kAllChunks;
    }

    template <size_t Words> static constexpr uint32_t editedChunks(const WideSlotMask<Words> &slots)
    {
        uint32_t chunks = 0;
        for (size_t w = 0; w < Words; ++w)
            chunks |= wordChunks(slots.words[w]) << (w * 32 / K);
        return chunks & kAllChunks;
    }

    Table current_{};
    uint64_t generation_ = 0;
    uint64_t captured_ = std::numeric_limits<uint64_t>::max();
    uint32_t index_ = std::numeric_limits<uint32_t>::max();
    GrowList<Header> headers_;
    GrowList<Chunk> chunks_;
    GrowList<Mask> changed_;
    Mask pending_ = everySlot<Mask>();
    Mask edited_{};
};

// A table of N object slots (textures, samplers) kept as SlotSnapshots keeps a buffer table, each capture with its mask of
// the slots bound (to an object or to nil): a draw that binds one texture copies one chunk of four, not the table (of
// eight in a table past 32 slots, whose chunk list stays one word)
template <size_t N> class ObjectSnapshots {
    using Objects = SlotSnapshots<void *, N, (N <= 32 ? 4 : 8)>;

public:
    using Mask = SlotMask<N>;

    struct View {
        typename Objects::View objects;
        Mask set;
    };

    struct Current {
        const typename Objects::Table &objects;
        Mask set;
    };

    Current current() const
    {
        return {objects_.current(), set_};
    }

    View operator[](uint32_t index) const
    {
        return {objects_[index], sets_[index]};
    }

    uint64_t generation() const
    {
        return objects_.generation();
    }

    size_t size() const
    {
        return objects_.size();
    }

    size_t storageBytes() const
    {
        return objects_.storageBytes() + sets_.capacity() * sizeof(Mask);
    }

    // `object` at `slot`; false where the slot held it bound already or there is no such slot
    bool bind(uint32_t slot, void *object)
    {
        if (slot >= N)
            return false;
        if (hasSlot(set_, slot) && objects_.current()[slot] == object)
            return false;
        addSlot(set_, slot);
        return objects_.update(slot, [&](typename Objects::Table &table) {
            table[slot] = object;
            return true;
        });
    }

    uint32_t capture()
    {
        const uint32_t index = objects_.capture();
        if (index == sets_.size())
            sets_.push_back(set_);
        return index;
    }

    Mask changedFrom(uint32_t from, uint32_t index) const
    {
        return objects_.changedFrom(from, index);
    }

    void resetCurrent()
    {
        objects_.resetCurrent();
        set_ = Mask{};
    }

    void clear()
    {
        objects_.clear();
        sets_.clear();
        set_ = Mask{};
    }

private:
    Objects objects_;
    GrowList<Mask> sets_;
    Mask set_{};
};

}  // namespace raw
