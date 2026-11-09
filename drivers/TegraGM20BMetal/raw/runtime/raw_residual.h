// SPDX-License-Identifier: MIT
// m2v's SlotBindOffset residual (metal2vulkan bind/instantiate.rs descriptor_range): a buffer slot's descriptor starts at the
// bind offset rounded down to the view's base alignment, and the function reads the rest from a root push word. Its uvec4 and
// narrow views index by the descriptor byte, so that byte stays congruent to the Metal address modulo 16 (memory/views.rs
// BASE_ALIGNMENT): a descriptor at the bind offset itself would read another element.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include "raw_pack.h"

namespace raw {

// max(min*BufferOffsetAlignment, 16) of NVK's: a uniform buffer's 256-byte cbuf alignment (nvk_min_cbuf_alignment before
// Turing), a storage buffer's NVK_MIN_SSBO_ALIGNMENT
constexpr uint32_t bufferBaseAlignment(bool storage)
{
    return storage ? 16 : 256;
}

struct BufferDescriptor {
    uint64_t base = 0;
    uint32_t range = 0;
    uint32_t residual = 0;
    int word = -1;  // the root push word (0x700 + 4 x word) the residual goes to; -1: the residual is 0
};

// A set-0 buffer descriptor for `size` bytes bound at `va`: m2v's range (the function's `window` in bytes, 0 the whole buffer,
// clamped to the bytes padded to 16; a uniform one at most NVK_MAX_CBUF_SIZE and rounded up to the cbuf alignment,
// nvk_ubo_descriptor). False where the residual is not 0 and `word` (the record's push word for it) is -1.
inline bool bufferDescriptor(bool storage, uint32_t window, int word, uint64_t va, uint32_t size, BufferDescriptor &d)
{
    d = {};
    d.range = (size + 15) & ~15u;
    if (!storage)
        d.range = std::min(d.range, 0x10000u);
    if (window && window < d.range)
        d.range = window;
    d.residual = uint32_t(va & (bufferBaseAlignment(storage) - 1));
    d.base = va - d.residual;
    if (d.residual) {
        d.word = word;
        if (d.word < 0)
            return false;
        const uint64_t end = (va + size + 15) & ~15ull,
                       hi = window ? std::min<uint64_t>((va + window + 15) & ~15ull, end) : end;
        d.range = uint32_t(std::min<uint64_t>(hi - d.base, storage ? 0xFFFFFFF0u : 0x10000u));
    }
    if (!storage)
        d.range = (d.range + 255) & ~255u;
    return true;
}

inline bool bufferDescriptor(const PackEntry &e, uint64_t va, uint32_t size, BufferDescriptor &d)
{
    return bufferDescriptor(bufferStorage(e), (uint32_t(e.sampler) | uint32_t(e.pad) << 8) * 16, bufferResidualWord(e),
                            va, size, d);
}

// Compute buffer entry `e`'s descriptor: a kernel's buffers are storage buffers
inline bool bufferDescriptor(const PackKernelEntry &e, uint64_t va, uint32_t size, BufferDescriptor &d)
{
    return bufferDescriptor(true, kernelBufferWindow(e), kernelResidualWord(e), va, size, d);
}

// The root push words the bind-offset run set where the 256-byte run left 0, by their value's low byte (pack/mkpack.py
// attach_residuals); a low byte two words share names neither
inline std::map<uint32_t, uint32_t> residualWordsByValue(const std::map<uint32_t, uint32_t> &plain,
                                                         const std::map<uint32_t, uint32_t> &bound)
{
    std::map<uint32_t, uint32_t> out;
    std::map<uint32_t, int> seen;
    for (const auto &[offset, word] : bound) {
        const auto was = plain.find(offset);
        if (!word || (was != plain.end() && was->second))
            continue;
        if (seen[word & 0xff]++)
            out.erase(word & 0xff);
        else
            out[word & 0xff] = offset;
    }
    return out;
}

}  // namespace raw
