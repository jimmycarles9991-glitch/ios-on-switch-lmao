// SPDX-License-Identifier: MIT
// A record's set-0 entries: the first kPackEntries in its PackPipeline, the rest in its far block (pack version 27): a run
// of words in the pack's blob section at PackPipeline::farEntriesAt (a runtime record's RuntimeRecord::far), the entries
// past kPackEntries two words each, then the boundEntries bits and the memoryEntries bits of those, a word per 32 entries.
#pragma once
#include <cstdint>
#include "pack.h"

namespace gm20b {

// The Metal arguments a render stage binds (device_answers.h kCounts): 31 buffers on both identities, 31 textures on
// 21H461's A10X and 128 on 24B119's A13, and 16 samplers, constexpr samplers among them (the Metal compiler refuses a
// function of more: "uses too many samplers (17)")
constexpr uint32_t kStageBufferArguments = 31, kStageSamplerArguments = 16;
constexpr uint32_t kStageTextureArgumentsA10X = 31, kStageTextureArgumentsA13 = 128;
constexpr uint32_t kRenderArgumentsA10X =
    2 * (kStageBufferArguments + kStageTextureArgumentsA10X + kStageSamplerArguments);
constexpr uint32_t kRenderArgumentsA13 =
    2 * (kStageBufferArguments + kStageTextureArgumentsA13 + kStageSamplerArguments);
static_assert(kRenderArgumentsA10X == 156 && kRenderArgumentsA13 == 350);
// The entries the driver binds of its own: the sRGB colour table, each stage's texture and sampler handle tables and
// argument table, the tessellation buffers, the depth and stencil stand-ins, and each colour's fetch
constexpr uint32_t kColorTableEntries = 1, kHandleTableEntries = 2 * 2, kArgumentTableEntries = 2;
constexpr uint32_t kStandInEntries = 2, kColorEntries = 8;
constexpr uint32_t kSyntheticEntries = kColorTableEntries + kHandleTableEntries + kArgumentTableEntries +
                                       TessBufferCount + kStandInEntries + kColorEntries;
static_assert(kSyntheticEntries == 20);
// The entries a record holds at most: every argument of the widest identity's render pipeline and every synthetic entry
constexpr uint32_t kSetEntriesMax = kRenderArgumentsA13 + kSyntheticEntries;

constexpr uint32_t kFarEntryWords = sizeof(PackEntry) / 4;

inline uint32_t entryCount(const PackPipeline &p)
{
    return p.entriesLow | uint32_t(p.entriesHigh) << 8;
}

inline void setEntryCount(PackPipeline &p, uint32_t count)
{
    p.entriesLow = uint8_t(count);
    p.entriesHigh = uint8_t(count >> 8);
}

constexpr uint32_t farEntries(uint32_t count)
{
    return count > kPackEntries ? count - kPackEntries : 0;
}

constexpr uint32_t farMaskWords(uint32_t count)
{
    return (farEntries(count) + 31) / 32;
}

// The words of the far block of a record of `count` entries
constexpr uint32_t farWords(uint32_t count)
{
    return kFarEntryWords * farEntries(count) + 2 * farMaskWords(count);
}

inline uint32_t farEntriesAt(const PackPipeline &p)
{
    return p.farEntriesAt[0] | uint32_t(p.farEntriesAt[1]) << 8 | uint32_t(p.farEntriesAt[2]) << 16;
}

// spillEntry[k] with its high byte: the entry index + 1 of stage k's argument table, 0: none
inline uint32_t spillEntryOf(const PackPipeline &p, uint32_t k)
{
    return p.spillEntry[k] | (uint32_t(p.spillEntryHigh) >> 8 * k & 0xFF) << 8;
}

inline void setSpillEntry(PackPipeline &p, uint32_t k, uint32_t entry)
{
    p.spillEntry[k] = uint8_t(entry);
    p.spillEntryHigh = uint16_t((p.spillEntryHigh & ~(0xFFu << 8 * k)) | (entry >> 8 & 0xFF) << 8 * k);
}

// A record's entries, `far` its far block (nullptr: a record of kPackEntries or fewer)
class SetEntries {
public:
    SetEntries() = default;

    SetEntries(const PackPipeline &p, const uint32_t *far) : head_(&p), far_(far), count_(entryCount(p))
    {}

    uint32_t size() const
    {
        return count_;
    }

    bool wide() const
    {
        return count_ > kPackEntries;
    }

    const PackEntry &operator[](uint32_t k) const
    {
        return k < kPackEntries ? head_->entry[k] : reinterpret_cast<const PackEntry *>(far_)[k - kPackEntries];
    }

    // its far block, nullptr: none
    const uint32_t *far() const
    {
        return far_;
    }

    bool memory(uint32_t k) const
    {
        return k < kPackEntries ? head_->memoryEntries >> k & 1 : farBit(farMaskWords(count_), k);
    }

    bool bound(uint32_t k) const
    {
        return k < kPackEntries ? head_->boundEntries >> k & 1 : farBit(0, k);
    }

private:
    bool farBit(uint32_t mask, uint32_t k) const
    {
        const uint32_t far = k - kPackEntries;
        return far_[kFarEntryWords * farEntries(count_) + mask + far / 32] >> far % 32 & 1;
    }

    const PackPipeline *head_ = nullptr;
    const uint32_t *far_ = nullptr;
    uint32_t count_ = 0;
};

// The words of a record's set as the encoder builds and uploads it (descriptors.h renderSetBytes)
constexpr uint32_t setWords(uint32_t setBytes)
{
    return (setBytes <= 256 ? 256 : (setBytes + 255) & ~255u) / 4;
}

// The set words an entry's descriptor takes: a buffer's or a table's four, a handle's one
constexpr uint32_t entryWords(uint8_t kind)
{
    return kind == EntryUbo || kind == EntryTessBuffer || kind == EntryColorTable || kind == EntryTextureTable ||
                   kind == EntrySamplerTable || kind == EntrySpillTable
               ? 4
               : 1;
}

// Why a record's entries are not ones its draws can write, null where they are: each of a kind the encoder writes,
// inside the set it uploads, a buffer at a stage and slot it binds, a colour of the pass's eight, a tessellation
// buffer of the three, a sampler of the stage's or one of the record's `constexprSamplers`, and each argument table
// and uniform range naming such an entry. A record from outside the process (a pack, the record cache) is held to
// this once, where it enters; a draw indexes by its entries unchecked. A texture slot's bound is the identity's
// (stage_textures.h), which the draw keeps: a texture3d entry of kNoSlot binds no texture.
inline const char *entriesRefusal(const PackPipeline &p, const uint32_t *far, uint32_t constexprSamplers)
{
    const SetEntries entries(p, far);
    const uint32_t words = setWords(p.setBytes);
    for (uint32_t k = 0; k < entries.size(); k++) {
        const PackEntry &e = entries[k];
        if (e.kind > EntryTessBuffer)
            return "an entry of a kind no draw writes";
        if (e.offset / 4 + entryWords(e.kind) > words)
            return "an entry past its record's set";
        if ((e.kind == EntryUbo || e.kind == EntrySpillTable) && e.stage > 2)
            return "a buffer entry of no stage";
        if (e.kind == EntryUbo && e.slot >= kStageBufferArguments)
            return "a buffer entry past its stage's buffers";
        if (e.kind == EntryTessBuffer && e.slot >= TessBufferCount)
            return "a tessellation buffer entry past the three";
        if (e.kind == EntryFetch && e.slot >= 8)
            return "a colour entry past the pass's eight";
        if (e.kind == EntryUbo)
            continue;
        if (e.samplerKind == SamplerSlot && e.sampler >= kStageSamplers)
            return "a sampler entry past its stage's samplers";
        if (entryWords(e.kind) == 1 &&
            (e.samplerKind > SamplerConstexpr || (e.samplerKind == SamplerConstexpr && e.sampler >= constexprSamplers)))
            return "a handle entry of no sampler the record holds";
    }
    for (uint32_t k = 0; k < 2; k++)
        if (const uint32_t table = spillEntryOf(p, k);
            table > entries.size() || (table && entries[table - 1].offset / 4 + 4 > words))
            return "an argument table past its record's entries";
    for (const PackUniformRange &u : p.uniform)
        if (u.entry != kNoUniform && u.entry < entries.size() &&
            (entries[u.entry].stage > 2 || entries[u.entry].slot >= kStageBufferArguments))
            return "a uniform range of an entry past its stage's buffers";
    return nullptr;
}

}  // namespace gm20b
