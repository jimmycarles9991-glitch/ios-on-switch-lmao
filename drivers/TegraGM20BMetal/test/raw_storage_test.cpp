// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_direct_storage.h"
#include "../raw/runtime/raw_draw_interlock.h"
#include "../raw/runtime/raw_emit.h"
#include "../raw/runtime/raw_fragment_tests.h"
#include "../raw/runtime/raw_direct_push.h"
#include "../raw/runtime/raw_shader_test_push.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

namespace {
struct State {
    NmbDirectState state = {};
    NmbDirectWrite write = {};

    State()
    {
        state.storageForm = true;
        state.storage.mask = state.storage.writes = state.storage.samples = 1;
        state.storage.formats[0] = 98;
        write.type = 3;
        write.stage = 2;
        write.source = NmbDirectStorageAttachment;
        write.viewType = 1;
        for (auto &draw : state.draws) {
            draw.writes = &write;
            draw.writeCount = 1;
        }
    }
};

auto header = [](uint32_t format, uint32_t &word) {
    word = format == 98 ? 0x24912 : 0;
    return word != 0;
};

raw::PackPipeline pipeline()
{
    State s;
    raw::PackPipeline out = {};
    EXPECT_EQ(raw::qualifyStorage(s.state, header, out), nullptr);
    out.entries = 1;
    out.entry[0] = {0, raw::EntryFetch, 2, 0, 0, 0, 0};
    return out;
}

TEST(RawStorage, DepthStencilNeedsDrawQualifiedVariant)
{
    EXPECT_TRUE(raw::storageDepthQualified("samples=1 topo=0 ds=0/0 c0=80:15:0"));
    EXPECT_FALSE(raw::storageDepthQualified("samples=1 topo=0 ds=252/0 c0=80:15:0"));
    EXPECT_FALSE(raw::storageDepthQualified("samples=1 topo=0 ds=0/253 c0=80:15:0"));
    EXPECT_FALSE(raw::storageDepthQualified("samples=1 topo=0 ds=252/253 c0=80:15:0"));
    EXPECT_FALSE(raw::storageDepthQualified("samples=1 topo=0"));
    EXPECT_TRUE(raw::storageDepthQualified("samples=1 topo=0 ds=252/0 c0=80:15:0", true));
    EXPECT_TRUE(raw::storageDepthQualified("samples=4 topo=0 ds=260/260 c0=80:15:0", true));
}

TEST(RawStorage, ShaderTestsQualifyExactDescriptorsAndDynamicWords)
{
    State s;
    s.state.storage.fragmentTests = 4;
    s.state.storage.depthFormat = 100;
    s.state.storage.stencilFormat = 13;
    s.state.storage.testSamples = 1;
    std::array<NmbDirectWrite, 3> writes = {s.write, s.write, s.write};
    writes[1].source = NmbDirectDepthStandIn;
    writes[2].source = NmbDirectStencilStandIn;
    for (auto &draw : s.state.draws) {
        draw.writes = writes.data();
        draw.writeCount = writes.size();
        draw.depthTestWord = 1;
        draw.stencilTestWord = 5;
        draw.pushMask = 0xff;
    }
    raw::PackPipeline p = {};
    const auto formats = [](uint32_t format, uint32_t &word) {
        word = format;
        return true;
    };
    ASSERT_EQ(raw::qualifyStorage(s.state, formats, p), nullptr);
    p.shaderTests.depthWord = 1;
    p.shaderTests.stencilWord = 5;
    p.flags |= raw::PipelinePushMetadata;
    p.pushMask = 0xff;
    p.entries = 3;
    p.entry[0] = {0, raw::EntryFetch, 2, 0, 0, 0, 0};
    p.entry[1] = {4, raw::EntryDepthTest, 2, 0, 0, 0, 0};
    p.entry[2] = {8, raw::EntryStencilTest, 2, 0, 0, 0, 0};
    const uint32_t blob[] = {0x1224, 2, 0x1a00, 0, 0x0210, 0};
    EXPECT_TRUE(raw::usableStorageMetadata(p, blob, 6));
    EXPECT_TRUE(raw::usableDynamicPush(p, "ds=260/260"));
    EXPECT_TRUE(raw::validDynamicPush(s.state));
    EXPECT_EQ(raw::constantPushWords(s.state), 0u);
    p.entry[2].stage = 1;
    EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 6));
    p.entry[2].stage = 2;
    p.shaderTests.stencilWord = 4;
    EXPECT_FALSE(raw::usableDynamicPush(p, "ds=260/260"));
    s.state.draws[3].depthTestWord = 9;
    EXPECT_FALSE(raw::validDynamicPush(s.state));
    s.state.storage.testSamples = 4;
    EXPECT_NE(raw::qualifyStorage(s.state, formats, p), nullptr);
}

TEST(RawStorage, ShaderTestPushTracksDrawStateWithoutTouchingOtherWords)
{
    raw::PackShaderTests tests = {3, 9, 3, 0, 100, 13, 0};
    raw::DepthStencilDesc state;
    state.depthCompare = 1;
    state.depthWrite = true;
    state.front = {2, 1, 7, 2, 0x0f, 0x1f0};
    state.back = {4, 0, 5, 0, ~0u, ~0u};
    const uint32_t references[] = {0x1a5, 0x3c};
    const float range[] = {-0.0f, 0.75f};
    std::array<uint32_t, 32> push;
    push.fill(0xbadcafe);
    raw::patchShaderTestPush(tests, state, references, range, true, push.data());
    EXPECT_EQ(push[2], 25u);
    EXPECT_EQ(push[3], 0x80000000u);
    EXPECT_EQ(push[4], 0x3f400000u);
    EXPECT_EQ(push[5], 0u);
    EXPECT_EQ(push[8], 2u | 1u << 3 | 2u << 6 | 15u << 9 | 240u << 17 | 7u << 25);
    EXPECT_EQ(push[9], 4u | 255u << 9 | 255u << 17 | 5u << 25);
    EXPECT_EQ(push[10], 0x3ca5u);
    EXPECT_EQ(push[11], 0u);
    for (uint32_t i = 0; i < push.size(); ++i)
        if (!(i >= 2 && i < 6) && !(i >= 8 && i < 12))
            EXPECT_EQ(push[i], 0xbadcafeu) << i;
    state.depthCompare = 7;
    state.depthWrite = false;
    raw::patchShaderTestPush(tests, state, references, range, false, push.data());
    EXPECT_EQ(push[2], 7u);
}

TEST(RawStorage, DrawTestClassificationAccountsForApplicableStencilOperations)
{
    using Tests = raw::FragmentTests;
    raw::DepthStencilDesc state;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::Pass);
    state.depthCompare = 1;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::ReadOnly);
    state.depthWrite = true;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::DepthWrite);
    state.front.fail = 2;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::DepthWrite);
    state.front.depthFail = 2;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::StencilWrite);
    state.front.writeMask = 0;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::DepthWrite);
    state.depthWrite = false;
    state.back.compare = 2;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::ReadOnly);
    state.back.pass = 3;
    EXPECT_EQ(raw::fragmentTests(state, 260), Tests::StencilWrite);
    EXPECT_EQ(raw::fragmentTests(state, 252), Tests::ReadOnly);
    EXPECT_EQ(raw::fragmentTests(state, 0), Tests::Pass);
}

TEST(RawStorage, DrawQualifiedRecordCannotCrossDepthStencilStateChanges)
{
    raw::DepthStencilDesc state;
    EXPECT_TRUE(raw::fragmentTestsMatch(1, state, 252));
    state.depthCompare = 1;
    EXPECT_FALSE(raw::fragmentTestsMatch(1, state, 252));
    EXPECT_TRUE(raw::fragmentTestsMatch(2, state, 252));
    state.depthWrite = true;
    EXPECT_FALSE(raw::fragmentTestsMatch(2, state, 252));
    EXPECT_TRUE(raw::fragmentTestsMatch(0, state, 252));
}

TEST(RawStorage, ReadOnlyDrawVariantRequiresExplicitEarlyTests)
{
    auto p = pipeline();
    p.storageTests = 2;
    uint32_t blob[] = {0x1224, 2, 0x1a00, 0, 0x0210, 1};
    EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 4));
    EXPECT_TRUE(raw::usableStorageMetadata(p, blob, 6));
    blob[5] = 0;
    EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 6));
    p.storageTests = 1;
    EXPECT_TRUE(raw::usableStorageMetadata(p, blob, 6));
    for (uint8_t tests : {3, 4}) {
        p.storageTests = tests;
        EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 6));
        blob[5] = 1;
        EXPECT_TRUE(raw::usableStorageMetadata(p, blob, 6));
        blob[5] = 0;
    }
}

TEST(RawStorage, PreservesActualViewAndReadOnlyContract)
{
    State s;
    s.state.storage.writes = 0;
    raw::PackPipeline out = {};
    std::vector<uint32_t> queried;
    EXPECT_EQ(raw::qualifyStorage(
                  s.state,
                  [&](uint32_t f, uint32_t &word) {
                      queried.push_back(f);
                      return header(f, word);
                  },
                  out),
              nullptr);
    EXPECT_EQ(queried, std::vector<uint32_t>{98});
    EXPECT_EQ(out.storageMask, 1);
    EXPECT_EQ(out.storageWriteMask, 0);
    EXPECT_EQ(out.storageSamples, 1);
    EXPECT_EQ(out.storageTic[0], 0x24912u);
    EXPECT_EQ(out.flags, raw::PipelineStorageMetadata);
}

TEST(RawStorage, RejectsIncompatibleContractBeforeHeaderWork)
{
    for (int mutation = 0; mutation < 10; mutation++) {
        State s;
        switch (mutation) {
        case 0:
            s.state.storage.mask = 256;
            break;
        case 1:
            s.state.storage.writes = 2;
            break;
        case 2:
            s.state.storage.samplePlanes = 1;
            break;
        case 3:
            s.state.storage.samples = 3;
            break;
        case 4:
            s.write.viewType = 5;
            break;
        case 5:
            s.write.set = 1;
            break;
        case 6:
            s.write.type = 2;
            break;
        case 7:
            s.state.draws[3].writeCount = 0;
            break;
        case 8:
            s.state.storageForm = false;
            break;
        case 9:
            s.write.stage = 1;
            break;
        }
        raw::PackPipeline out = {};
        bool queried = false;
        EXPECT_NE(raw::qualifyStorage(
                      s.state,
                      [&](uint32_t, uint32_t &) {
                          queried = true;
                          return true;
                      },
                      out),
                  nullptr)
            << mutation;
        EXPECT_FALSE(queried) << mutation;
        EXPECT_EQ(out.flags, 0u) << mutation;
    }
}

TEST(RawStorage, MultisampledViewsRequireExactPerSampleInvocationState)
{
    for (uint32_t samples : {2u, 4u, 8u}) {
        State s;
        s.state.storage.samples = samples;
        raw::PackPipeline out = {};
        ASSERT_EQ(raw::qualifyStorage(s.state, header, out), nullptr);
        out.entries = 1;
        out.entry[0] = {0, raw::EntryFetch, 2, 0, 0, 0, 0};
        uint32_t blob[] = {0x1224, 2, 0x1a00, 0, 0x0754, raw::sampleShadingControl(samples)};
        EXPECT_TRUE(raw::usableStorageMetadata(out, blob, 6));
        EXPECT_FALSE(raw::usableStorageMetadata(out, blob, 4));
        blob[5] = 1;
        EXPECT_FALSE(raw::usableStorageMetadata(out, blob, 6));
        blob[5] = raw::sampleShadingControl(samples == 2 ? 4 : 2);
        EXPECT_FALSE(raw::usableStorageMetadata(out, blob, 6));
    }
}

TEST(RawStorage, SampleShadingMaskWordsMatchNativeMmeLayout)
{
    for (uint32_t samples : {1u, 2u, 4u, 8u}) {
        uint32_t words[4];
        ASSERT_TRUE(raw::sampleShadingMasks(samples, samples, words));
        EXPECT_EQ(words[0], 0x00020001u);
        EXPECT_EQ(words[1], 0x00080004u);
        EXPECT_EQ(words[2], 0x00200010u);
        EXPECT_EQ(words[3], 0x00800040u);
        EXPECT_EQ(raw::sampleShadingControl(samples), samples == 1 ? 1u : samples | 0x10u);
        ASSERT_TRUE(raw::sampleShadingMasks(samples, 1, words));
        if (samples > 1)
            for (auto word : words)
                EXPECT_EQ(word, ~0u);
    }
    uint32_t words[4];
    EXPECT_FALSE(raw::sampleShadingMasks(8, 3, words));
    EXPECT_FALSE(raw::sampleShadingMasks(4, 8, words));
    EXPECT_FALSE(raw::sampleShadingMasks(3, 1, words));
}

TEST(RawStorage, EmitsFullSampleRateFromCompiledShaderRequirement)
{
    raw::EmitProgram vertex, fragment;
    fragment.usesSampleShading = true;
    for (uint32_t samples : {1u, 2u, 4u, 8u}) {
        std::vector<raw::EmitRegister> regs;
        std::string why;
        ASSERT_TRUE(raw::emitState("samples=" + std::to_string(samples) + " ds=0/0", vertex, fragment, regs, why));
        bool found = false;
        for (const auto &reg : regs)
            if (reg.method == 0x0754) {
                EXPECT_EQ(reg.value, raw::sampleShadingControl(samples));
                found = true;
            }
        EXPECT_TRUE(found);
    }
}

TEST(RawStorage, UnsupportedFormatProducesNoArtifact)
{
    State s;
    s.state.storage.formats[0] = 43;
    raw::PackPipeline out = {};
    EXPECT_NE(raw::qualifyStorage(s.state, header, out), nullptr);
    EXPECT_EQ(out.storageMask, 0);
    EXPECT_EQ(out.flags, 0u);
}

TEST(RawStorage, RequiresOrderedControlAndExplicitRopExclusion)
{
    auto p = pipeline();
    const uint32_t pixel[] = {0x1224, 2, 0x1a00, 0};
    const uint32_t sample[] = {0x1224, 1, 0x1a00, 0};
    EXPECT_TRUE(raw::usableStorageMetadata(p, pixel, 4));
    EXPECT_TRUE(raw::usableStorageMetadata(p, sample, 4));
    for (uint32_t control : {0u, 3u, 6u, 9u, 10u, 0xffffffffu}) {
        uint32_t blob[] = {0x1224, control, 0x1a00, 0};
        EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 4)) << control;
    }
    EXPECT_FALSE(raw::usableStorageMetadata(p, pixel, 2));
    EXPECT_FALSE(raw::usableStorageMetadata(p, pixel + 2, 2));
    EXPECT_FALSE(raw::usableStorageMetadata(p, pixel, 3));
    for (uint32_t component : {1u, 16u, 256u, 4096u}) {
        uint32_t blob[] = {0x1224, 2, 0x1a00, component};
        EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 4));
    }
}

TEST(RawStorage, RejectsMalformedArtifact)
{
    const uint32_t blob[] = {0x1224, 2, 0x1a00, 0};
    for (int mutation = 0; mutation < 13; mutation++) {
        auto p = pipeline();
        switch (mutation) {
        case 0:
            p.flags = 0;
            break;
        case 1:
            p.storageMask = 0;
            break;
        case 2:
            p.storageWriteMask = 2;
            break;
        case 3:
            p.storageSamples = 4;
            break;
        case 4:
            p.storageTic[0] = 0;
            break;
        case 5:
            p.storageTic[1] = 42;
            break;
        case 6:
            p.entries = 0;
            break;
        case 7:
            p.entries = 255;
            break;
        case 8:
            p.flags |= raw::PipelineBarrierForm;
            break;
        case 9:
            p.storageTests = 255;
            break;
        case 10:
            p.entry[0].stage = 1;
            break;
        case 11:
            p.entries = 2;
            p.entry[1] = {4, raw::EntryFetch, 1, 0, 0, 0, 0};
            break;
        case 12:
            p.entries = 2;
            p.entry[1] = {4, raw::EntryFetch, 2, 8, 0, 0, 0};
            break;
        }
        EXPECT_FALSE(raw::usableStorageMetadata(p, blob, 4)) << mutation;
    }
}

TEST(RawStorage, OrdinaryArtifactCannotCarryStorageMetadata)
{
    raw::PackPipeline p = {};
    EXPECT_TRUE(raw::usableStorageMetadata(p, nullptr, 0));
    p.storageTests = 1;
    EXPECT_FALSE(raw::usableStorageMetadata(p, nullptr, 0));
    p.storageTests = 0;
    for (uint32_t i = 0; i < 8; i++) {
        p.storageTic[i] = 42;
        EXPECT_FALSE(raw::usableStorageMetadata(p, nullptr, 0)) << i;
        p.storageTic[i] = 0;
    }
}

TEST(RawStorage, EmissionResetsControlAndDisablesStorageRopWrites)
{
    const std::string key = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0";
    std::array<uint8_t, 80> header = {};
    for (uint32_t mode : {2u, 1u, 0u}) {
        raw::EmitProgram fs = {header.data(), 16, false, mode};
        std::vector<raw::EmitRegister> words;
        std::string why;
        ASSERT_TRUE(raw::emitState(key, {}, fs, words, why)) << why;
        bool control = false, rop = false;
        for (const auto &r : words) {
            if (r.method == 0x1224) {
                EXPECT_EQ(r.value, mode);
                control = r.read;
            }
            if (r.method == 0x1a00) {
                EXPECT_EQ(r.value, 0u);
                rop = r.read;
            }
        }
        EXPECT_TRUE(control);
        EXPECT_TRUE(rop);
    }
    EXPECT_NE(raw::emittableMethods().count(0x1224), 0u);
}
}  // namespace

// A multisampled storage form runs a pass a sample; its record's pixel conflict detection is drawn as sample detection
// (DATA_ERROR 0x134 otherwise: gpu.raw.framebuffer_fetch_depth's 2x case), every other pair as recorded
TEST(RawStorage, PerSamplePassesDrawSampleConflictDetection)
{
    constexpr uint32_t none = 0, sample = 1, pixel = 2, tile8 = 1u << 2;
    for (uint32_t samples : {2u, 4u, 8u}) {
        const uint32_t hybrid = raw::sampleShadingControl(samples);
        EXPECT_EQ(raw::legalInterlockControl(pixel | tile8, hybrid), sample | tile8) << samples;
        EXPECT_EQ(raw::legalInterlockControl(sample | tile8, hybrid), sample | tile8) << samples;
        EXPECT_EQ(raw::legalInterlockControl(none, hybrid), none) << samples;
    }
    EXPECT_EQ(raw::legalInterlockControl(pixel | tile8, raw::sampleShadingControl(1)), pixel | tile8);
    EXPECT_EQ(raw::legalInterlockControl(pixel, 0), pixel);
}
