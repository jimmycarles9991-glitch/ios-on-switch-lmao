// SPDX-License-Identifier: MIT
// tessellation.cpp and a tessellation state's record on gm20b::Device: its control and evaluation programs bound at their
// stages beside its tessellator registers, the stages switched off again after it, patches drawn only with it and
// primitives only without it, a draw's indexing and control points as Metal draws them for the state, and Metal's factor
// scale as m2v's control stage takes it. Each holds of the state's runtime record and of its pack record alike.
#include "device_harness.h"
#include "record_pack.h"
#include "../depth_bias.h"
#include "../icb.h"
#include "../record.h"
#include <cmath>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kControlWord = 5;
constexpr uint32_t kTessParameters = 0x0320, kPatch = 0x0dcc, kShaderControl = 0x2080, kShaderEvaluation = 0x20c0;
constexpr uint32_t kProgramControl = 0x2084, kProgramEvaluation = 0x20c4, kRegistersControl = 0x208c;
constexpr uint32_t kRegistersEvaluation = 0x20cc, kBindingControl = 0x2090, kBindingEvaluation = 0x20d0;

// (method, value) of every method a pushed word stream writes (push.h's encodings), in order
std::vector<std::pair<uint32_t, uint32_t>> written(const std::vector<uint32_t> &words)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, method = (h & 0x1fff) << 2;
        if (op == 4) {
            out.emplace_back(method, count);
            continue;
        }
        for (uint32_t k = 0; k < count && i < words.size(); k++)
            out.emplace_back(op == 1 ? method + 4 * k : op == 5 && k ? method + 4 : method, words[i++]);
    }
    return out;
}

std::multimap<uint32_t, uint32_t> methods(const std::vector<uint32_t> &words)
{
    const auto in = written(words);
    return {in.begin(), in.end()};
}

gm20b::RuntimeRecord record(bool tessellates, uint64_t key, uint8_t indexBytes = 0, bool evaluationWrites = false)
{
    gm20b::RuntimeRecord r;
    r.keyString = "samples=1 c0=70:15:0 ds=0/0 ";
    for (gm20b::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
    }
    r.pipeline.keyHash = key;
    r.blob = {0x1a00, 0x1111};
    if (!tessellates)
        return r;
    for (gm20b::CapturedProgram &p : r.tessPrograms) {
        p.image.assign(80 + 64, 0);
        p.image[80] = uint8_t(1 + (&p - r.tessPrograms));
        p.id = std::string(evaluationWrites ? "tess-write-" : "tess") + std::to_string(&p - r.tessPrograms);
        p.gprs = 12;
    }
    if (evaluationWrites) {
        gm20b::CapturedProgram &eval = r.tessPrograms[1];
        eval.image[2] |= 1;   // SPHV3T1_DOES_GLOBAL_STORE
        eval.image[53] |= 4;  // OMAP_SYSTEM_VALUES_A ViewportIndex (0x068)
        const uint64_t stg = uint64_t(0xeed8) << 48;
        memcpy(eval.image.data() + 80 + 8, &stg, sizeof stg);
    }
    r.pipeline.controlWord = kControlWord;
    r.pipeline.controlPoints = 4;
    r.pipeline.controlPointIndexBytes = indexBytes;
    gm20b::setEntryCount(r.pipeline, gm20b::TessBufferCount);
    for (uint8_t k = 0; k < gm20b::TessBufferCount; k++)
        r.pipeline.entry[k] = {uint16_t(16 * k),
                               gm20b::EntryTessBuffer,
                               1,
                               k,
                               uint8_t(1 | (kControlWord + gm20b::kTessResidualWord[k]) << 2),
                               0,
                               0};
    r.pipeline.pushMask = 0x3fu << (kControlWord - 1);
    r.extras = {{kTessParameters, 0x202},  {0x0dcc, 1}, {kShaderControl, 0x21}, {0x2090, 1},
                {kShaderEvaluation, 0x31}, {0x20d0, 2}};
    r.restore = {{kShaderControl, 0x20}, {kShaderEvaluation, 0x30}};
    return r;
}

// The parameter: the states' records are the pack's (record_pack.h), else the runtime's (Device::addRecord)
class DeviceTessellation : public harness::DeviceTest, public testing::WithParamInterface<bool> {
protected:
    gm20b::Texture target;
    gm20b::Buffer factors, indices;
    gm20b::Pipeline *patches = nullptr, *plain = nullptr, *indexed = nullptr, *storing = nullptr;
    gm20b::Refusal refused;  // the last refused draw's

    void SetUp() override
    {
        const std::vector<gm20b::RuntimeRecord> records = {record(true, 0x7e55), record(false, 0x7e56),
                                                           record(true, 0x7e57, 2), record(true, 0x7e5a, 0, true)};
        if (GetParam()) {
            kernel_model::model = {};
            std::string err;
            ASSERT_TRUE(dev.open(harness::recordPack(records, "tessellation-pack.bin").c_str(), 0, err,
                                 gm20b::Device::Role::Client))
                << err;
        } else {
            DeviceTest::SetUp();
        }
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, factors));
        ASSERT_TRUE(dev.createBuffer(4096, indices));
        gm20b::Pipeline **made[] = {&patches, &plain, &indexed, &storing};
        for (size_t k = 0; k < records.size(); k++) {
            std::string why;
            *made[k] = GetParam() ? dev.pipeline(records[k].pipeline.keyHash) : dev.addRecord(records[k], false, why);
            ASSERT_NE(*made[k], nullptr) << why;
        }
    }

    template <class Draw> std::string drawWith(gm20b::Pipeline *pl, Draw draw)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return "no command buffer";
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb->render(pass);
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
            refused = cb->refusal;
            cb->abandon();
            return why ? why : "not committed";
        }
        return "";
    }
};

TEST_P(DeviceTessellation, AStateBindsItsControlAndEvaluationProgramsAndSwitchesThemOffAfter)
{
    const auto bound = methods(patches->methods);
    ASSERT_EQ(bound.count(kProgramControl), 1u);
    ASSERT_EQ(bound.count(kProgramEvaluation), 1u);
    ASSERT_EQ(bound.count(kPatch), 1u);
    EXPECT_EQ(bound.find(kPatch)->second, 1u);
    EXPECT_EQ(bound.find(kRegistersControl)->second, 12u);
    EXPECT_EQ(bound.find(kRegistersEvaluation)->second, 12u);
    EXPECT_NE(bound.find(kProgramControl)->second, bound.find(kProgramEvaluation)->second);
    EXPECT_EQ(bound.find(kShaderControl)->second, 0x21u);
    EXPECT_EQ(bound.find(kShaderEvaluation)->second, 0x31u);
    EXPECT_EQ(bound.find(kBindingControl)->second, 1u);
    EXPECT_EQ(bound.find(kBindingEvaluation)->second, 2u);
    EXPECT_EQ(bound.find(kTessParameters)->second, 0x202u);
    const auto after = methods(patches->restore);
    EXPECT_EQ(after.find(kShaderControl)->second, 0x20u);
    EXPECT_EQ(after.find(kShaderEvaluation)->second, 0x30u);
    const auto plainBound = methods(plain->methods);
    EXPECT_EQ(plainBound.count(kProgramControl), 0u);
    EXPECT_EQ(plainBound.count(kPatch), 0u);
}

TEST_P(DeviceTessellation, EvaluationProgramStoresAndViewportReachThePipeline)
{
    EXPECT_FALSE(patches->vertexStores);
    EXPECT_FALSE(patches->writesMemory[0]);
    EXPECT_FALSE(patches->viewportIndex);
    EXPECT_TRUE(storing->vertexStores);
    EXPECT_TRUE(storing->writesMemory[0]);
    EXPECT_TRUE(storing->viewportIndex);
    EXPECT_GT(storing->vertexInstructions, plain->vertexInstructions);
}

// Either way round is Metal's validation error, which fails the command buffer (TegraGM20BMetalDrawErrors.mm)
TEST_P(DeviceTessellation, PatchesDrawOnlyWithATessellationState)
{
    EXPECT_EQ(drawWith(patches,
                       [](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 2, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_NE(drawWith(patches,
                       [](gm20b::RenderEncoder &e, const char **why) {
                           return e.draw(kTriangle, 0, 3, 1, 0, why);
                       }),
              "");
    EXPECT_EQ(refused.id, gm20b::RefTessellationDraw);
    EXPECT_EQ(refused.a, kTriangle);
    EXPECT_EQ(refused.b, 1u);
    EXPECT_NE(drawWith(plain,
                       [](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 2, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_EQ(refused.id, gm20b::RefTessellationDraw);
    EXPECT_EQ(refused.a, gm20b::icb::kPatchPrimitive);
    EXPECT_EQ(refused.b, 0u);
    EXPECT_EQ(drawWith(plain,
                       [](gm20b::RenderEncoder &e, const char **why) {
                           return e.draw(kTriangle, 0, 3, 1, 0, why);
                       }),
              "");
}

// As Metal draws them (m2v render.rs draw_patches): the state's control points whatever the draw names, a control-point
// index buffer the state has no index type for unread, none where it has one read as index 0, patch indices from their
// offset rounded down to 4; control-point indices off their size are refused
TEST_P(DeviceTessellation, ADrawsIndexingAndControlPointsAreTheStates)
{
    const gm20b::Buffer *points = &indices;
    EXPECT_EQ(drawWith(patches,
                       [](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(3, 0, 1, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_EQ(drawWith(patches,
                       [points](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, points, 0, why);
                       }),
              "");
    EXPECT_EQ(drawWith(indexed,
                       [](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "");
    EXPECT_EQ(drawWith(indexed,
                       [points](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, points, 4, points, 2, why);
                       }),
              "");
    EXPECT_EQ(drawWith(indexed,
                       [points](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, points, 2, points, 2, why);
                       }),
              "");
    EXPECT_NE(drawWith(indexed,
                       [points](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatches(4, 0, 1, 1, 0, points, 4, points, 1, why);
                       }),
              "");
    EXPECT_EQ(refused.id, gm20b::RefControlPointOffset);
    EXPECT_EQ(refused.a, 1u);
}

// A factor scale that is not a normal float above 0 is Metal's validation error (setTessellationFactorScale:)
TEST_P(DeviceTessellation, AScaleMetalsValidationStopsFailsTheCommandBuffer)
{
    EXPECT_EQ(drawWith(patches,
                       [](gm20b::RenderEncoder &e, const char **why) {
                           e.setTessellationFactorScale(0);
                           return e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, nullptr, 0, why);
                       }),
              "a tessellation factor scale that is not a positive half");
    EXPECT_EQ(refused.id, gm20b::RefTessellationScale);
}

TEST_P(DeviceTessellation, AnIndirectPatchDrawReadsItsArgumentsOnTheGpu)
{
    gm20b::Buffer arguments;
    ASSERT_TRUE(dev.createBuffer(256, arguments));
    const uint32_t draw[4] = {2, 1, 0, 0};
    memcpy(arguments.cpu, draw, sizeof draw);
    const gm20b::Buffer *args = &arguments;
    EXPECT_EQ(drawWith(patches,
                       [args](gm20b::RenderEncoder &e, const char **why) {
                           return e.drawPatchesIndirect(4, nullptr, 0, nullptr, 0, args, 0, why);
                       }),
              "");
}

// A post-tessellation vertex function reading textures or samplers of argument buffers reads set 1 from its
// tessellation program's constant buffer: the draw binds the state's set 1 there (group 2, Pipeline::heapSlot), as it
// does at the vertex and pixel programs' slots
TEST_P(DeviceTessellation, AStateWhoseTessellationProgramReadsSetOneBindsItAtItsGroup)
{
    constexpr uint32_t kBindGroupCb = 0x2410, kCbSelectorB = 0x2384, kCbSelectorC = 0x2388, kSampled2d = 0x5;
    gm20b::RuntimeRecord r = record(true, 0x7e58);
    r.tessPrograms[1].id = "tessheap";
    r.tessPrograms[1].cbufTypes = {gm20b::CbufRoot, gm20b::kCbufHeapSet};
    r.pipeline.heapArrays = 1;
    r.pipeline.heap[0] = {kSampled2d, 0};
    r.pipeline.heapBytes = 0x1000;
    std::string why;
    gm20b::Pipeline *heap = dev.addRecord(r, false, why);
    ASSERT_NE(heap, nullptr) << why;
    ASSERT_NE(heap->heapSet, nullptr);
    EXPECT_EQ(heap->heapSlot[3], 1);
    const size_t first = kernel_model::model.jobs.size();
    EXPECT_EQ(drawWith(heap,
                       [](gm20b::RenderEncoder &e, const char **refused) {
                           return e.drawPatches(4, 0, 2, 1, 0, nullptr, 0, nullptr, 0, refused);
                       }),
              "");
    std::vector<uint32_t> words;
    for (size_t j = first; j < kernel_model::model.jobs.size(); j++)
        words.insert(words.end(), kernel_model::model.jobs[j].words.begin(), kernel_model::model.jobs[j].words.end());
    uint64_t selected = 0, boundAtTessellation = 0;
    for (const auto &[m, v] : written(words)) {
        if (m == kCbSelectorB)
            selected = uint64_t(v) << 32;
        else if (m == kCbSelectorC)
            selected |= v;
        else if (m == kBindGroupCb + 0x20 * 2 && v == (1u | 1u << 4))
            boundAtTessellation = selected;
    }
    EXPECT_EQ(boundAtTessellation, heap->heapSet->memory.va);
}

// A pack record draws with the methods and restore its runtime record has: only where each program sits differs
TEST_P(DeviceTessellation, ItsMethodsAndRestoreAreTheRuntimeRecordsOfTheState)
{
    std::string why;
    const gm20b::Pipeline *made = dev.addRecord(record(true, 0x7e59), false, why);
    ASSERT_NE(made, nullptr) << why;
    EXPECT_EQ(patches->restore, made->restore);
    auto bound = methods(patches->methods), runtime = methods(made->methods);
    for (const uint32_t placed : {0x2044u, 0x2144u, kProgramControl, kProgramEvaluation}) {
        bound.erase(placed);
        runtime.erase(placed);
    }
    EXPECT_EQ(bound, runtime);
}

INSTANTIATE_TEST_SUITE_P(RuntimeAndPackRecords, DeviceTessellation, testing::Bool());

TEST(TessellationScale, MetalsScaleAsAHalfAsM2vPushesIt)
{
    uint32_t bits = 0;
    ASSERT_TRUE(gm20b::tessellationScaleBits(1.0f, bits));
    EXPECT_EQ(bits, 0x3f800000u);
    ASSERT_TRUE(gm20b::tessellationScaleBits(1.0001f, bits));
    EXPECT_EQ(bits, 0x3f800000u);
    ASSERT_TRUE(gm20b::tessellationScaleBits(70000.0f, bits));
    EXPECT_EQ(bits, 0x4f800000u);
    ASSERT_TRUE(gm20b::tessellationScaleBits(std::ldexp(1.0f, -24), bits));
    float subnormal;
    memcpy(&subnormal, &bits, 4);
    EXPECT_EQ(subnormal, std::ldexp(1.0f, -24));
    for (float refused : {0.0f, -1.0f, std::nanf("")})
        EXPECT_FALSE(gm20b::tessellationScaleBits(refused, bits)) << refused;
}

// An M4 Max biases a tessellated quad patch's depth as a triangle's: the draw's polygon offset is on with the encoder's
// factors, the infinite ones included (depth_bias.h)
TEST_P(DeviceTessellation, PatchesTakeTheEncodersDepthBias)
{
    constexpr uint32_t kPolyOffsetFill = 0x0dc8, kDepthBias = 0x15bc, kMacroCalls = 0x3800;
    for (const float constant : {100.0f, INFINITY}) {
        const size_t first = kernel_model::model.jobs.size();
        EXPECT_EQ(drawWith(patches,
                           [constant](gm20b::RenderEncoder &e, const char **why) {
                               e.setDepthBias(constant, 0, 0);
                               return e.drawPatches(4, 0, 1, 1, 0, nullptr, 0, nullptr, 0, why);
                           }),
                  "");
        const float written = gm20b::depthBiasFactors(constant, 0, 0).constant;
        uint32_t want;
        memcpy(&want, &written, 4);
        uint32_t fill = 1, bias = 0;  // the channel holds polygon offset on until a write turns it off
        bool drawn = false;
        for (size_t j = first; j < kernel_model::model.jobs.size() && !drawn; j++) {
            uint32_t op = 0, subc = 0, m = 0, left = 0;
            for (const uint32_t w : kernel_model::model.jobs[j].words) {
                if (left) {
                    if (subc == gm20b::SUBC_3D && m >= kMacroCalls) {
                        drawn = true;
                        break;
                    }
                    if (subc == gm20b::SUBC_3D && m == kPolyOffsetFill)
                        fill = w;
                    if (subc == gm20b::SUBC_3D && m == kDepthBias)
                        bias = w;
                    left--;
                    m += op == 1 || op == 5 ? 4 : 0;
                    op = op == 5 ? 3 : op;
                    continue;
                }
                if (!w)
                    continue;
                op = w >> 29;
                subc = w >> 13 & 7;
                m = (w & 0xfff) << 2;
                left = op == 4 ? 0 : w >> 16 & 0x1fff;
                if (op == 4 && subc == gm20b::SUBC_3D && m == kPolyOffsetFill)
                    fill = w >> 16 & 0x1fff;
            }
        }
        EXPECT_TRUE(drawn) << constant;
        EXPECT_EQ(fill, 1u) << constant;
        EXPECT_EQ(bias, want) << constant;
    }
}

TEST(PatchPrimitive, IsAPatchListOfTrianglesOut)
{
    EXPECT_EQ(gm20b::icb::topology(gm20b::icb::kPatchPrimitive), 0xeu);
    EXPECT_TRUE(gm20b::icb::triangles(gm20b::icb::kPatchPrimitive));
    EXPECT_FALSE(gm20b::icb::restarts(gm20b::icb::kPatchPrimitive));
}

}  // namespace
