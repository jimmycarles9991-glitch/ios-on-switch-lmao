// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.h: which private textures bind compressed memory, and of which kind.
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_storage_form.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

// An attachment as the storage view sees it
struct Target {
    uint32_t bpp = 4, samples = 1;
    bool comptags = false;
};

// What a draw of a state with `storageBits` (and a barrier form or not) takes over `t`: its storage form ("raw"), its
// barrier form ("twin"), or the refusal that sends it to m2v ("enc.storage"), once the plain move ran before the encode
std::string route(uint8_t storageBits, bool twin, Target &t)
{
    if (raw::storageSlotsToPlain(storageBits, twin) & 1)
        t.comptags = false;
    const auto reason = raw::storageViewMismatch(4, 1, t);
    if (reason == raw::StorageFallback::None)
        return "raw";
    return twin ? "twin" : "enc.storage";
}

raw::PrivateTextureFacts target(uint32_t bpp)
{
    raw::PrivateTextureFacts f;
    f.privateStorage = f.oneLevel = f.oneSample = f.twoD = f.renderedAndSampledOnly = f.plainColour = true;
    f.compression = true;
    f.bpp = bpp;
    return f;
}

TEST(RawPrivateComp, FourAndEightByteTargetsTakeTheProvenColourKinds)
{
    EXPECT_EQ(raw::privateCompressedKind(target(4)), uint32_t(raw::kCompKindC32));
    EXPECT_EQ(raw::privateCompressedKind(target(8)), uint32_t(raw::kCompKindC64));
    EXPECT_EQ(raw::privateCompressedKind(target(16)), 0u) << "C128: the copy engine was not asked";
    EXPECT_EQ(raw::privateCompressedKind(target(2)), 0u);
}

TEST(RawPrivateComp, AnythingElseStaysPlain)
{
    bool raw::PrivateTextureFacts::*fields[] = {
        &raw::PrivateTextureFacts::privateStorage,
        &raw::PrivateTextureFacts::oneLevel,
        &raw::PrivateTextureFacts::oneSample,
        &raw::PrivateTextureFacts::twoD,
        &raw::PrivateTextureFacts::renderedAndSampledOnly,
        &raw::PrivateTextureFacts::plainColour,
        &raw::PrivateTextureFacts::compression,
    };
    for (auto field : fields) {
        raw::PrivateTextureFacts f = target(4);
        f.*field = false;
        EXPECT_EQ(raw::privateCompressedKind(f), 0u);
    }
}

TEST(RawPrivateComp, AFetchingStateWithNoBarrierFormDrawsAPlainTargetOnTheRawPath)
{
    // gpu.raw.fetch_blend_order strip16 (rgba8, 64x64 private RenderTarget|ShaderRead): boot 11's sto=compressed,notwin
    Target strip{4, 1, true};
    EXPECT_EQ(raw::storageViewMismatch(4, 1, strip), raw::StorageFallback::Compressed);
    EXPECT_EQ(route(0x1, false, strip), "raw");
    EXPECT_FALSE(strip.comptags);
    // gpu.raw.pass_fusion private_color_adjacent: a clear pass and a load pass on one target, the fetch in both
    Target adjacent{4, 1, true};
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
}

TEST(RawPrivateComp, AStateWithABarrierFormKeepsTheCompressedTarget)
{
    Target t{4, 1, true};
    EXPECT_EQ(raw::storageSlotsToPlain(0x3, true), 0);
    EXPECT_EQ(route(0x1, true, t), "twin");
    EXPECT_TRUE(t.comptags);
    EXPECT_EQ(raw::storageSlotsToPlain(0x6, false), 0x6);
}

// A compositor drawing a no-twin fetch pass over a private target every frame, keeping the target or making a new one
// of the same shape each frame: the move runs once, and the target, or every later one, is plain from then on
uint32_t movesOver(uint32_t frames, bool newTargetEachFrame)
{
    raw::PlainShapes plain;
    const raw::PrivateShape shape{70, 64, 64, 5};
    uint32_t moves = 0;
    Target target;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (!frame || newTargetEachFrame)
            target = Target{4, 1, !plain.holds(shape)};
        if (raw::storageSlotsToPlain(0x1, false) & 1 && target.comptags) {
            plain.add(shape);
            target.comptags = false;
            ++moves;
        }
        EXPECT_EQ(raw::storageViewMismatch(4, 1, target), raw::StorageFallback::None) << "frame " << frame;
    }
    return moves;
}

TEST(RawPrivateComp, AStorageTargetMovesOnceEverAndItsShapeIsMadePlainFromThen)
{
    EXPECT_EQ(movesOver(1, false), 1u);
    EXPECT_EQ(movesOver(2, false), 1u) << "the second frame does no move";
    EXPECT_EQ(movesOver(100, false), 1u);
    EXPECT_EQ(movesOver(2, true), 1u) << "the second frame's new target is made plain";
    EXPECT_EQ(movesOver(100, true), 1u);
    raw::PlainShapes plain;
    plain.add({70, 64, 64, 5});
    EXPECT_FALSE(plain.holds({70, 64, 32, 5}));
    EXPECT_FALSE(plain.holds({80, 64, 64, 5}));
}

TEST(RawPrivateComp, TheCheckIsAFlagTestPerPassAtRecordAndTheMoveRunsBeforeTheRawEncode)
{
    const std::string record = SourceText("../raw/runtime/RawMetalCommandBuffer.mm");
    EXPECT_THAT(record, HasSubstr("if (cmd.op == OpPipeline && _recordedPass < _passes.size() && "
                                  "_passes[_recordedPass].compressed)\n        rmNoteStorageTargets(self, "
                                  "_passes[_recordedPass], cmd);"));
    const std::string pass = SourceText("../raw/runtime/RawMetalEncoder.mm");
    EXPECT_THAT(pass, HasSubstr("facts.compressed |= uint8_t(1u << c);"));
    const std::string submit = SourceText("../raw/runtime/RawMetalSubmit.mm");
    EXPECT_THAT(submit, HasSubstr("rmStorageTargetsPlain(cb, range.from, range.to);\n        const bool encoded = "
                                  "encodeRaw(cb, range.from, range.to, &why, &refusedEncoder);"));
    const std::string facade = SourceText("../raw/runtime/RawMetalPrivateComp.mm");
    const std::string note = Body(facade, "static void notePipeline(");
    EXPECT_THAT(note,
                HasSubstr("raw::storageSlotsToPlain(pl->storageBits, pl->barrierTwin != nullptr) & pass.compressed"));
    EXPECT_THAT(note, HasSubstr("cb->_scanStorageTargets = true;")) << "a variant still being made is looked at later";
    const std::string move = Body(facade, "void rmStorageTargetsPlain(");
    EXPECT_THAT(move, HasSubstr("if (cb->_scanStorageTargets)"));
    EXPECT_THAT(move, HasSubstr("gPlainShapes.add("));
    EXPECT_THAT(move, HasSubstr("rmTexturePlain(t)"));
    EXPECT_THAT(Body(facade, "uint32_t privateCompressedKind("), HasSubstr("gPlainShapes.holds("));
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalEncode.mm"), HasSubstr("tRawEncoding = true;"));
}

}  // namespace
