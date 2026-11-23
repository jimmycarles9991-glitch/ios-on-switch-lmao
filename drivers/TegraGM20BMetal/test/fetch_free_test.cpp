// SPDX-License-Identifier: MIT
// fetch_free.h: which fetch-free form a record names, its words, and which draws take it.
#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

#include "drivers/TegraGM20BMetal/fetch_free.h"

namespace {

gm20b::PackPipeline formRecord(uint8_t colour, uint8_t rung)
{
    gm20b::PackPipeline pp = {};
    pp.flags = gm20b::PipelineFetchFreeForm;
    pp.fetchRung[colour] = rung;
    return pp;
}

gm20b::PackIteratedBlend multiply()
{
    gm20b::PackIteratedBlend b = {};
    b.count = 2;
    b.enable = 1;
    b.name = gm20b::iteratedProgramName("mpy");
    b.alphaSource = 0x4001;
    b.alphaDest = 0x4303;
    b.program[0] = 0x00091001;
    b.program[1] = 0x00081209;
    return b;
}

TEST(GM20BFetchFree, ARecordNamesItsRungAndColour)
{
    const gm20b::PackIteratedBlend b = multiply();
    const gm20b::FetchFreeForm iterated = gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), &b);
    EXPECT_EQ(iterated.rung, gm20b::FetchIteratedBlend);
    EXPECT_EQ(iterated.colour, 0);
    EXPECT_EQ(iterated.programWords, 2);
    const gm20b::FetchFreeForm blend = gm20b::fetchFreeForm(formRecord(2, gm20b::FetchBlend), nullptr);
    EXPECT_EQ(blend.rung, gm20b::FetchBlend);
    EXPECT_EQ(blend.colour, 2);
}

TEST(GM20BFetchFree, AnIteratedFormWithoutItsProgramIsNone)
{
    EXPECT_EQ(gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), nullptr).rung, gm20b::FetchNone);
    gm20b::PackIteratedBlend empty = {};
    empty.uniformSlot = gm20b::kNoUniform;
    EXPECT_EQ(gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), &empty).rung, gm20b::FetchNone);
    gm20b::PackPipeline storage = formRecord(0, gm20b::FetchBlend);
    storage.flags = gm20b::PipelineStorageMetadata;
    EXPECT_EQ(gm20b::fetchFreeForm(storage, nullptr).rung, gm20b::FetchNone);
}

TEST(GM20BFetchFree, AColourMatrixFormReadsItsRowsAndTheClampPairWhereItsUniformHasOne)
{
    gm20b::PackIteratedBlend matrix = {};
    matrix.uniformSlot = 1;
    matrix.uniformOffset = 0xb0;
    for (const uint8_t words : {uint8_t(10), uint8_t(11)}) {
        matrix.uniformWords = words;
        const gm20b::FetchFreeForm f = gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), &matrix);
        EXPECT_EQ(f.rung, gm20b::FetchIteratedBlend);
        EXPECT_EQ(f.matrixSlot, 1);
        EXPECT_EQ(f.matrixOffset, 0xb0);
        EXPECT_EQ(f.matrixWords, words);
    }
    matrix.uniformWords = 9;
    EXPECT_EQ(gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), &matrix).rung, gm20b::FetchNone);
}

TEST(GM20BFetchFree, TheIteratedWordsEnableTheColoursBlendLoadTheProgramAndTurnOffAfter)
{
    std::vector<uint32_t> on, off;
    gm20b::iteratedBlendWords(multiply(), 0, 80, on, off);
    ASSERT_GE(on.size(), 4u);
    EXPECT_EQ(on[0], gm20b::Push::hdr(1, gm20b::SUBC_3D, 0x1360, 1));  // SET_BLEND(0)
    EXPECT_EQ(on[1], 1u);
    const auto has = [&](uint32_t word) {
        return std::find(on.begin(), on.end(), word) != on.end();
    };
    EXPECT_TRUE(has(0x00091001));
    EXPECT_TRUE(has(0x00081209));
    EXPECT_FALSE(off.empty());
}

// The iterated blend takes the shader's colour unclamped, where Metal's blend into an 8-bit target clamps it first
uint32_t saturateAfter(const std::vector<uint32_t> &words)
{
    const uint32_t header = gm20b::Push::hdr(1, gm20b::SUBC_3D, 0x13a8, 1);  // SET_PS_SATURATE
    const auto at = std::find(words.begin(), words.end(), header);
    return at == words.end() || at + 1 == words.end() ? ~0u : at[1];
}

TEST(GM20BFetchFree, TheIteratedFormSaturatesAnEightBitColourAndTheNextStateDoesNot)
{
    for (uint8_t colour : {0, 2}) {
        std::vector<uint32_t> on, off;
        gm20b::iteratedBlendWords(multiply(), colour, 80, on, off);
        EXPECT_EQ(saturateAfter(on), 1u << (4 * colour)) << "OUTPUT" << int(colour) << " TRUE, CLAMP_RANGE 0 to 1";
        EXPECT_EQ(saturateAfter(off), 0u);
    }
    gm20b::PackIteratedBlend matrix = {};
    matrix.uniformSlot = 1;
    std::vector<uint32_t> on, off;
    gm20b::iteratedBlendWords(matrix, 0, 70, on, off);
    EXPECT_EQ(saturateAfter(on), 1u);
    EXPECT_EQ(saturateAfter(off), 0u);
}

TEST(GM20BFetchFree, AFloatTargetsIteratedFormKeepsItsColourUnclamped)
{
    std::vector<uint32_t> on, off;
    gm20b::iteratedBlendWords(multiply(), 0, 115, on, off);  // RGBA16Float
    EXPECT_EQ(saturateAfter(on), ~0u) << "no SET_PS_SATURATE: Metal blends a float target's colour as it is";
    EXPECT_EQ(saturateAfter(off), ~0u);
}

TEST(GM20BFetchFree, TargetSelectMapsEachSlotToItself)
{
    EXPECT_EQ(gm20b::targetSelect(1) & 0xf, 1u);
    EXPECT_EQ(gm20b::targetSelect(3) & 0xf, 3u);
    for (uint32_t j = 0; j < 8; j++)
        EXPECT_EQ(gm20b::targetSelect(3) >> (4 + 3 * j) & 7, j);
}

TEST(GM20BFetchFree, Rung2TakesAnEightBitScreenTargetTwoLayersDeep)
{
    const gm20b::FetchFreeForm f = gm20b::fetchFreeForm(formRecord(0, gm20b::FetchBlend), nullptr);
    EXPECT_EQ(gm20b::fetchFreeTaken(f, {80, false, 0}), gm20b::fixedBlendProven());
    EXPECT_EQ(gm20b::fetchFreeTaken(f, {80, false, 1}), gm20b::fixedBlendProven());
    EXPECT_FALSE(gm20b::fetchFreeTaken(f, {80, false, 2}));
    EXPECT_FALSE(gm20b::fetchFreeTaken(f, {80, true, 0}));
    EXPECT_FALSE(gm20b::fetchFreeTaken(f, {90, false, 0}));
    EXPECT_FALSE(gm20b::fetchFreeTaken(f, {80, false, 0}, 4));
}

TEST(GM20BFetchFree, APipelinePlacesTheFormsItsRuleTakesAndAnyTheProbeAsksFor)
{
    gm20b::PackIteratedBlend b = multiply();
    const gm20b::FetchFreeForm listed = gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), &b);
    EXPECT_TRUE(gm20b::fetchFreePlaced(listed));
    b.name = gm20b::iteratedProgramName("hue");
    const gm20b::FetchFreeForm unlisted = gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), &b);
    EXPECT_FALSE(gm20b::fetchFreePlaced(unlisted)) << "release and debug place the same forms";
    for (uint8_t probe : {2, 3, 4})
        EXPECT_TRUE(gm20b::fetchFreePlaced(unlisted, probe));
    EXPECT_FALSE(gm20b::fetchFreePlaced(gm20b::FetchFreeForm{}, 3));
}

TEST(GM20BFetchFree, AFormKnowsTheFragmentTexturesItSamples)
{
    gm20b::PackPipeline pp = formRecord(0, gm20b::FetchBlend);
    gm20b::setEntryCount(pp, 3);
    pp.entry[0] = {0, gm20b::EntryTexture, 2, 3, 0, 0, 0};
    pp.entry[1] = {8, gm20b::EntryTexture, 1, 5, 0, 0, 0};
    pp.entry[2] = {16, gm20b::EntryUbo, 2, 0, 0, 0, 0};
    const gm20b::FetchFreeForm f = gm20b::fetchFreeForm(pp, nullptr);
    EXPECT_EQ(f.sampledSlots, 1u << 3) << "the fragment stage's texture alone";
    EXPECT_FALSE(f.samplesPast32);
}

// 24B119's fragment slots 32 to 127: a form sampling one is never a draw of eight-bit sources, whatever it binds there
TEST(GM20BFetchFree, AFormSamplingAFragmentSlotPast31KnowsItSamplesOneSampledSlotsCannotName)
{
    gm20b::PackPipeline pp = formRecord(0, gm20b::FetchBlend);
    gm20b::setEntryCount(pp, 3);
    pp.entry[0] = {0, gm20b::EntryTexture, 2, 31, 0, 0, 0};
    pp.entry[1] = {8, gm20b::EntryTexture, 2, 64, 0, 0, 0};
    pp.entry[2] = {16, gm20b::EntryTexture, 1, 127, 0, 0, 0};
    const gm20b::FetchFreeForm f = gm20b::fetchFreeForm(pp, nullptr);
    EXPECT_EQ(f.sampledSlots, 1u << 31);
    EXPECT_TRUE(f.samplesPast32);
}

struct Image {
    uint32_t format = 0;
};

TEST(GM20BFetchFree, EightBitSourcesAreSamplesOfUnorm8ImagesOnly)
{
    const Image bgra{80}, rgba{70}, srgb{81}, half{115};
    const Image *bound[4] = {&bgra, &rgba, &srgb, &half};
    EXPECT_TRUE(gm20b::eightBitSamples(0b11u, bound));
    EXPECT_FALSE(gm20b::eightBitSamples(0b101u, bound)) << "sRGB samples are decoded off the codes";
    EXPECT_FALSE(gm20b::eightBitSamples(0b1001u, bound));
    EXPECT_FALSE(gm20b::eightBitSamples(0u, bound)) << "a colour no image gives";
    const Image *none[1] = {nullptr};
    EXPECT_FALSE(gm20b::eightBitSamples(1u, none));
}

// x1-units UNITS.md: 8-bit sources within 2 codes at any depth, computed colour 2 layers deep; never re-blended frames
TEST(GM20BFetchFree, EightBitSourcesTakeRung2AtAnyDepthUnlessTheTargetAccumulates)
{
    const gm20b::FetchFreeForm f = gm20b::fetchFreeForm(formRecord(0, gm20b::FetchBlend), nullptr);
    gm20b::FetchFreeDraw draw = {80, false, 7};
    EXPECT_FALSE(gm20b::fetchFreeTaken(f, draw));
    draw.eightBitSources = true;
    EXPECT_EQ(gm20b::fetchFreeTaken(f, draw), gm20b::fixedBlendProven());
    draw.accumulates = true;
    EXPECT_FALSE(gm20b::fetchFreeTaken(f, draw));
    draw.ropLayers = 0;
    EXPECT_FALSE(gm20b::fetchFreeTaken(f, draw));
}

TEST(GM20BFetchFree, Rung3WaitsForItsPassesToBeProvenUnlessAProbeForcesIt)
{
    const gm20b::PackIteratedBlend b = multiply();
    const gm20b::FetchFreeForm f = gm20b::fetchFreeForm(formRecord(0, gm20b::FetchIteratedBlend), &b);
    EXPECT_EQ(f.program, b.name);
    EXPECT_EQ(gm20b::iteratedPassesProven(f), gm20b::iteratedProgramProven(b.name));
    EXPECT_EQ(gm20b::fetchFreeTaken(f, {80, false, 0}), gm20b::iteratedPassesProven(f));
    EXPECT_TRUE(gm20b::fetchFreeTaken(f, {80, false, 0}, 3));
    EXPECT_EQ(gm20b::fetchFreeTaken(f, {80, false, 0}, 2), gm20b::iteratedPassesProven(f));
}

}  // namespace
