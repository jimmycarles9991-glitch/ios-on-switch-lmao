// SPDX-License-Identifier: MIT
// raw/runtime/raw_fetch_free.h: which fetch-free form a record names, its words, and which draws take it.
#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/raw/runtime/raw_fetch_free.h"

namespace {

raw::PackPipeline formRecord(uint8_t colour, uint8_t rung)
{
    raw::PackPipeline pp = {};
    pp.flags = raw::PipelineFetchFreeForm;
    pp.fetchRung[colour] = rung;
    return pp;
}

raw::PackIteratedBlend multiply()
{
    raw::PackIteratedBlend b = {};
    b.count = 2;
    b.enable = 1;
    b.name = raw::iteratedProgramName("mpy");
    b.alphaSource = 0x4001;
    b.alphaDest = 0x4303;
    b.program[0] = 0x00091001;
    b.program[1] = 0x00081209;
    return b;
}

TEST(RawFetchFree, ARecordNamesItsRungAndColour)
{
    const raw::PackIteratedBlend b = multiply();
    const raw::FetchFreeForm iterated = raw::fetchFreeForm(formRecord(0, raw::FetchIteratedBlend), &b);
    EXPECT_EQ(iterated.rung, raw::FetchIteratedBlend);
    EXPECT_EQ(iterated.colour, 0);
    EXPECT_EQ(iterated.programWords, 2);
    const raw::FetchFreeForm blend = raw::fetchFreeForm(formRecord(2, raw::FetchBlend), nullptr);
    EXPECT_EQ(blend.rung, raw::FetchBlend);
    EXPECT_EQ(blend.colour, 2);
}

TEST(RawFetchFree, AnIteratedFormWithoutItsProgramIsNone)
{
    EXPECT_EQ(raw::fetchFreeForm(formRecord(0, raw::FetchIteratedBlend), nullptr).rung, raw::FetchNone);
    raw::PackIteratedBlend empty = {};
    empty.uniformSlot = raw::kNoUniform;
    EXPECT_EQ(raw::fetchFreeForm(formRecord(0, raw::FetchIteratedBlend), &empty).rung, raw::FetchNone);
    raw::PackPipeline storage = formRecord(0, raw::FetchBlend);
    storage.flags = raw::PipelineStorageMetadata;
    EXPECT_EQ(raw::fetchFreeForm(storage, nullptr).rung, raw::FetchNone);
}

TEST(RawFetchFree, AColourMatrixFormReadsItsRowsAndTheClampPairWhereItsUniformHasOne)
{
    raw::PackIteratedBlend matrix = {};
    matrix.uniformSlot = 1;
    matrix.uniformOffset = 0xb0;
    for (const uint8_t words : {uint8_t(10), uint8_t(11)}) {
        matrix.uniformWords = words;
        const raw::FetchFreeForm f = raw::fetchFreeForm(formRecord(0, raw::FetchIteratedBlend), &matrix);
        EXPECT_EQ(f.rung, raw::FetchIteratedBlend);
        EXPECT_EQ(f.matrixSlot, 1);
        EXPECT_EQ(f.matrixOffset, 0xb0);
        EXPECT_EQ(f.matrixWords, words);
    }
    matrix.uniformWords = 9;
    EXPECT_EQ(raw::fetchFreeForm(formRecord(0, raw::FetchIteratedBlend), &matrix).rung, raw::FetchNone);
}

TEST(RawFetchFree, TheIteratedWordsEnableTheColoursBlendLoadTheProgramAndTurnOffAfter)
{
    std::vector<uint32_t> on, off;
    raw::iteratedBlendWords(multiply(), 0, on, off);
    ASSERT_GE(on.size(), 4u);
    EXPECT_EQ(on[0], raw::Push::hdr(1, raw::SUBC_3D, 0x1360, 1));  // SET_BLEND(0)
    EXPECT_EQ(on[1], 1u);
    const auto has = [&](uint32_t word) {
        return std::find(on.begin(), on.end(), word) != on.end();
    };
    EXPECT_TRUE(has(0x00091001));
    EXPECT_TRUE(has(0x00081209));
    EXPECT_FALSE(off.empty());
}

TEST(RawFetchFree, TargetSelectMapsEachSlotToItself)
{
    EXPECT_EQ(raw::targetSelect(1) & 0xf, 1u);
    EXPECT_EQ(raw::targetSelect(3) & 0xf, 3u);
    for (uint32_t j = 0; j < 8; j++)
        EXPECT_EQ(raw::targetSelect(3) >> (4 + 3 * j) & 7, j);
}

TEST(RawFetchFree, Rung2TakesAnEightBitScreenTargetTwoLayersDeep)
{
    const raw::FetchFreeForm f = raw::fetchFreeForm(formRecord(0, raw::FetchBlend), nullptr);
    EXPECT_EQ(raw::fetchFreeTaken(f, {80, false, 0}), raw::fixedBlendProven());
    EXPECT_EQ(raw::fetchFreeTaken(f, {80, false, 1}), raw::fixedBlendProven());
    EXPECT_FALSE(raw::fetchFreeTaken(f, {80, false, 2}));
    EXPECT_FALSE(raw::fetchFreeTaken(f, {80, true, 0}));
    EXPECT_FALSE(raw::fetchFreeTaken(f, {115, false, 0}));
    EXPECT_FALSE(raw::fetchFreeTaken(f, {80, false, 0}, 4));
}

TEST(RawFetchFree, Rung3WaitsForItsPassesToBeProvenUnlessAProbeForcesIt)
{
    const raw::PackIteratedBlend b = multiply();
    const raw::FetchFreeForm f = raw::fetchFreeForm(formRecord(0, raw::FetchIteratedBlend), &b);
    EXPECT_EQ(f.program, b.name);
    EXPECT_EQ(raw::iteratedPassesProven(f), raw::iteratedProgramProven(b.name));
    EXPECT_EQ(raw::fetchFreeTaken(f, {80, false, 0}), raw::iteratedPassesProven(f));
    EXPECT_TRUE(raw::fetchFreeTaken(f, {80, false, 0}, 3));
    EXPECT_EQ(raw::fetchFreeTaken(f, {80, false, 0}, 2), raw::iteratedPassesProven(f));
}

}  // namespace
