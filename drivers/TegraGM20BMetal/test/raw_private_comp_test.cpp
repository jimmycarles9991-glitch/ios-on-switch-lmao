// SPDX-License-Identifier: MIT
// raw/runtime/raw_private_comp.h: which private textures bind compressed memory, and of which kind.
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_fetch_order.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h"
#include "drivers/TegraGM20BMetal/raw/runtime/raw_storage_form.h"

namespace {

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

// An attachment as the storage view sees it
struct Target {
    uint32_t bpp = 4, samples = 1;
    bool comptags = false;
};

// A list of two triangles, which a barrier form draws a triangle at a time
constexpr raw::FetchDraw kList = {3, 6, 1, false, false};

// What a draw of shape `draw` of a state with `storageBits` (and a barrier form that fetches the colour it writes or not)
// takes over `t`: its storage form ("raw"), its barrier form ("twin"), or the refusal that sends it to m2v ("enc.storage",
// "enc.geometry"), once the plain move ran before the encode
std::string route(uint8_t storageBits, bool twin, Target &t, const raw::FetchDraw &draw = kList,
                  bool compressedOk = raw::storageReadsCompressed())
{
    const bool twinDraws = raw::barrierFormDraws(0x1, 0x1, 0, false, draw);
    if (raw::storageSlotsToPlain(storageBits, twin, twinDraws, compressedOk) & 1)
        t.comptags = false;
    const auto reason = raw::storageViewMismatch(4, 1, t, compressedOk);
    if (reason == raw::StorageFallback::None)
        return "raw";
    if (!twin)
        return "enc.storage";
    return twinDraws ? "twin" : "enc.geometry";
}

raw::PrivateTextureFacts target(uint32_t bpp)
{
    raw::PrivateTextureFacts f;
    f.privateStorage = f.compressibleUsage = f.plainColour = true;
    f.shape = raw::PrivateShape2D::Plain;
    f.compression = true;
    f.bpp = bpp;
    return f;
}

TEST(RawPrivateComp, TargetsTakeTheColourKindsTheCopyEngineReads)
{
    EXPECT_EQ(raw::privateCompressedKind(target(4)), uint32_t(raw::kCompKindC32));
    EXPECT_EQ(raw::privateCompressedKind(target(8)), uint32_t(raw::kCompKindC64));
    EXPECT_EQ(raw::privateCompressedKind(target(16)), uint32_t(raw::kCompKindC128)) << "RGBA32Float";
    EXPECT_EQ(raw::privateCompressedKind(target(2)), 0u);
}

TEST(RawPrivateComp, AnythingElseStaysPlain)
{
    bool raw::PrivateTextureFacts::*fields[] = {
        &raw::PrivateTextureFacts::privateStorage,
        &raw::PrivateTextureFacts::compressibleUsage,
        &raw::PrivateTextureFacts::plainColour,
        &raw::PrivateTextureFacts::compression,
    };
    for (auto field : fields) {
        raw::PrivateTextureFacts f = target(4);
        f.*field = false;
        EXPECT_EQ(raw::privateCompressedKind(f), 0u);
    }
    raw::PrivateTextureFacts other = target(4);
    other.shape = raw::PrivateShape2D::Other;
    EXPECT_EQ(raw::privateCompressedKind(other), 0u) << "a cube, a 3D or a 1D texture";
}

TEST(RawPrivateComp, ShaderWriteUsageIsTakenWithStorageReadingCompressed)
{
    const std::string facade =
        Body(SourceText("../raw/runtime/RawMetalPrivateComp.mm"), "uint32_t privateCompressedKind(");
    EXPECT_THAT(facade,
                HasSubstr("MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite"))
        << "gpu.raw.private_compression shader_write";
    EXPECT_TRUE(raw::storageReadsCompressed());
}

TEST(RawPrivateComp, OnlyAnM2vTwinMovesACompressedTextureToPlainMemory)
{
    EXPECT_THAT(Body(SourceText("../raw/runtime/RawMetalResourceIds.mm"), "- (MTLResourceID)gpuResourceID"),
                Not(HasSubstr("rmTexturePlain")));
    const std::string texture = SourceText("../raw/runtime/RawMetalTexture.mm");
    EXPECT_EQ(texture.find("rmTexturePlain(self);"), texture.rfind("rmTexturePlain(self);")) << "rmTwin's alone";
    EXPECT_THAT(Body(texture, "- (id)rmTwin"), HasSubstr("rmTexturePlain(self);"));
}

TEST(RawPrivateComp, AMipChainTakesItsKind)
{
    raw::PrivateTextureFacts f = target(4);
    f.levels = 5;
    EXPECT_EQ(raw::privateCompressedKind(f), uint32_t(raw::kCompKindC32)) << "gpu.raw.private_compression mips";
}

TEST(RawPrivateComp, AOneLevelArrayTakesItsKind)
{
    raw::PrivateTextureFacts f = target(8);
    f.shape = raw::PrivateShape2D::Array;
    EXPECT_EQ(raw::privateCompressedKind(f), uint32_t(raw::kCompKindC64)) << "gpu.raw.private_compression array";
    f.levels = 2;
    EXPECT_EQ(raw::privateCompressedKind(f), 0u) << "an array's levels are no chain the move copies";
}

// A stored multisampled target takes its sample count's kind where the copy engine reads that kind (the move to plain
// memory is its copy of the sample grid)
TEST(RawPrivateComp, AStoredMultisampledTargetTakesItsKindWhereTheCopyEngineReadsIt)
{
    raw::PrivateTextureFacts f = target(4);
    f.shape = raw::PrivateShape2D::Multisample;
    f.samples = 4;
    const uint32_t kind = raw::compressedKind(4, 0, 4);
    EXPECT_EQ(raw::privateCompressedKind(f), raw::copyEngineReadsCompressed(kind) ? kind : 0u);
    EXPECT_TRUE(raw::privateShapeCompresses(f)) << "gpu.raw.private_compression ms4";
    f.samples = 8;
    EXPECT_FALSE(raw::privateShapeCompresses(f));
}

// The copy engine moves a multisampled texture as fx::msLayout's grid of its samples
TEST(RawPrivateComp, AMultisampledTextureMovesAsItsSampleGrid)
{
    uint32_t sx = 9, sy = 9;
    raw::sampleGrid(1, sx, sy);
    EXPECT_EQ(sx, 0u);
    EXPECT_EQ(sy, 0u);
    raw::sampleGrid(2, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 0u);
    raw::sampleGrid(4, sx, sy);
    EXPECT_EQ(sx, 1u);
    EXPECT_EQ(sy, 1u);
}

TEST(RawPrivateComp, EveryLaidOutShapeMovesWholeAndKeepsWhatWorkSubmittedReads)
{
    const std::string source = SourceText("../raw/runtime/raw_private_comp.cpp");
    const std::string move = Body(source, "bool Device::plainFromCompressed(");
    EXPECT_THAT(move, HasSubstr("createLaidOut(ask, plain, nullptr)")) << "a chain, array or multisampled texture";
    EXPECT_THAT(move, HasSubstr("copyImages(*b, t, plain);"));
    EXPECT_THAT(move, HasSubstr("p.staleChain = t.chain;"));
    const std::string copy = Body(source, "void copyImages(");
    EXPECT_THAT(copy, HasSubstr("b.copyTexels(from, 0, 0, 0, to, 0, 0, 0, from.width, from.height);")) << "level 0";
    EXPECT_THAT(copy, HasSubstr("chainLevelImage(from, level, a)")) << "every later level";
    EXPECT_THAT(copy, HasSubstr("layer < std::max(from.layers, 1u)")) << "every layer";
    EXPECT_THAT(copy, HasSubstr("sampleGrid(from.samples, sx, sy)")) << "every sample";
    EXPECT_THAT(Body(source, "void Device::releasePrivateCompressed("), HasSubstr("delete stale;"));
    const std::string laid = Body(SourceText("../raw/runtime/raw_gpu.cpp"), "bool Device::createLaidOut(");
    EXPECT_THAT(laid, HasSubstr("ask.compressKind && !at && !zeta && takeCompressed(bytes, ask.compressKind, comp)"));
    EXPECT_THAT(laid, HasSubstr("noteCompressed(ask, comp, bytes, t);"));
}

TEST(RawPrivateComp, AFetchingStateWithNoBarrierFormDrawsItsCompressedTargetOnTheRawPath)
{
    // gpu.raw.fetch_blend_order strip16 (rgba8, 64x64 private RenderTarget|ShaderRead): boot 11's sto=compressed,notwin,
    // which rule storage.compressed now draws in place
    Target strip{4, 1, true};
    EXPECT_EQ(raw::storageViewMismatch(4, 1, strip), raw::StorageFallback::None);
    EXPECT_EQ(route(0x1, false, strip), "raw");
    EXPECT_TRUE(strip.comptags) << "no move to plain memory";
    // gpu.raw.pass_fusion private_color_adjacent: a clear pass and a load pass on one target, the fetch in both
    Target adjacent{4, 1, true};
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
    EXPECT_EQ(route(0x1, false, adjacent), "raw");
}

TEST(RawPrivateComp, AStateWithABarrierFormKeepsTheCompressedTarget)
{
    Target t{4, 1, true};
    EXPECT_EQ(raw::storageSlotsToPlain(0x3, true, true, false), 0);
    EXPECT_EQ(route(0x1, true, t), "raw") << "rule storage.compressed: its storage form, in place";
    EXPECT_TRUE(t.comptags);
    EXPECT_EQ(raw::storageSlotsToPlain(0x6, false, true, false), 0x6);
    Target off{4, 1, true};
    EXPECT_EQ(route(0x1, true, off, kList, false), "twin") << "compressed storage views off: its barrier form";
    EXPECT_TRUE(off.comptags);
}

// gpu.raw.storage_compressed's storage arm turns storage views of compressed memory on (switchRawProbeStorageCompressed:):
// its no-twin fetching draw must keep the compressed target it judges, not move it to plain memory first
TEST(RawPrivateComp, StorageViewsOfCompressedMemoryMoveNothing)
{
    EXPECT_EQ(raw::storageSlotsToPlain(0x3, false, true, true), 0);
    Target t{4, 1, true};
    EXPECT_EQ(raw::storageViewMismatch(4, 1, t, true), raw::StorageFallback::None);
}

// Boot 21 iconservicesagent (RenderBox filter_color 2218a4f4d2231d46, uber 43017f3f15e41157, both drawn in the pixel
// interlock elsewhere): a quad drawn as an indexed strip of four over a compressed private target took the barrier
// form, and the encode refused it as enc.geometry. The barrier form draws an indexed strip a triangle at a time now,
// unless its faces decide its pixels (raw_fetch_order.h). Such a strip, and every other shape the barrier form cannot
// draw, takes the storage form: over the compressed target under rule storage.compressed, over a plain target with
// storage views of compressed memory off.
TEST(RawPrivateComp, AShapeTheBarrierFormCannotDrawTakesTheStorageForm)
{
    const raw::FetchDraw indexedStrip = {4, 4, 1, false, true};
    EXPECT_TRUE(raw::barrierFormDraws(0x1, 0x1, 0, false, indexedStrip)) << "drawn a triangle at a time";
    const raw::FetchDraw facing = {4, 4, 1, false, true, true};
    EXPECT_FALSE(raw::barrierFormDraws(0x1, 0x1, 0, false, facing));
    Target shipped{4, 1, true};
    EXPECT_EQ(route(0x1, true, shipped, facing), "raw");
    EXPECT_TRUE(shipped.comptags);
    Target t{4, 1, true};
    EXPECT_EQ(route(0x1, true, t, facing, false), "raw");
    EXPECT_FALSE(t.comptags);
    const raw::FetchDraw shapes[] = {
        {3, 6, 2, false, false}, {3, 0, 1, true, false}, {1, 4, 1, false, false}, {3, 3 * 97, 1, false, false}};
    for (const raw::FetchDraw &d : shapes) {
        Target each{4, 1, true};
        EXPECT_EQ(route(0x1, true, each, d, false), "raw") << d.primitive << " " << d.count << " " << d.instances;
    }
    Target strip{4, 1, true};
    EXPECT_EQ(route(0x1, true, strip, {4, 4, 1, false, false}, false), "twin")
        << "a vertex strip is drawn a triangle a time";
    EXPECT_TRUE(strip.comptags);
    EXPECT_TRUE(raw::barrierFormDraws(0x1, 0x2, 0, false, indexedStrip)) << "it fetches nothing it writes: drawn whole";
    EXPECT_FALSE(raw::barrierFormDraws(0x1, 0x1, 0, true, {3, 6, 1, false, false}))
        << "its fragments read the primitive";
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
        if (raw::storageSlotsToPlain(0x1, false, true, false) & 1 && target.comptags) {
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
                                  "_passes.own(_recordedPass).compressed)\n        rmNoteStorageTargets(self, "
                                  "_passes.own(_recordedPass), cmd);"));
    const std::string pass = SourceText("../raw/runtime/RawMetalEncoder.mm");
    EXPECT_THAT(pass, HasSubstr("facts.compressed |= uint8_t(1u << c);"));
    const std::string submit = SourceText("../raw/runtime/RawMetalSubmit.mm");
    EXPECT_THAT(submit, HasSubstr("rmStorageTargetsPlain(cb, range.from, range.to);\n        const bool encoded = "
                                  "encodeRaw(cb, range.from, range.to, &why, &refusedEncoder);"));
    const std::string facade = SourceText("../raw/runtime/RawMetalPrivateComp.mm");
    const std::string note = Body(facade, "static void notePipeline(");
    EXPECT_THAT(note, HasSubstr("raw::storageSlotsToPlain(pl->storageBits, twin != nullptr, twinDraws, "
                                "gRM->raw->storageCompressedOk()) &\n        pass.compressed"));
    EXPECT_THAT(note, HasSubstr("raw::barrierFormDraws(twin->fetchBits, twin->writeBits, twin->storageBits"));
    EXPECT_THAT(note, HasSubstr("free->rung == raw::FetchIteratedBlend && raw::fetchFreeTaken(*free, {})"))
        << "a proven iterated-blend form keeps the compressed target";
    EXPECT_THAT(Body(facade, "static void noteRangeDraws("), HasSubstr("false, &shape);"))
        << "a variant made late is judged by its draws' shapes";
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalRenderEncoder.mm"),
                HasSubstr("if (const RMPass &pass = e->_cb->_passes[e->_passAt]; pass.compressed)\n        "
                          "rmNoteStorageDraw(e->_cb, pass, pl, e->_drawnVariant, shape);"))
        << "each draw recorded in a compressed pass, by its shape";
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalParallel.mm"), HasSubstr("cb->_plainForStorage.insert("))
        << "a parallel encoder's sub-encoders' targets";
    EXPECT_THAT(note, HasSubstr("cb->_scanStorageTargets = true;")) << "a variant still being made is looked at later";
    const std::string move = Body(facade, "void rmStorageTargetsPlain(");
    EXPECT_THAT(move, HasSubstr("if (cb->_scanStorageTargets)"));
    EXPECT_THAT(move, HasSubstr("gPlainShapes.add("));
    EXPECT_THAT(move, HasSubstr("rmTexturePlain(t)"));
    EXPECT_THAT(Body(facade, "uint32_t privateCompressedKind("), HasSubstr("gPlainShapes.holds("));
    EXPECT_THAT(SourceText("../raw/runtime/RawMetalRangeEncode.h"), HasSubstr("tRawEncoding = true;"));
}

TEST(RawPrivateComp, AStoredZetaTakesCompressedMemoryInEveryBuildAndKeepsItsTwinMove)
{
    const std::string zeta = Body(SourceText("../raw/runtime/raw_multisample.cpp"), "bool Device::createOwnedZeta(");
    EXPECT_THAT(zeta, HasSubstr("storedZetaKind("));
    EXPECT_THAT(zeta, HasSubstr("createCompressedZeta(bytes, kind, sampled, t)"));
    EXPECT_THAT(zeta, Not(HasSubstr("#if"))) << "the compressed route is the release one, not a probe's";
    const std::string move = Body(SourceText("../raw/runtime/raw_multisample.cpp"), "bool Device::moveOwnedZeta(");
    EXPECT_THAT(move, HasSubstr("compZeta_.count(t.va)")) << "a compressed zeta still moves into its m2v twin's image";
    const std::string made =
        Body(SourceText("../raw/runtime/raw_private_comp.cpp"), "bool Device::createCompressedZeta(");
    EXPECT_THAT(made, HasSubstr("takeCompressed(bytes, kind, bo)")) << "memory made off the render thread, as colour's";
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_private_comp.cpp"), "bool Device::takeCompressed("),
                HasSubstr("comp_.fit(need, kind)"));
    EXPECT_THAT(made, Not(HasSubstr("allocCompressible")));
    EXPECT_THAT(Body(SourceText("../raw/runtime/raw_gpu.h"), "    void freeOwnedZeta(uint64_t va)"),
                HasSubstr("freeCompressedZeta(va);"));
}

}  // namespace
