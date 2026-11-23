// SPDX-License-Identifier: MIT
// written_textures.h: which bound formats a state's record was not compiled for, the variant's name the
// runtime and the pack builder both give it, and the check a made variant passes. RenderBox's uber_resolve_fragment writes
// a half texture at slot 0 and a 565 code at slot 1; the probe compiled them for BGRA8Unorm (80) and RGBA8Uint (73).
// RenderBox resolves a B5G6R5Unorm or BGR5A1Unorm target through an R16Uint (23) texture; slot 1 is the function's only
// ushort texture, so that is where it binds it.
#include <gtest/gtest.h>
#include "../written_textures.h"

namespace {

gm20b::PackPipeline uberResolve()
{
    gm20b::PackPipeline rec = {};
    rec.writtenFormat[0] = 80;
    rec.writtenFormat[1] = 73;
    return rec;
}

TEST(WrittenTextures, ABoundFormatTheRecordWasCompiledForNeedsNoVariant)
{
    const gm20b::PackPipeline rec = uberResolve();
    const auto none = gm20b::reboundWritten(rec, [](uint32_t slot) {
        return slot == 0 ? 80u : slot == 1 ? 73u : 0u;
    });
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(gm20b::writtenVariant(none), "");
}

TEST(WrittenTextures, AnotherFormatNamesTheVariantOfThatSlotAlone)
{
    const gm20b::PackPipeline rec = uberResolve();
    const auto packed = gm20b::reboundWritten(rec, [](uint32_t slot) {
        return slot == 1 ? 23u : 0u;
    });
    ASSERT_EQ(packed.size(), 1u);
    EXPECT_EQ(packed[0], (gm20b::WrittenTexture{1, 23}));
    EXPECT_EQ(gm20b::writtenVariant(packed), " | wtex=1:23");
    const auto both = gm20b::reboundWritten(rec, [](uint32_t slot) {
        return slot == 0 ? 25u : slot == 1 ? 23u : 0u;
    });
    EXPECT_EQ(gm20b::writtenVariant(both), " | wtex=0:25,1:23");
}

TEST(WrittenTextures, ASlotTheRecordOnlySamplesIsNoWrittenSlot)
{
    const gm20b::PackPipeline rec = uberResolve();
    EXPECT_EQ(gm20b::writtenSlots(rec), 0b11);
    const auto none = gm20b::reboundWritten(rec, [](uint32_t slot) {
        return slot == 3 ? 115u : 0u;
    });
    EXPECT_TRUE(none.empty());
}

TEST(WrittenTextures, TheNameParsesBackAsTheCorpusWritesIt)
{
    std::vector<gm20b::WrittenTexture> parsed;
    ASSERT_TRUE(gm20b::parseWritten("wtex=0:25,1:23", parsed));
    EXPECT_EQ(gm20b::writtenVariant(parsed), " | wtex=0:25,1:23");
    EXPECT_FALSE(gm20b::parseWritten("wtex=1:23,0:25", parsed));
    EXPECT_FALSE(gm20b::parseWritten("wtex=16:23", parsed));
    EXPECT_FALSE(gm20b::parseWritten("wtex=1:", parsed));
    EXPECT_FALSE(gm20b::parseWritten("wtex=1:23,", parsed));
    EXPECT_FALSE(gm20b::parseWritten("xrs=2", parsed));
}

TEST(WrittenTextures, AMadeVariantHoldsOnlyWhereItWasCompiledForTheBoundFormats)
{
    gm20b::PackPipeline made = uberResolve();
    made.writtenFormat[1] = 23;
    EXPECT_TRUE(gm20b::compiledFor(made, {{1, 23}}));
    EXPECT_FALSE(gm20b::compiledFor(uberResolve(), {{1, 23}}));
    EXPECT_FALSE(gm20b::compiledFor(made, {{2, 23}}));
    EXPECT_TRUE(gm20b::compiledFor(made, {}));
}

TEST(WrittenTextures, TheVariantNameIsAKeysLastVariantSegment)
{
    const std::string key = "c0=80:15 | v plane__vertex | f uber_resolve_fragment | l RenderBox";
    EXPECT_EQ(gm20b::variantNameAt(key + " | wtex=1:23"), key.size());
    EXPECT_EQ(gm20b::variantNameAt(key + " | xrs=2 | wtex=1:23"), key.size());
}

}  // namespace
