// SPDX-License-Identifier: MIT
// Programs run in place from a read-only file mapping (the pack, the shader compiler's store): the file's pages are
// imported and bound in the program window at offsets that keep each page's place in the file, so a slot keeps the
// 256-byte alignment it has there. Those pages are the page cache's, shared by every process that maps the file, and
// phys_footprint counts none of them (XNU task.c: internal, compressed, IOKit and page-table memory only), wired or not.
// They are imported in fixed grains of the file, so every process's import of a grain is the same page list: the kernel's
// backing and coherency registries (keyed by the first page) then share one entry for it, and no two imports overlap.
// This is which grains a range needs that are not bound yet.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace raw {

// How far past a program's end the GPU's instruction prefetch reads (what is bound after each slot)
constexpr uint64_t kProgramPrefetchBytes = 0x1000;
// The shader compiler's store keeps a program slot as an entry of whole pages of this size (SwitchSeedStore.h places such
// an entry on its own pages): the slot, then zeros through the prefetch past its end
constexpr uint64_t kProgramSlotPage = 16384;
// What one import takes of a file: a multiple of every page size the kernel and the GPU map by
constexpr uint64_t kProgramGrain = 0x10000;

constexpr uint64_t programSlotEntryBytes(uint64_t slot)
{
    return (slot + kProgramPrefetchBytes + kProgramSlotPage - 1) / kProgramSlotPage * kProgramSlotPage;
}

class ProgramPages {
public:
    struct Grain {
        uint64_t offset, bytes;  // in the file: kProgramGrain bytes, but the mapping's last
    };

    // The file's [begin, end) (end within the mapping), whose mapping runs to `mapped` (a page multiple: the last grain
    // ends there), from window offset `window` (a kProgramGrain multiple) on
    void reset(uint64_t begin, uint64_t end, uint64_t mapped, uint64_t window)
    {
        first_ = begin / kProgramGrain * kProgramGrain;
        end_ = std::min((end + kProgramGrain - 1) / kProgramGrain * kProgramGrain, mapped);
        window_ = window;
        bound_ = 0;
        bits_.assign(size_t(((end_ - first_ + kProgramGrain - 1) / kProgramGrain + 63) / 64), 0);
        set_ = end_ > first_;
    }

    uint64_t windowOffset(uint64_t fileOffset) const
    {
        return window_ + (fileOffset - first_);
    }

    uint64_t fileOffset(uint64_t windowOffset) const
    {
        return first_ + (windowOffset - window_);
    }

    // The window offsets the region spans: [window, windowEnd), windowEnd a grain multiple
    uint64_t windowEnd() const
    {
        return window_ + (end_ - first_ + kProgramGrain - 1) / kProgramGrain * kProgramGrain;
    }

    bool holdsWindow(uint64_t windowOffset) const
    {
        return set_ && windowOffset >= window_ && windowOffset < window_ + (end_ - first_);
    }

    // The unbound grains [offset, offset + bytes) lies in, in file order; false where it is not inside the region
    bool missing(uint64_t offset, uint64_t bytes, std::vector<Grain> &grains) const
    {
        grains.clear();
        if (!set_ || !bytes || offset < first_ || offset > end_ || bytes > end_ - offset)
            return false;
        const uint64_t g0 = (offset - first_) / kProgramGrain, g1 = (offset + bytes - first_ - 1) / kProgramGrain;
        for (uint64_t g = g0; g <= g1; g++)
            if (!isBound(g)) {
                const uint64_t at = first_ + g * kProgramGrain;
                grains.push_back({at, std::min(kProgramGrain, end_ - at)});
            }
        return true;
    }

    bool bound(uint64_t offset, uint64_t bytes) const
    {
        std::vector<Grain> grains;
        return missing(offset, bytes, grains) && grains.empty();
    }

    void bind(const Grain &g)
    {
        const uint64_t index = (g.offset - first_) / kProgramGrain;
        if (isBound(index))
            return;
        bits_[index / 64] |= uint64_t(1) << (index % 64);
        bound_ += g.bytes;
    }

    uint64_t boundBytes() const
    {
        return bound_;
    }

private:
    uint64_t first_ = 0, end_ = 0, window_ = 0, bound_ = 0;
    bool set_ = false;
    std::vector<uint64_t> bits_;

    bool isBound(uint64_t g) const
    {
        return bits_[g / 64] >> (g % 64) & 1;
    }
};

}  // namespace raw
