// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_set_reads.h"
#include "../raw/runtime/raw_descriptors.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

constexpr uint64_t kMovCbuf = 0x4c98ull << 48, kLdc = 0xef90ull << 48, kRz = 255;

uint64_t movCbuf(uint32_t bank, uint32_t offset)
{
    return kMovCbuf | uint64_t(bank) << 34 | uint64_t(offset / 4) << 20;
}

uint64_t ldc(uint32_t bank, uint32_t offset, uint32_t ra = kRz, uint32_t size = 4)
{
    return kLdc | uint64_t(bank) << 36 | uint64_t(offset) << 20 | uint64_t(ra) << 8 | uint64_t(size) << 48;
}

// The instructions three a group behind a scheduling word, as NAK lays SM50 code out
std::vector<uint8_t> code(const std::vector<uint64_t> &instructions, uint64_t schedule = 0)
{
    std::vector<uint64_t> words;
    for (size_t i = 0; i < instructions.size(); i++) {
        if (i % 3 == 0)
            words.push_back(schedule);
        words.push_back(instructions[i]);
    }
    while (words.size() % 4)
        words.push_back(0);
    const auto *bytes = reinterpret_cast<const uint8_t *>(words.data());
    return std::vector<uint8_t>(bytes, bytes + words.size() * 8);
}

// a uniform buffer's descriptor at 0, a texture handle at 16 and a sampler's at 20
raw::PackPipeline record()
{
    raw::PackPipeline pp{};
    pp.entries = 3;
    pp.entry[0] = {0, raw::EntryUbo, 1, 0, 0, 0, 0};
    pp.entry[1] = {16, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0};
    pp.entry[2] = {20, raw::EntrySamplerOnly, 2, 0, raw::SamplerSlot, 1, 0};
    return pp;
}

uint32_t reads(const std::vector<uint64_t> &instructions, uint32_t setSlot = 3, uint64_t schedule = 0)
{
    const raw::PackPipeline pp = record();
    const std::vector<uint8_t> bytes = code(instructions, schedule);
    return raw::setMemoryReads(bytes.data(), bytes.size(), setSlot, pp.entry, pp.entries);
}

TEST(RawSetReads, AConstantOperandAtTheSetSlotReadsTheEntryItCovers)
{
    EXPECT_EQ(reads({movCbuf(3, 16)}), 1u << 1);
    EXPECT_EQ(reads({movCbuf(3, 8)}), 1u << 0);
    EXPECT_EQ(reads({ldc(3, 20)}), 1u << 2);
    EXPECT_EQ(reads({ldc(3, 12, kRz, 5)}), 1u << 0 | 1u << 1);
}

TEST(RawSetReads, OtherSlotsAndSchedulingWordsReadNothing)
{
    EXPECT_EQ(reads({movCbuf(2, 16), ldc(1, 0)}), 0u);
    EXPECT_EQ(reads({movCbuf(3, 16)}, 0), 0u);
    EXPECT_EQ(reads({movCbuf(2, 0)}, 3, movCbuf(3, 16)), 0u);
}

TEST(RawSetReads, AComputedOffsetOrTheRootsSetAddressReadsEveryEntry)
{
    EXPECT_EQ(reads({ldc(3, 0, 2)}), 7u);
    EXPECT_EQ(reads({ldc(0, 0, 4)}, 0), 7u);
    EXPECT_EQ(reads({movCbuf(0, 0x38)}, 0), 7u);
    EXPECT_EQ(reads({ldc(0, 0x3c)}, 0), 7u);
    EXPECT_EQ(reads({movCbuf(0, 0x30), movCbuf(0, 0x700)}, 0), 0u);
}

TEST(RawSetReads, BoundTexturesReadEveryHandleOfTheSet)
{
    const raw::PackPipeline pp = record();
    const std::vector<uint8_t> bytes = code({movCbuf(2, 0)});
    const uint8_t cbufs[4] = {raw::CbufRoot, raw::CbufShaderData, raw::CbufUbo, raw::CbufSet};
    EXPECT_EQ(raw::programSetReads(bytes.data(), bytes.size(), cbufs, 4, 4, pp), 1u << 1 | 1u << 2);
    EXPECT_EQ(raw::programSetReads(bytes.data(), bytes.size(), cbufs, 4, 0, pp), 0u);
    EXPECT_EQ(raw::programSetReads(bytes.data(), bytes.size(), cbufs, 4, 3, pp), 0u);
}

// A program as the heap holds it (PackProgram): its slot, the shader program header at 0x30, the code at 0x80
struct PlacedProgram {
    std::vector<uint8_t> slot;
    raw::PackProgram record{};
};

PlacedProgram placed(const std::vector<uint64_t> &instructions, const std::vector<uint8_t> &cbufTypes)
{
    PlacedProgram p;
    const std::vector<uint8_t> body = code(instructions);
    p.slot.assign(0x80 + body.size(), 0);
    memcpy(p.slot.data() + 0x80, body.data(), body.size());
    p.record.bytes = uint32_t(p.slot.size());
    p.record.cbufs = uint8_t(cbufTypes.size());
    memcpy(p.record.cbufType, cbufTypes.data(), cbufTypes.size());
    return p;
}

// What a state's draws upload of set 0 when entry `entry` moves (PreparedDescriptors::resolve): 1 or 0 uploads
size_t uploadsWhenMoved(uint32_t memoryEntries, uint32_t entry)
{
    raw::PreparedDescriptors descriptors;
    size_t uploads = 0;
    const auto build = [](raw::PreparedDescriptorWords &words, uint32_t moved) {
        words.words[0] += moved;
        return true;
    };
    const auto upload = [&](const raw::DescriptorWords &) {
        return uint64_t(0x1000 * ++uploads);
    };
    const int pipeline = 0;
    descriptors.resolve(&pipeline, ~0u, memoryEntries, build, upload);
    uploads = 0;
    descriptors.resolve(&pipeline, 1u << entry, memoryEntries, build, upload);
    return uploads;
}

// No pack state reads set 0 at an offset it computes or through the root table's address of the set (directpack over the
// HW Test and precompile corpora): synthetic states do, through the heap program's code and the draws' set uploads
TEST(RawSetReads, ASyntheticStateThatComputesItsSetOffsetUploadsTheSetForEveryMovedEntry)
{
    const raw::PackPipeline pp = record();
    const std::vector<uint8_t> types = {raw::CbufRoot, raw::CbufShaderData, raw::CbufUbo, raw::CbufSet};
    const PlacedProgram computed = placed({movCbuf(3, 0), ldc(3, 0, 2)}, types);
    const uint32_t memory = raw::placedSetReads(computed.slot.data() + 0x30, computed.record, pp);
    EXPECT_EQ(memory, 7u);
    for (uint32_t entry = 0; entry < pp.entries; entry++)
        EXPECT_EQ(uploadsWhenMoved(memory, entry), 1u) << entry;
}

TEST(RawSetReads, ASyntheticStateThatReadsTheRootsSetAddressUploadsTheSetForEveryMovedEntry)
{
    const raw::PackPipeline pp = record();
    const std::vector<uint8_t> types = {raw::CbufRoot, raw::CbufShaderData};
    const PlacedProgram root = placed({movCbuf(1, 0), movCbuf(0, 0x3c)}, types);
    const uint32_t memory = raw::placedSetReads(root.slot.data() + 0x30, root.record, pp);
    EXPECT_EQ(memory, 7u);
    for (uint32_t entry = 0; entry < pp.entries; entry++)
        EXPECT_EQ(uploadsWhenMoved(memory, entry), 1u) << entry;
}

// Beside them, a state reading its uniform buffer's descriptor alone uploads the set only where that entry moves
TEST(RawSetReads, ASyntheticStateThatReadsOneEntryUploadsTheSetWhereThatEntryMoves)
{
    const raw::PackPipeline pp = record();
    const std::vector<uint8_t> types = {raw::CbufRoot, raw::CbufShaderData, raw::CbufUbo, raw::CbufSet};
    const PlacedProgram one = placed({movCbuf(3, 4), movCbuf(0, 0x700)}, types);
    const uint32_t memory = raw::placedSetReads(one.slot.data() + 0x30, one.record, pp);
    EXPECT_EQ(memory, 1u);
    EXPECT_EQ(uploadsWhenMoved(memory, 0), 1u);
    EXPECT_EQ(uploadsWhenMoved(memory, 1), 0u);
    EXPECT_EQ(uploadsWhenMoved(memory, 2), 0u);
}

}  // namespace
