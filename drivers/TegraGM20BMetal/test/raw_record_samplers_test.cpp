// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <vector>
#include "../raw/runtime/raw_record_parts.h"

namespace {
using Header = std::array<uint32_t, 8>;

// The first constexpr sampler of a 24B119 pack (linear, repeating) and the header linear_fragment's clamp_to_edge sampler
// makes (linear, clamped to the edge, LOD clamp 0.25), which that pack holds at index 3
constexpr Header kLinearRepeat = {0x26000, 0x2a2, 0x40000, 0, 0, 0, 0, 0};
constexpr Header kLinearClamp = {0x26092, 0x2a2, 0x40000, 0, 0, 0, 0, 0};
constexpr Header kNearestClamp = {0x26092, 0x291, 0x40000, 0, 0, 0, 0, 0};

// Device::constexprSampler's numbering: the pack's headers first, then each other header once, after them
struct Samplers {
    std::vector<Header> placed;

    bool place(const Header &words, uint32_t &index)
    {
        const auto at = std::find(placed.begin(), placed.end(), words);
        index = uint32_t(at - placed.begin());
        if (at == placed.end())
            placed.push_back(words);
        return true;
    }
};

raw::CapturedProgram program(const char *id)
{
    raw::CapturedProgram p;
    p.id = id;
    p.image.assign(96, 0);
    p.cbufTypes = {raw::CbufRoot};
    return p;
}

raw::LayoutEntry sampled(uint32_t offset, uint8_t slot, const Header &header)
{
    raw::LayoutEntry e;
    e.offset = offset;
    e.tex = raw::Tex::Slot;
    e.texSlot = slot;
    e.smp = raw::Smp::Constexpr;
    e.headerKnown = true;
    e.header = header;
    return e;
}

raw::RuntimeRecord assemble(const std::vector<raw::LayoutEntry> &layout)
{
    static const raw::CapturedProgram vs = program("vs"), fs = program("fs");
    raw::RecordParts parts;
    parts.programs[0] = &vs;
    parts.programs[1] = &fs;
    parts.layout = layout;
    raw::RuntimeRecord out;
    std::string why;
    EXPECT_TRUE(raw::assembleRecord(parts, "c0=80 | v vs | f fs", raw::PackRules{}, out, why)) << why;
    return out;
}

// The header each constexpr entry samples with once the device placed the record's samplers
std::vector<Header> drawn(raw::RuntimeRecord record, Samplers &device)
{
    bool unknown = false;
    EXPECT_TRUE(raw::placeConstexprSamplers(
        record.pipeline, record.samplers,
        [&](const Header &words, uint32_t &index) {
            return device.place(words, index);
        },
        unknown));
    std::vector<Header> out;
    for (uint32_t k = 0; k < record.pipeline.entries; k++)
        if (record.pipeline.entry[k].samplerKind == raw::SamplerConstexpr)
            out.push_back(device.placed.at(record.pipeline.entry[k].sampler));
    return out;
}

TEST(RecordSamplers, AnEntryIndexesItsRecordsOwnHeaders)
{
    const raw::RuntimeRecord r =
        assemble({sampled(0x10, 0, kLinearClamp), sampled(0x14, 1, kNearestClamp), sampled(0x18, 2, kLinearClamp)});
    ASSERT_EQ(r.samplers.size(), 2u);
    for (uint32_t k = 0; k < r.pipeline.entries; k++) {
        const raw::PackEntry &e = r.pipeline.entry[k];
        ASSERT_EQ(e.samplerKind, raw::SamplerConstexpr);
        ASSERT_LT(e.sampler, r.samplers.size());
    }
    EXPECT_EQ(r.samplers[r.pipeline.entry[0].sampler], kLinearClamp);
    EXPECT_EQ(r.samplers[r.pipeline.entry[1].sampler], kNearestClamp);
    EXPECT_EQ(r.pipeline.entry[2].sampler, r.pipeline.entry[0].sampler);
}

// A slot the state samples by comparison (the direct compile's write, the probe's captured compare header) is a compared
// entry, which the draw binds the sampler's compare header at; a slot sampled plainly is not
TEST(RecordSamplers, ASlotSampledByComparisonIsACompareEntry)
{
    raw::LayoutEntry compared, plain;
    for (raw::LayoutEntry *e : {&compared, &plain}) {
        e->tex = raw::Tex::Slot;
        e->smp = raw::Smp::Slot;
    }
    compared.offset = 0x10;
    compared.smpCompare = true;
    plain.offset = 0x14;
    plain.texSlot = plain.smpSlot = 1;
    const raw::RuntimeRecord r = assemble({compared, plain});
    ASSERT_EQ(r.pipeline.entries, 2u);
    EXPECT_TRUE(raw::entryCompares(r.pipeline.entry[0]));
    EXPECT_EQ(r.pipeline.entry[0].sampler, 0);
    EXPECT_FALSE(raw::entryCompares(r.pipeline.entry[1]));
    EXPECT_EQ(r.pipeline.entry[1].sampler, 1);
}

// gpu.draw.render_then_sample on device: a record of the native seed (its first header at index 0) drawn on a pack whose
// sampler 0 repeats sampled column 127 through the repeating sampler, halfway to column 0
TEST(RecordSamplers, ASeededRecordSamplesItsOwnHeaderNotThePacksOfTheSameIndex)
{
    Samplers device{{kLinearRepeat, kNearestClamp, kNearestClamp, kLinearClamp}};
    const std::vector<Header> got = drawn(assemble({sampled(0x10, 0, kLinearClamp)}), device);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], kLinearClamp);
    EXPECT_EQ(device.placed.size(), 4u);
}

TEST(RecordSamplers, AHeaderThePackLacksIsPlacedAfterThePacksOnce)
{
    const Header own = {0x260db, 0x2a2, 0xf00000, 0, 0, 0, 0, 0};
    Samplers device{{kLinearRepeat, kLinearClamp}};
    const std::vector<Header> got =
        drawn(assemble({sampled(0x10, 0, own), sampled(0x14, 1, kLinearClamp), sampled(0x18, 2, own)}), device);
    EXPECT_EQ(got, (std::vector<Header>{own, kLinearClamp, own}));
    EXPECT_EQ(device.placed.size(), 3u);
}

TEST(RecordSamplers, AnEntryNamingNoHeaderOfItsRecordIsRefused)
{
    raw::RuntimeRecord r = assemble({sampled(0x10, 0, kLinearClamp)});
    r.pipeline.entry[0].sampler = 1;
    Samplers device{{kLinearRepeat, kLinearClamp}};
    bool unknown = false;
    EXPECT_FALSE(raw::placeConstexprSamplers(
        r.pipeline, r.samplers,
        [&](const Header &words, uint32_t &index) {
            return device.place(words, index);
        },
        unknown));
    EXPECT_TRUE(unknown);
}

TEST(RecordSamplers, NoRoomLeavesTheRecordRetryable)
{
    raw::RuntimeRecord r = assemble({sampled(0x10, 0, kLinearClamp)});
    bool unknown = true;
    EXPECT_FALSE(raw::placeConstexprSamplers(
        r.pipeline, r.samplers,
        [](const Header &, uint32_t &) {
            return false;
        },
        unknown));
    EXPECT_FALSE(unknown);
}

}  // namespace
