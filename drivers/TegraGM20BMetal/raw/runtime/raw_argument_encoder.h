// SPDX-License-Identifier: GPL-2.0-or-later OR LGPL-3.0-or-later
// MTLArgumentEncoder's writes over a layout (raw_argument_layout.h), as metal2vulkan-runtime's argument_encoder.rs refuses
// what Metal's validation stops (measured on an M3 Max and an M4): a setter before an argument buffer, an argument buffer the
// CPU cannot write or whose encoded length passes its end, a buffer offset past the buffer, an argument of another type at
// the index. Plain Metal's answers stand where validation would assert: a setter at an index no argument has writes at the
// arguments' offset 0 while 8 bytes fit there, and constantDataAtIndex: there answers the arguments' start. A function's
// encoder shares its argument buffer's layout and those its pointers point to with the encoders of those.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "raw_argument_layout.h"

namespace raw {

enum class ArgumentRefusal : uint8_t {
    None,
    NotWritable,   // the argument buffer is private or has no CPU contents
    PastTheEnd,    // the encoded length passes the argument buffer's end
    NoBuffer,      // no argument buffer is set
    OtherType,     // the index holds an argument of another type
    NoIndex,       // no argument has the index and 8 bytes do not fit at the arguments' start
    OffsetPastEnd  // a buffer's offset at or past its end
};

// One layout as the bridge gives a function's (bridge/NativeMetalObjects.h NmbArgumentLayout)
struct ArgumentRuns {
    uint64_t encodedLength, alignment;
    const ArgumentRun *runs;
    uint32_t count;
};

class ArgumentEncoder {
public:
    using Layouts = std::shared_ptr<const std::vector<ArgumentLayout>>;

    ArgumentEncoder() = default;

    explicit ArgumentEncoder(ArgumentLayout layout, Layouts layouts = {})
        : layout_(std::move(layout)), layouts_(std::move(layouts))
    {}

    // A function's encoder: its argument buffer's layout (the first) and those of the buffers its pointers point to
    static bool fromRuns(const ArgumentRuns *layouts, uint32_t count, ArgumentEncoder &out)
    {
        auto all = std::make_shared<std::vector<ArgumentLayout>>(count);
        for (uint32_t i = 0; i < count; i++)
            if (!ArgumentLayout::fromRuns(layouts[i].encodedLength, layouts[i].alignment, layouts[i].runs,
                                          layouts[i].count, count, (*all)[i]))
                return false;
        if (!count)
            return false;
        out = ArgumentEncoder(all->front(), all);
        return true;
    }

    const ArgumentLayout &layout() const
    {
        return layout_;
    }

    // `cpu`: the argument buffer's contents, null where the CPU cannot write it (private storage too)
    ArgumentRefusal setBuffer(uint8_t *cpu, uint64_t length, uint64_t start, uint64_t element)
    {
        cpu_ = nullptr;
        if (!cpu)
            return ArgumentRefusal::NotWritable;
        const uint64_t at = start + element * layout_.encodedLength;
        if (at > length || length - at < layout_.encodedLength)
            return ArgumentRefusal::PastTheEnd;
        cpu_ = cpu;
        length_ = length;
        start_ = at;
        return ArgumentRefusal::None;
    }

    void clearBuffer()
    {
        cpu_ = nullptr;
    }

    // Where argument `index` of `kind` goes in the argument buffer's bytes; null where Metal's validation stops the call
    uint8_t *at(uint64_t index, ArgumentKind kind, ArgumentRefusal &why) const
    {
        why = ArgumentRefusal::None;
        if (!cpu_) {
            why = ArgumentRefusal::NoBuffer;
            return nullptr;
        }
        ArgumentPlace place;
        uint64_t offset = 0;
        if (index <= UINT32_MAX && layout_.place(uint32_t(index), place)) {
            if (place.kind != kind) {
                why = ArgumentRefusal::OtherType;
                return nullptr;
            }
            offset = place.offset;
        } else if (length_ - start_ < kArgumentHandleBytes) {
            why = ArgumentRefusal::NoIndex;
            return nullptr;
        }
        return cpu_ + start_ + offset;
    }

    ArgumentRefusal write(uint64_t word, uint64_t index, ArgumentKind kind)
    {
        ArgumentRefusal why;
        if (uint8_t *to = at(index, kind, why))
            memcpy(to, &word, sizeof word);
        return why;
    }

    // A buffer of `length` bytes at `gpuAddress` (0: none) bound at `offset`
    ArgumentRefusal writeBuffer(uint64_t gpuAddress, uint64_t length, uint64_t offset, uint64_t index)
    {
        if (gpuAddress && offset >= length)
            return ArgumentRefusal::OffsetPastEnd;
        return write(gpuAddress ? gpuAddress + offset : 0, index, ArgumentKind::Buffer);
    }

    // Whether every index of [first, first + count) takes an argument of `kind`: a range call writes all or nothing
    ArgumentRefusal fits(uint64_t first, uint64_t count, ArgumentKind kind) const
    {
        ArgumentRefusal why = ArgumentRefusal::None;
        for (uint64_t i = 0; i < count; i++)
            if (!at(first + i, kind, why))
                return why;
        return why;
    }

    uint8_t *constantData(uint64_t index, ArgumentRefusal &why) const
    {
        why = cpu_ ? ArgumentRefusal::None : ArgumentRefusal::NoBuffer;
        if (!cpu_)
            return nullptr;
        ArgumentPlace place;
        const uint64_t offset = index <= UINT32_MAX && layout_.place(uint32_t(index), place) ? place.offset : 0;
        return cpu_ + start_ + offset;
    }

    // The encoder of the argument buffer the pointer at `index` points to; false for any other index, as Metal answers nil
    // (a layout of descriptors holds none)
    bool nested(uint64_t index, ArgumentEncoder &out) const
    {
        ArgumentPlace place;
        if (!layouts_ || index > UINT32_MAX || !layout_.place(uint32_t(index), place) ||
            place.kind != ArgumentKind::Buffer || place.nested < 0 || size_t(place.nested) >= layouts_->size())
            return false;
        out = ArgumentEncoder((*layouts_)[size_t(place.nested)], layouts_);
        return true;
    }

private:
    ArgumentLayout layout_;
    Layouts layouts_;
    uint8_t *cpu_ = nullptr;
    uint64_t length_ = 0, start_ = 0;
};

}  // namespace raw
