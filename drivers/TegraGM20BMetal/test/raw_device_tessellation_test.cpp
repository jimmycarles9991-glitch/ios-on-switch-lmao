// SPDX-License-Identifier: MIT
// raw_tessellation.cpp and a tessellation state's record on raw::Device: its control and evaluation programs bound at their
// stages beside its tessellator registers, the stages switched off again after it, patches drawn only with it and
// primitives only without it, a draw's indexing and control points as Metal draws them for the state, and Metal's factor
// scale as m2v's control stage takes it.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_icb.h"
#include "../raw/runtime/raw_record.h"
#include <cmath>
#include <map>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kControlWord = 5;
constexpr uint32_t kTessParameters = 0x0320, kShaderControl = 0x2080, kShaderEvaluation = 0x20c0;
constexpr uint32_t kProgramControl = 0x2084, kProgramEvaluation = 0x20c4, kRegistersControl = 0x208c;

// (method, value) of every method a pushed word stream writes (raw_push.h's encodings)
std::multimap<uint32_t, uint32_t> methods(const std::vector<uint32_t> &words)
{
    std::multimap<uint32_t, uint32_t> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, method = (h & 0x1fff) << 2;
        if (op == 4) {
            out.emplace(method, count);
            continue;
        }
        for (uint32_t k = 0; k < count && i < words.size(); k++)
            out.emplace(op == 1 ? method + 4 * k : method, words[i++]);
    }
    return out;
}

raw::RuntimeRecord record(bool tessellates, uint64_t key, uint8_t indexBytes = 0)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    if (!tessellates)
        return r;
    for (raw::CapturedProgram &p : r.tessPrograms) {
        p.image.assign(80 + 64, 0);
        p.image[80] = uint8_t(1 + (&p - r.tessPrograms));
        p.id = "tess" + std::to_string(&p - r.tessPrograms);
        p.gprs = 12;
    }
    r.pipeline.controlWord = kControlWord;
    r.pipeline.controlPoints = 4;
    r.pipeline.controlPointIndexBytes = indexBytes;
    r.pipeline.entries = raw::TessBufferCount;
    for (uint8_t k = 0; k < raw::TessBufferCount; k++)
        r.pipeline.entry[k] = {uint16_t(16 * k),
                               raw::EntryTessBuffer,
                               1,
                               k,
                               uint8_t(1 | (kControlWord + raw::kTessResidualWord[k]) << 2),
                               0,
                               0};
    r.pipeline.pushMask = 0x3fu << (kControlWord - 1);
    r.extras = {{kTessParameters, 0x202},  {0x0dcc, 1}, {kShaderControl, 0x21}, {0x2090, 1},
                {kShaderEvaluation, 0x31}, {0x20d0, 2}};
    r.restore = {{kShaderControl, 0x20}, {kShaderEvaluation, 0x30}};
    return r;
}

class DeviceTessellation : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer factors, indices;
    raw::Pipeline *patches = nullptr, *plain = nullptr, *indexed = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, factors));
        ASSERT_TRUE(dev.createBuffer(4096, indices));
        std::string why;
        patches = dev.addRecord(record(true, 0x7e55), false, why);
        ASSERT_NE(patches, nullptr) << why;
        plain = dev.addRecord(record(false, 0x7e56), false, why);
        ASSERT_NE(plain, nullptr) << why;
        indexed = dev.addRecord(record(true, 0x7e57, 2), false, why);
        ASSERT_NE(indexed, nullptr) << why;
    }

    template <class Draw> std::string drawWith(raw::Pipeline *pl, Draw draw)
    {
        raw::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        raw::RenderEncoder *e = cb->render(pass);
        if (!e) {
            cb->abandon();
            return "no pass";
        }
        e->setPipeline(pl);
        e->setTessellationFactors(&factors, 6, 13);
        const char *why = nullptr;
        const bool ok = draw(*e, &why);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            return why ? why : "not committed";
        }
        return "";
    }
};

TEST_F(DeviceTessellation, AStateBindsItsControlAndEvaluationProgramsAndSwitchesThemOffAfter)
{
    const auto bound = methods(patches->methods);
    ASSERT_EQ(bound.count(kProgramControl), 1u);
    ASSERT_EQ(bound.count(kProgramEvaluation), 1u);
    EXPECT_EQ(bound.find(kRegistersControl)->second, 12u);
    EXPECT_NE(bound.find(kProgramControl)->second, bound.find(kProgramEvaluation)->second);
    EXPECT_EQ(bound.find(kShaderControl)->second, 0x21u);
    EXPECT_EQ(bound.find(kShaderEvaluation)->second, 0x31u);
    EXPECT_EQ(bound.find(kTessParameters)->second, 0x202u);
    const auto after = methods(patches->restore);
    EXPECT_EQ(after.find(kShaderControl)->second, 0x20u);
    EXPECT_EQ(after.find(kShaderEvaluation)->second, 0x30u);
    EXPECT_EQ(methods(plain->methods).count(kProgramControl), 0u);
}

TEST_F(DeviceTessellation, PatchesDrawOnlyWithATessellationState)
{
    EXPECT_EQ(drawWith(patches,
                       [](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 2, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_NE(drawWith(patches,
                       [](raw::RenderEncoder &e, const char **why) {
                           return e.draw(kTriangle, 0, 3, 1, 0, why);
                       }),
              "");
    EXPECT_NE(drawWith(plain,
                       [](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 2, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_EQ(drawWith(plain,
                       [](raw::RenderEncoder &e, const char **why) {
                           return e.draw(kTriangle, 0, 3, 1, 0, why);
                       }),
              "");
}

// As Metal draws them (m2v render.rs draw_patches): the state's control points whatever the draw names, a control-point
// index buffer the state has no index type for unread, none where it has one read as index 0, patch indices from their
// offset rounded down to 4; control-point indices off their size are refused
TEST_F(DeviceTessellation, ADrawsIndexingAndControlPointsAreTheStates)
{
    const raw::Buffer *points = &indices;
    EXPECT_EQ(drawWith(patches,
                       [](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(3, 0, 1, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_EQ(drawWith(patches,
                       [points](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, points, 0, why);
                       }),
              "");
    EXPECT_EQ(drawWith(indexed,
                       [](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_EQ(drawWith(indexed,
                       [points](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, points, 4, points, 2, why);
                       }),
              "");
    EXPECT_EQ(drawWith(indexed,
                       [points](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, points, 2, points, 2, why);
                       }),
              "");
    EXPECT_NE(drawWith(indexed,
                       [points](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, points, 4, points, 1, why);
                       }),
              "");
}

TEST_F(DeviceTessellation, AnIndirectPatchDrawReadsItsArgumentsOnTheGpu)
{
    raw::Buffer arguments;
    ASSERT_TRUE(dev.createBuffer(256, arguments));
    const uint32_t draw[4] = {2, 1, 0, 0};
    memcpy(arguments.cpu, draw, sizeof draw);
    const raw::Buffer *args = &arguments;
    EXPECT_EQ(drawWith(patches,
                       [args](raw::RenderEncoder &e, const char **why) {
                           return e.drawPatchesIndirect(4, nullptr, 0, nullptr, 0, args, 0, why);
                       }),
              "");
}

// Set 1 is bound at the vertex and pixel programs' slots alone (Pipeline::heapSlot)
TEST_F(DeviceTessellation, AStateWhoseTessellationProgramReadsSetOneIsRefused)
{
    raw::RuntimeRecord r = record(true, 0x7e58);
    r.tessPrograms[1].id = "tessheap";
    r.tessPrograms[1].cbufTypes = {raw::CbufRoot, raw::kCbufHeapSet};
    std::string why;
    raw::Pipeline *heap = dev.addRecord(r, false, why);
    ASSERT_NE(heap, nullptr) << why;
    EXPECT_EQ(drawWith(heap,
                       [](raw::RenderEncoder &e, const char **refused) {
                           return e.drawPatches(4, 0, 2, 1, 0, nullptr, 0, nullptr, 0, refused);
                       }),
              "a tessellation program that reads argument buffers through set 1");
}

TEST(TessellationScale, MetalsScaleAsAHalfAsM2vPushesIt)
{
    uint32_t bits = 0;
    ASSERT_TRUE(raw::tessellationScaleBits(1.0f, bits));
    EXPECT_EQ(bits, 0x3f800000u);
    ASSERT_TRUE(raw::tessellationScaleBits(1.0001f, bits));
    EXPECT_EQ(bits, 0x3f800000u);
    ASSERT_TRUE(raw::tessellationScaleBits(70000.0f, bits));
    EXPECT_EQ(bits, 0x4f800000u);
    ASSERT_TRUE(raw::tessellationScaleBits(std::ldexp(1.0f, -24), bits));
    float subnormal;
    memcpy(&subnormal, &bits, 4);
    EXPECT_EQ(subnormal, std::ldexp(1.0f, -24));
    for (float refused : {0.0f, -1.0f, std::nanf("")})
        EXPECT_FALSE(raw::tessellationScaleBits(refused, bits)) << refused;
}

TEST(PatchPrimitive, IsAPatchListOfTrianglesOut)
{
    EXPECT_EQ(raw::icb::topology(raw::icb::kPatchPrimitive), 0xeu);
    EXPECT_TRUE(raw::icb::triangles(raw::icb::kPatchPrimitive));
    EXPECT_FALSE(raw::icb::restarts(raw::icb::kPatchPrimitive));
}

}  // namespace
