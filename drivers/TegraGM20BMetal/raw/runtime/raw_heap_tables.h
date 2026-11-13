// SPDX-License-Identifier: MIT
// What a draw reads through argument buffers on the raw path: m2v's device-wide heap (metal2vulkan bind/heap.rs, the
// HeapPlacement::ResourceId form that keeps one heap of every live texture), where a texture's resource ID (raw_resource_id.h:
// 1 << 32 | TIC) is element TIC of every set-1 array and a sampler's (1 << 32 | TSC << 20) element TSC; and m2v's root table
// (memory/roots.rs), whose resident set is one identity entry, since a buffer's gpuAddress is its GPU VA. Everything here is
// written when an object's ID is first asked for and when the object goes, never per draw.
//  - The texture table: an entry per texture header i, {1 << 32 | i, element, flags}, element i for a texture whose ID was
//    asked for and the neutral element 0 otherwise, flags its m2v handle_entry_flags. Its ids never change, so it stays
//    sorted and every id resolves as m2v's lookup_host says; entry 0 is the null entry. The sampler table likewise by
//    sampler header, its flags the sampler's SamplerWord.
//  - A heap set per set-1 layout (a record's arrays: their classes and NVK offsets): element i of an image array is
//    texture header i's sampled header where the texture fits the array's class (HeapHolds), header 0 (the null texture)
//    otherwise; element i of a sampler array is the sampled header of sampler header i: its own, or for a sampler of pixel
//    coordinates its normalized twin, since m2v's program scales such a sampler's coordinates itself (its SamplerWord).
//  - The roots: the root table of a draw whose stages reach no buffer slot through it, {null, sink}, then the identity
//    entry {start, start, end - start, 0} over the connection's VA range, which every table's resident set is: a user VA
//    resolves to itself and address 0 misses, reading nothing. The sink, and the ExecutionStatus words, which a program
//    writes on m2v's execution errors and no one reads: with every VA resident a resident-set miss is impossible.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "raw_pack.h"
#include "raw_resource_id.h"

namespace raw {

// m2v's handle-table entry (memory/handles.rs TABLE_ENTRY_BYTES) and root-table entry (memory/roots.rs ENTRY_BYTES)
struct HandleEntry {
    uint32_t idLo, idHi, element, flags;
};

struct RootEntry {
    uint64_t host, guest, size, bindOffset;
};

static_assert(sizeof(HandleEntry) == 16 && sizeof(RootEntry) == 32, "m2v's table entries");

constexpr uint32_t kRootSinkBytes = 64;         // m2v NeutralResource::SINK_BYTES
constexpr uint32_t kExecutionStatusBytes = 32;  // m2v ExecutionStatus::BYTES
constexpr uint32_t kSinkRoot = 1, kFirstSlotRoot = 2;

// m2v RootTable::resident_fields' R0 for a table at `table` whose resident set starts at `set`: a set below the table is
// 2^64 minus its distance in entries, so table + 32 * R0 reaches it in 64-bit wrapping arithmetic
constexpr uint64_t residentFirst(uint64_t table, uint64_t set)
{
    return set >= table ? (set - table) / sizeof(RootEntry) : 0 - (table - set) / sizeof(RootEntry);
}

// A buffer slot a draw's root table holds (entry 2 + n for [[buffer(n)]]): what the slot binds, va 0 for none
struct RootSlot {
    uint64_t va = 0, bytes = 0, bindOffset = 0;
};

// The entries of a draw's root table whose stage reaches the slots `slots` (bit n: [[buffer(n)]], not 0) through it
inline uint32_t drawRootEntries(uint32_t slots)
{
    return kFirstSlotRoot + 32 - uint32_t(__builtin_clz(slots));
}

// A draw's root table at `va` (drawRootEntries(slots) entries): m2v's {null, sink, an entry per slot} over the resident set
// at `identity`; a slot outside `slots` or bound to nothing is the null entry
inline void writeDrawRoots(RootEntry *table, uint64_t va, uint32_t slots, const RootSlot *slot, uint64_t sink,
                           uint64_t identity)
{
    const uint32_t entries = drawRootEntries(slots);
    table[0] = {};
    table[kSinkRoot] = {sink, residentFirst(va, identity), 1, 0};
    for (uint32_t n = 0; kFirstSlotRoot + n < entries; n++) {
        const RootSlot &s = slot[n];
        table[kFirstSlotRoot + n] = (slots >> n & 1) && s.va
                                        ? RootEntry{s.va, s.va, (s.bytes + 15) & ~uint64_t(15), s.bindOffset}
                                        : RootEntry{};
    }
}

// A texture as the bridge asks whether an image array holds it (switch_nmb_heap_holds): its MTLTextureDescriptor's
struct HeapTextureFacts {
    uint32_t pixelFormat, textureType, sampleCount, usage, arrayLength, levels;
    uint8_t swizzle[4];
};

// MTLTextureUsageShaderRead | ShaderWrite | RenderTarget | PixelFormatView: what Metal takes MTLTextureUsageUnknown (0) as
constexpr uint32_t kTextureUsageEvery = 0x1 | 0x2 | 0x4 | 0x10;

inline HeapTextureFacts heapFacts(HeapTextureFacts facts)
{
    if (!facts.usage)
        facts.usage = kTextureUsageEvery;
    return facts;
}

// Whether an image array of class `heapClass` (PackHeapArray) holds a texture of `facts`
using HeapHolds = bool (*)(uint32_t heapClass, const HeapTextureFacts *facts);

struct HeapTexture {
    uint32_t sampleTic = 0;  // the header a sample of it reads (Texture::standIn's where it has one)
    uint32_t flags = 0;      // m2v handle_entry_flags
    HeapTextureFacts facts = {};
};

// m2v's SamplerWord::PIXEL_COORDINATES (metal/sampler.rs): normalizedCoordinates is false
constexpr uint32_t kSamplerWordPixelCoordinates = 1;

struct HeapSampler {
    uint32_t sampleTsc = 0;  // the header a sample through it reads
    uint32_t word = 0;       // m2v SamplerWord
};

struct HeapMemory {
    uint8_t *cpu = nullptr;
    uint64_t va = 0, bytes = 0;
};

// A record's set-1 layout: a state's (PackPipeline) or a kernel's (PackKernel)
struct HeapLayout {
    uint8_t arrays = 0;
    const PackHeapArray *array = nullptr;
    uint32_t bytes = 0;
};

inline HeapLayout heapLayout(const PackPipeline &p)
{
    return {p.heapArrays, p.heap, p.heapBytes};
}

inline HeapLayout heapLayout(const PackKernel &k)
{
    return {k.heapArrays, k.heap, k.heapBytes};
}

// One record's set 1, shared by every record of the same arrays
struct HeapSet {
    uint8_t arrays = 0;
    PackHeapArray array[kPackHeapArrays] = {};
    HeapMemory memory;

    bool of(const HeapLayout &l) const
    {
        return l.arrays == arrays && l.bytes <= memory.bytes && !memcmp(l.array, array, sizeof(PackHeapArray) * arrays);
    }
};

class HeapTables {
public:
    static constexpr uint64_t tablesBytes(uint32_t textures, uint32_t samplers)
    {
        return uint64_t(textures + samplers) * sizeof(HandleEntry);
    }

    static constexpr uint64_t kRootsBytes = 3 * sizeof(RootEntry) + kRootSinkBytes + kExecutionStatusBytes;

    // `tables`: tablesBytes(textures, samplers); `roots`: kRootsBytes, its VA 32-byte aligned; [vaStart, vaEnd): every VA a
    // buffer can have
    void attach(HeapMemory tables, HeapMemory roots, uint32_t textures, uint32_t samplers, uint64_t vaStart,
                uint64_t vaEnd, HeapHolds holds)
    {
        tables_ = tables;
        roots_ = roots;
        holds_ = holds;
        textures_.assign(textures, {});
        live_.assign(textures, false);
        samplers_.assign(samplers, {});
        for (uint32_t i = 0; i < textures; i++)
            textureEntry(i) = i ? HandleEntry{i, uint32_t(kResourceIdTag >> 32), 0, 0} : HandleEntry{};
        for (uint32_t i = 0; i < samplers; i++)
            samplerEntry(i) =
                i ? HandleEntry{i << kTextureHeaderBits, uint32_t(kResourceIdTag >> 32), 0, 0} : HandleEntry{};
        auto *root = reinterpret_cast<RootEntry *>(roots_.cpu);
        root[0] = {};
        root[kSinkRoot] = {sinkVa(), 2, 1, 0};
        root[2] = {vaStart, vaStart, vaEnd - vaStart, 0};
        memset(roots_.cpu + 3 * sizeof(RootEntry), 0, kRootSinkBytes + kExecutionStatusBytes);
    }

    bool attached() const
    {
        return tables_.cpu != nullptr;
    }

    uint64_t textureTableVa() const
    {
        return tables_.va;
    }

    uint32_t textureTableBytes() const
    {
        return uint32_t(textures_.size() * sizeof(HandleEntry));
    }

    uint64_t samplerTableVa() const
    {
        return tables_.va + textureTableBytes();
    }

    uint32_t samplerTableBytes() const
    {
        return uint32_t(samplers_.size() * sizeof(HandleEntry));
    }

    // the root table of a draw whose stages reach no buffer slot through it
    uint64_t rootTableVa() const
    {
        return roots_.va;
    }

    uint64_t identityVa() const
    {
        return roots_.va + 2 * sizeof(RootEntry);
    }

    uint64_t sinkVa() const
    {
        return roots_.va + 3 * sizeof(RootEntry);
    }

    uint64_t statusVa() const
    {
        return sinkVa() + kRootSinkBytes;
    }

    // Texture header `tic` now names `texture` (null: none)
    void texture(uint32_t tic, const HeapTexture *texture)
    {
        if (!tic || tic >= textures_.size())
            return;
        live_[tic] = texture != nullptr;
        textures_[tic] = texture ? *texture : HeapTexture{};
        textureEntry(tic).element = texture ? tic : 0;
        textureEntry(tic).flags = texture ? texture->flags : 0;
        for (const auto &set : sets_)
            writeTexture(*set, tic);
    }

    // Sampler header `tsc` now names `sampler` (null: none)
    void sampler(uint32_t tsc, const HeapSampler *sampler)
    {
        if (!tsc || tsc >= samplers_.size())
            return;
        samplers_[tsc] = sampler ? *sampler : HeapSampler{};
        samplerEntry(tsc).element = sampler ? tsc : 0;
        samplerEntry(tsc).flags = sampler ? sampler->word : 0;
        for (const auto &set : sets_)
            writeSampler(*set, tsc);
    }

    const HeapSet *find(const HeapLayout &l) const
    {
        for (const auto &set : sets_)
            if (set->of(l))
                return set.get();
        return nullptr;
    }

    // `l`'s set in `memory` (l.bytes of it), its elements those of every texture and sampler named so far
    const HeapSet *add(const HeapLayout &l, HeapMemory memory)
    {
        auto set = std::make_unique<HeapSet>();
        set->arrays = std::min<uint8_t>(l.arrays, kPackHeapArrays);
        memcpy(set->array, l.array, sizeof(PackHeapArray) * set->arrays);
        set->memory = memory;
        memset(memory.cpu, 0, memory.bytes);
        for (uint32_t i = 1; i < textures_.size(); i++)
            if (live_[i])
                writeTexture(*set, i);
        for (uint32_t i = 1; i < samplers_.size(); i++)
            if (samplers_[i].sampleTsc)
                writeSampler(*set, i);
        sets_.push_back(std::move(set));
        return sets_.back().get();
    }

    const std::vector<std::unique_ptr<HeapSet>> &sets() const
    {
        return sets_;
    }

    const HandleEntry &textureEntryAt(uint32_t i) const
    {
        return reinterpret_cast<const HandleEntry *>(tables_.cpu)[i];
    }

    const HandleEntry &samplerEntryAt(uint32_t i) const
    {
        return reinterpret_cast<const HandleEntry *>(tables_.cpu + textureTableBytes())[i];
    }

    // An array of `heapClass` holds samplers (metal2vulkan heap_class_code's kind 2)
    static bool samplerClass(uint32_t heapClass)
    {
        return (heapClass & 3) == 2;
    }

private:
    HeapMemory tables_, roots_;
    HeapHolds holds_ = nullptr;
    std::vector<HeapTexture> textures_;
    std::vector<bool> live_;
    std::vector<HeapSampler> samplers_;
    std::vector<std::unique_ptr<HeapSet>> sets_;

    HandleEntry &textureEntry(uint32_t i)
    {
        return reinterpret_cast<HandleEntry *>(tables_.cpu)[i];
    }

    HandleEntry &samplerEntry(uint32_t i)
    {
        return reinterpret_cast<HandleEntry *>(tables_.cpu + textureTableBytes())[i];
    }

    void element(const HeapSet &set, uint32_t array, uint32_t i, uint32_t word)
    {
        const uint64_t at = uint64_t(set.array[array].offset) + 4ull * i;
        if (at + 4 <= set.memory.bytes)
            memcpy(set.memory.cpu + at, &word, 4);
    }

    void writeTexture(const HeapSet &set, uint32_t tic)
    {
        const HeapTexture &t = textures_[tic];
        for (uint32_t a = 0; a < set.arrays; a++)
            if (!samplerClass(set.array[a].heapClass))
                element(set, a, tic,
                        live_[tic] && holds_ && holds_(set.array[a].heapClass, &t.facts) ? t.sampleTic : 0);
    }

    void writeSampler(const HeapSet &set, uint32_t tsc)
    {
        for (uint32_t a = 0; a < set.arrays; a++)
            if (samplerClass(set.array[a].heapClass))
                element(set, a, tsc, samplers_[tsc].sampleTsc << kTextureHeaderBits);
    }
};

}  // namespace raw
