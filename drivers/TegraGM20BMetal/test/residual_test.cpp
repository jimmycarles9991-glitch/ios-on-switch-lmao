// SPDX-License-Identifier: MIT
// residual.h and pack/probe_bind.h: a buffer bound off m2v's descriptor base alignment draws on raw with its
// SlotBindOffset residual in the push word the probe's bind-offset run names, a storage buffer's as a uniform one's, and a
// kernel's buffer dispatches with it in the word the direct compile's bind-offset dispatch names.
#include "../pack/probe_bind.h"
#include "../residual.h"

#include <gtest/gtest.h>

#include <set>

namespace {

gm20b::PackEntry bufferEntry(bool storage, int word, uint32_t windowBytes = 0)
{
    gm20b::PackEntry e = {};
    e.kind = gm20b::EntryUbo;
    e.stage = 1;
    e.slot = 1;
    e.samplerKind = uint8_t((storage ? 1 : 0) | (word >= 0 ? (word + 1) << 2 : 0));
    e.sampler = uint8_t(windowBytes / 16);
    e.pad = uint8_t(windowBytes / 16 >> 8);
    return e;
}

TEST(GM20BResidual, ProbeOffsetsNameEverySlot)
{
    std::set<uint32_t> offsets;
    for (uint32_t stage = 1; stage <= 2; stage++)
        for (uint32_t slot = 0; slot < 31; slot++) {
            const uint32_t at = gm20b::probeBindOffset(stage, slot), code = gm20b::probeStorageCode(stage, slot);
            EXPECT_TRUE(offsets.insert(at).second) << stage << " " << slot;
            EXPECT_GE(at, 16u) << "a uniform word's value is never a storage code";
            EXPECT_LE(at, 256u) << "inside the probe's 256 + 256-byte payload buffer";
            EXPECT_NE(code, 0u) << "a storage slot's word is set in the run";
            if (stage == 1) {
                EXPECT_EQ(at % 4, 0u) << "a fixed-function vertex buffer's offset";
            } else {
                EXPECT_NE(code % 4, 0u) << "no vertex slot's code";
            }
        }
    EXPECT_EQ(gm20b::probeBindOffset(1, 1), 24u);
    EXPECT_EQ(gm20b::probeStorageCode(1, 1), 8u);
    EXPECT_EQ(gm20b::probeBindOffset(2, 0), 193u);
    EXPECT_EQ(gm20b::probeBindOffset(2, 30), 233u);
}

// boot 13's iconservicesagent: vertex storage slot 1 at offset 0x38e8 (enc.ubo a=0x101, b=0x38e8_00000008)
TEST(GM20BResidual, StorageOffsetOffSixteenTakesItsWord)
{
    const uint64_t va = 0x200000000ull + 0x38e8;
    gm20b::BufferDescriptor d;
    ASSERT_TRUE(gm20b::bufferDescriptor(bufferEntry(true, 5), va, 0x400, d));
    EXPECT_EQ(d.base, va - 8);
    EXPECT_EQ(d.base % 16, 0u);
    EXPECT_EQ(d.residual, 8u);
    EXPECT_EQ(d.word, 5);
    EXPECT_EQ(d.range, 0x410u) << "from the aligned base to the bytes' end padded to 16";
    EXPECT_FALSE(gm20b::bufferDescriptor(bufferEntry(true, -1), va, 0x400, d));
    EXPECT_EQ(d.residual, 8u);
}

TEST(GM20BResidual, StorageWindowEndsWhereTheFunctionReads)
{
    const uint64_t va = 0x200000000ull + 0x104;
    gm20b::BufferDescriptor d;
    ASSERT_TRUE(gm20b::bufferDescriptor(bufferEntry(true, 2, 64), va, 0x1000, d));
    EXPECT_EQ(d.base, va - 4);
    EXPECT_EQ(d.range, 80u);
}

TEST(GM20BResidual, AlignedOffsetsNeedNoWord)
{
    gm20b::BufferDescriptor d;
    ASSERT_TRUE(gm20b::bufferDescriptor(bufferEntry(true, -1), 0x200000010ull, 0x30, d));
    EXPECT_EQ(d.word, -1);
    EXPECT_EQ(d.base, 0x200000010ull);
    EXPECT_EQ(d.range, 0x30u);
    ASSERT_TRUE(gm20b::bufferDescriptor(bufferEntry(false, -1), 0x200000100ull, 0x30, d));
    EXPECT_EQ(d.word, -1);
    EXPECT_EQ(d.range, 0x100u) << "NVK's cbuf alignment";
}

TEST(GM20BResidual, UniformResidualIsOffTwoHundredFiftySix)
{
    const uint64_t va = 0x200000000ull + 0x1230;
    gm20b::BufferDescriptor d;
    ASSERT_TRUE(gm20b::bufferDescriptor(bufferEntry(false, 3), va, 0x40, d));
    EXPECT_EQ(d.base, 0x200001200ull);
    EXPECT_EQ(d.residual, 0x30u);
    EXPECT_EQ(d.range, 0x100u);
    EXPECT_FALSE(gm20b::bufferDescriptor(bufferEntry(false, -1), va, 0x40, d));
}

TEST(GM20BResidual, WordsAreNamedByTheirValueOnce)
{
    const std::map<uint32_t, uint32_t> plain = {{0x700, 0}, {0x704, 0}, {0x708, 7}, {0x70c, 0}, {0x710, 0}, {0x714, 0}};
    const std::map<uint32_t, uint32_t> bound = {{0x700, gm20b::probeBindOffset(1, 0)},
                                                {0x704, gm20b::probeStorageCode(1, 1)},
                                                {0x708, 9},
                                                {0x70c, 1},
                                                {0x710, 1}};
    const auto words = gm20b::residualWordsByValue(plain, bound);
    EXPECT_EQ(words.at(gm20b::probeBindOffset(1, 0)), 0x700u);
    EXPECT_EQ(words.at(gm20b::probeStorageCode(1, 1)), 0x704u);
    EXPECT_FALSE(words.count(9)) << "set before the run";
    EXPECT_FALSE(words.count(1)) << "two words share it";
}

gm20b::PackKernelEntry kernelBuffer(int word, uint32_t windowBytes = 0)
{
    gm20b::PackKernelEntry e = {};
    e.kind = gm20b::KernelBuffer;
    e.window = windowBytes | uint32_t(word + 1) << 24;
    return e;
}

TEST(GM20BResidual, KernelCodesAreDistinctStorageResiduals)
{
    std::set<uint32_t> codes;
    for (uint32_t slot = 0; slot < 15; slot++)
        EXPECT_TRUE(codes.insert(gm20b::probeComputeCode(slot)).second) << slot;
    for (uint32_t slot = 0; slot < 31; slot++) {
        EXPECT_GT(gm20b::probeComputeCode(slot), 0u);
        EXPECT_LT(gm20b::probeComputeCode(slot), 16u);
    }
}

// boot 15's backboardd: Core Animation's luma kernels write their sums at offsets off 16 (compute a=0, slot 0)
TEST(GM20BResidual, KernelBufferOffSixteenTakesItsWord)
{
    const uint64_t va = 0x200000000ull + 0x1004;
    gm20b::BufferDescriptor d;
    ASSERT_TRUE(gm20b::bufferDescriptor(kernelBuffer(6), va, 0x40, d));
    EXPECT_EQ(gm20b::kernelResidualWord(kernelBuffer(6)), 6);
    EXPECT_EQ(gm20b::kernelBufferWindow(kernelBuffer(6, 0x30)), 0x30u);
    EXPECT_EQ(d.base, va - 4);
    EXPECT_EQ(d.residual, 4u);
    EXPECT_EQ(d.word, 6);
    EXPECT_EQ(d.range, 0x50u);
    EXPECT_FALSE(gm20b::bufferDescriptor(kernelBuffer(-1), va, 0x40, d));
}

TEST(GM20BResidual, KernelBufferWindowAndAlignedOffsets)
{
    gm20b::BufferDescriptor d;
    ASSERT_TRUE(gm20b::bufferDescriptor(kernelBuffer(-1, 0x20), 0x200000100ull, 0x1000, d));
    EXPECT_EQ(d.word, -1);
    EXPECT_EQ(d.base, 0x200000100ull);
    EXPECT_EQ(d.range, 0x20u) << "the function's window, as before residual words";
    ASSERT_TRUE(gm20b::bufferDescriptor(kernelBuffer(-1), 0x200000100ull, 0x34, d));
    EXPECT_EQ(d.range, 0x40u) << "a storage range is the bytes padded to 16, never a cbuf's 256";
    ASSERT_TRUE(gm20b::bufferDescriptor(kernelBuffer(2, 0x20), 0x200000108ull, 0x1000, d));
    EXPECT_EQ(d.base, 0x200000100ull);
    EXPECT_EQ(d.range, 0x30u) << "from the aligned base to where the window ends";
}

}  // namespace
