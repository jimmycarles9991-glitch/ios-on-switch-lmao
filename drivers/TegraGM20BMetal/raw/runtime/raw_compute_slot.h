// SPDX-License-Identifier: MIT
// A runtime-made kernel's slot in the shader heap: its code from the slot's start (a kernel has no shader header), its
// shader data 256-aligned after it, as NVK uploads them. The shader compiler publishes the same bytes, so a client runs
// a large one in place from the store (switch_compiler::Store::putProgramSlots).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "raw_compute_record.h"

namespace raw {

inline size_t computeSlotDataAt(const ComputeRecord &r)
{
    return r.data.empty() ? r.code.size() : (r.code.size() + 0xff) & ~size_t(0xff);
}

inline std::vector<uint8_t> computeSlot(const ComputeRecord &r)
{
    const size_t dataAt = computeSlotDataAt(r);
    std::vector<uint8_t> slot(dataAt + r.data.size(), 0);
    memcpy(slot.data(), r.code.data(), r.code.size());
    if (!r.data.empty())
        memcpy(slot.data() + dataAt, r.data.data(), r.data.size());
    return slot;
}

}  // namespace raw
