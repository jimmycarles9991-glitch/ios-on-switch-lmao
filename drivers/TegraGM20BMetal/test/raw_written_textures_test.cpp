// SPDX-License-Identifier: MIT
// raw/runtime/raw_written_textures.h: which bound formats a state's record was not compiled for, the variant's name the
// runtime and the pack builder both give it, and the check a made variant passes. RenderBox's uber_resolve_fragment writes
// a half texture at slot 0 and a 565 code at slot 1; the probe compiled them for BGRA8Unorm (80) and RGBA8Uint (73).
// RenderBox resolves a B5G6R5Unorm or BGR5A1Unorm target through an R16Uint (23) texture; slot 1 is the function's only
// ushort texture, so that is where it binds it.
#include <gtest/gtest.h>
#include "../raw/runtime/raw_written_textures.h"

namespace {

raw::PackPipeline uberResolve()
{
    raw::PackPipeline rec = {};
    rec.writtenFormat[0] = 80;
    rec.writtenFormat[1] = 73;
    return rec;
}

TEST(WrittenTextures, ABoundFormatTheRecordWasCompiledForNeedsNoVariant)
{
    const raw::PackPipeline rec = uberResolve();
    const auto none = raw::reboundWritten(rec, [](uint32_t slot) {
        return slot == 0 ? 80u : slot == 1 ? 73u : 0u;
    });
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(raw::writtenVariant(none), "");
}

TEST(WrittenTextures, AnotherFormatNamesTheVariantOfThatSlotAlone)
{
    const raw::PackPipeline rec = uberResolve();
    const auto packed = raw::reboundWritten(rec, [](uint32_t slot) {
        return slot == 1 ? 23u : 0u;
    });
    ASSERT_EQ(packed.size(), 1u);
    EXPECT_EQ(packed[0], (raw::WrittenTexture{1, 23}));
    EXPECT_EQ(raw::writtenVariant(packed), " | wtex=1:23");
    const auto both = raw::reboundWritten(rec, [](uint32_t slot) {
        return slot == 0 ? 25u : slot == 1 ? 23u : 0u;
    });
    EXPECT_EQ(raw::writtenVariant(both), " | wtex=0:25,1:23");
}

TEST(WrittenTextures, ASlotTheRecordOnlySamplesIsNoWrittenSlot)
{
    const raw::PackPipeline rec = uberResolve();
    EXPECT_EQ(raw::writtenSlots(rec), 0b11);
    const auto none = raw::reboundWritten(rec, [](uint32_t slot) {
        return slot == 3 ? 115u : 0u;
    });
    EXPECT_TRUE(none.empty());
}

TEST(WrittenTextures, TheNameParsesBackAsTheCorpusWritesIt)
{
    std::vector<raw::WrittenTexture> parsed;
    ASSERT_TRUE(raw::parseWritten("wtex=0:25,1:23", parsed));
    EXPECT_EQ(raw::writtenVariant(parsed), " | wtex=0:25,1:23");
    EXPECT_FALSE(raw::parseWritten("wtex=1:23,0:25", parsed));
    EXPECT_FALSE(raw::parseWritten("wtex=16:23", parsed));
    EXPECT_FALSE(raw::parseWritten("wtex=1:", parsed));
    EXPECT_FALSE(raw::parseWritten("wtex=1:23,", parsed));
    EXPECT_FALSE(raw::parseWritten("xrs=2", parsed));
}

TEST(WrittenTextures, AMadeVariantHoldsOnlyWhereItWasCompiledForTheBoundFormats)
{
    raw::PackPipeline made = uberResolve();
    made.writtenFormat[1] = 23;
    EXPECT_TRUE(raw::compiledFor(made, {{1, 23}}));
    EXPECT_FALSE(raw::compiledFor(uberResolve(), {{1, 23}}));
    EXPECT_FALSE(raw::compiledFor(made, {{2, 23}}));
    EXPECT_TRUE(raw::compiledFor(made, {}));
}

TEST(WrittenTextures, TheVariantNameIsAKeysLastVariantSegment)
{
    const std::string key = "c0=80:15 | v plane__vertex | f uber_resolve_fragment | l RenderBox";
    EXPECT_EQ(raw::variantNameAt(key + " | wtex=1:23"), key.size());
    EXPECT_EQ(raw::variantNameAt(key + " | xrs=2 | wtex=1:23"), key.size());
}

}  // namespace
