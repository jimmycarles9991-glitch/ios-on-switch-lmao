// SPDX-License-Identifier: MIT
// Whether a heap image array of a class holds a texture, and the flags of the texture's handle-table entry: m2v's rules
// (metal2vulkan bind/heap_code.rs heap_code_holds over bind/heap.rs fit, and bind/heap.rs handle_entry_flags), whose words
// the translated functions read, over the per-format facts heap_classes.h holds. A texture whose facts carry a value
// no Metal enumerator has fits no class and carries flags 0, as m2v names no texture of it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include "heap_classes.h"
#include "heap_tables.h"

namespace gm20b {

namespace heap_fit {

namespace hc = heap_classes;

// heap_code.rs's class word: bits 0-1 the kind, 2-4 the dimension, 5 arrayed, 6 multisampled, 7-8 the sampled type, 9
// depth comparison, 10 storage, 11-12 the storage format class and 13-31 a storage format's VkFormat
constexpr uint32_t kImageKind = 1, kDimBuffer = 4, kSampledUint = 2, kFormatShift = 13;
constexpr uint32_t kStorageFormat = 2, kStorageHalfBits = 3;

struct ImageClass {
    uint32_t dim, sampled, format;
    bool arrayed, multisampled, depthCompare, storage;
};

constexpr bool bit(uint32_t word, uint32_t n)
{
    return word >> n & 1;
}

// heap_code_class's image class of `word`; false where it names none (or a sampler class)
constexpr bool imageClass(uint32_t word, ImageClass &c)
{
    const uint32_t dim = word >> 2 & 7, format = word >> 11 & 3;
    if ((word & 3) != kImageKind || dim > kDimBuffer || (format != kStorageFormat && word >> kFormatShift))
        return false;
    c = {dim, word >> 7 & 3, format, bit(word, 5), bit(word, 6), bit(word, 9), bit(word, 10)};
    return true;
}

struct Dims {
    uint32_t dim;
    bool arrayed, multisampled;
};

// heap.rs dims of each MTLTextureType, as the class word's dimension (1D 0, 2D 1, 3D 2, cube 3, texture buffer 4)
constexpr Dims kDims[hc::kTextureTypes] = {{0, false, false}, {0, true, false},  {1, false, false}, {1, true, false},
                                           {1, false, true},  {3, false, false}, {3, true, false},  {2, false, false},
                                           {1, true, true},   {4, false, false}};

constexpr const hc::Format *format(uint32_t mtl)
{
    size_t lo = 0, hi = std::size(hc::kFormats);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (hc::kFormats[mid].mtl == mtl)
            return &hc::kFormats[mid];
        if (hc::kFormats[mid].mtl < mtl)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

// The format's row where every value of `f` is a Metal enumerator's (the bridge's raw::<T>), else null
constexpr const hc::Format *row(const HeapTextureFacts &f)
{
    if (f.textureType >= hc::kTextureTypes || (f.usage & ~hc::kUsageTaken))
        return nullptr;
    for (uint8_t s : f.swizzle)
        if (s >= hc::kSwizzles)
            return nullptr;
    return format(f.pixelFormat);
}

// The swizzle's key (heap_classes.h), -1 where it has none
constexpr int swizzleKey(const uint8_t (&s)[4])
{
    int key = 0;
    for (int c = 0; c < 4; c++) {
        if (s[c] == hc::kLacking[c])
            key |= 1 << c;
        else if (s[c] != hc::kOwn[c])
            return -1;
    }
    return key;
}

// fit's Fit::Holds
constexpr bool fits(const ImageClass &c, const hc::Format &pf, const HeapTextureFacts &f)
{
    const Dims d = kDims[f.textureType];
    const uint32_t needs = c.storage ? hc::kShaderWrite | hc::kBlockWritesOnly : hc::kShaderRead;
    const bool shaped = d.dim == c.dim && d.arrayed == c.arrayed && d.multisampled == c.multisampled &&
                        c.dim != kDimBuffer && (f.sampleCount > 1) == c.multisampled &&
                        (!c.depthCompare || pf.depthStencil) && (f.usage & needs) && !(c.storage && pf.depthStencil);
    const int key = swizzleKey(f.swizzle);
    if (c.format == kStorageHalfBits)
        return shaped && key >= 0 && (pf.halfBits >> key & 1);
    if (!shaped)
        return false;
    const bool gm20bTexels = pf.heapRead && f.sampleCount <= 1 && key == 0;
    if (gm20bTexels && !c.storage)
        return c.sampled == kSampledUint;
    return pf.sampled == c.sampled;
}

}  // namespace heap_fit

// Whether an image array of class `heapClass` (PackHeapArray) holds a texture of `facts`; a HeapHolds
inline bool heapHolds(uint32_t heapClass, const HeapTextureFacts *facts)
{
    heap_fit::ImageClass c = {};
    const heap_classes::Format *pf = facts ? heap_fit::row(*facts) : nullptr;
    return pf && heap_fit::imageClass(heapClass, c) && heap_fit::fits(c, *pf, *facts);
}

// The flags of a texture's handle-table entry (HeapTexture::flags)
inline uint32_t heapTextureFlags(const HeapTextureFacts &facts)
{
    const heap_classes::Format *pf = heap_fit::row(facts);
    if (!pf)
        return 0;
    const int key = heap_fit::swizzleKey(facts.swizzle);
    if (key == 0)
        return pf->own;
    return key > 0 && (pf->alike >> key & 1) ? pf->alt : pf->other;
}

}  // namespace gm20b
