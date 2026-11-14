// SPDX-License-Identifier: MIT
// Each form of state boot 21's shader compiler refused as "a request whose fields give another key than its own" (Core
// Animation's blur chain and the solid fill into b3a8, a state's written formats, vertex amplification, a compute stage
// input), keyed the way the client keys it (RawMetalStates.mm's pipeline key and RawMetalCubic.mm's variant name, the
// compute state's constants in RawMetalComputeState.mm) and asked with the request the client makes of it
// (RawCompleteRemote.mm): the compiler makes the client's key of every one, and a request that leaves the form out is
// refused with the first field the two keys part at and both keys. Every field of a request goes over the wire, and the
// descriptor the compiler compiles from carries what the client's did.
#include "../raw/runtime/RawCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../raw/runtime/raw_compute_key.h"
#include "made_metallib.h"
#include "bazel/testing/source_text.h"

#include <gtest/gtest.h>
#include <functional>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

raw::RequestStage stage(const char *tag, const char *function)
{
    raw::RequestStage s;
    s.tag = tag;
    s.function = s.name = function;
    return s;
}

raw::RequestLibrary data(const std::string &bytes)
{
    raw::RequestLibrary l;
    l.bytes = bytes;
    return l;
}

// The blur chain's downsample into b3a8 beside a memoryless b3a8 colour 1 (precompile/corpus.log Pb3a8Xm_Tds4A2Xhf_Isrc)
MTLRenderPipelineDescriptor *blurDownsample()
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.colorAttachments[0].pixelFormat = MTLPixelFormat(550);
    d.colorAttachments[1].pixelFormat = MTLPixelFormat(550);
    d.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    const MTLVertexFormat formats[4] = {MTLVertexFormatFloat4, MTLVertexFormatFloat2, MTLVertexFormatFloat2,
                                        MTLVertexFormatHalf4};
    const NSUInteger offsets[4] = {0, 16, 24, 32};
    for (unsigned i = 0; i < 4; i++) {
        v.attributes[i].format = formats[i];
        v.attributes[i].offset = offsets[i];
        v.attributes[i].bufferIndex = 1;
    }
    v.layouts[1].stride = 48;
    d.vertexDescriptor = v;
    return d;
}

MTLRenderPipelineDescriptor *quad(MTLPixelFormat format)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.colorAttachments[0].pixelFormat = format;
    return d;
}

struct Asked {
    raw::CacheKey key;  // the client's
    raw::CompileRequest request;
};

// A state of an app's own library (vertex_fn's AIR 0xA0..., fragment_fn's 0xA1...) as the client keys and asks it
Asked render(MTLRenderPipelineDescriptor *d, const raw::RenderVariant &variant)
{
    const raw::StageKey vertex{"vertex_fn", ""}, fragment{"fragment_fn", ""};
    const std::string packKey = raw::pipelineKey(d, &vertex, &fragment, "", "") + variant.name();
    raw::AirHash v, f;
    v.fill(0xA0);
    f.fill(0xA1);
    Asked a;
    a.key = raw::renderCacheKey(kStack, packKey, v, f, {}, true);
    raw::CompileRequest &r = a.request;
    r.stack = kStack;
    r.key = a.key.key;
    r.fields = raw::fieldsOf(d);
    r.variant = variant;
    r.libraries.push_back(data(made_metallib::universal({"vertex_fn", "fragment_fn"})));
    r.stages = {stage("v", "vertex_fn"), stage("f", "fragment_fn")};
    return a;
}

// The compiler's answer to `r`: "" where it takes it with the key it made in `made`, else why it refused it
std::string taken(const raw::CompileRequest &r, std::string *made = nullptr)
{
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    if (!switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why))
        return why.empty() ? "refused" : why;
    if (made)
        *made = p.key.key;
    return std::string();
}

void expectTaken(const Asked &a)
{
    std::string made;
    ASSERT_EQ(taken(a.request, &made), "");
    EXPECT_EQ(made, a.key.key);
}

void expectRefusedAt(const raw::CompileRequest &r, const std::string &field)
{
    const std::string why = taken(r);
    EXPECT_NE(why.find("first differing field \"" + field + "\" in its key"), std::string::npos) << why;
    EXPECT_NE(why.find("; its key "), std::string::npos) << why;
    EXPECT_NE(why.find("; the compiler's "), std::string::npos) << why;
}

TEST(CompileRequestForms, TwoPlaneColours)
{
    raw::RenderVariant planes;
    planes.twoPlane = true;
    const Asked blur = render(blurDownsample(), planes), solid = render(quad(MTLPixelFormat(550)), planes);
    expectTaken(blur);
    expectTaken(solid);
    raw::CompileRequest bare = blur.request;
    bare.variant = {};
    expectRefusedAt(bare, "| xr2p");
}

TEST(CompileRequestForms, WrittenFormatsAndSampledTwoPlaneTextures)
{
    raw::RenderVariant written;
    written.written = {{0, 23}, {3, 53}};
    const Asked a = render(quad(MTLPixelFormatRGBA8Unorm), written);
    EXPECT_NE(a.key.key.find(" | wtex=0:23,3:53"), std::string::npos) << a.key.key;
    expectTaken(a);
    raw::CompileRequest bare = a.request;
    bare.variant = {};
    expectRefusedAt(bare, "| wtex=0:23,3:53");

    raw::RenderVariant sampled;
    sampled.xrSlots = 0x5;
    sampled.fragmentTests = 2;
    sampled.cubic.samplers.push_back({0, 2, 2, 2, 0, true});
    sampled.cubic.textures.push_back({0, 80});
    expectTaken(render(quad(MTLPixelFormatRGBA8Unorm), sampled));
}

TEST(CompileRequestForms, VertexAmplification)
{
    for (const MTLPrimitiveTopologyClass topology :
         {MTLPrimitiveTopologyClassUnspecified, MTLPrimitiveTopologyClassTriangle}) {
        MTLRenderPipelineDescriptor *d = quad(MTLPixelFormatRGBA8Unorm);
        d.inputPrimitiveTopology = topology;
        d.maxVertexAmplificationCount = 2;
        const Asked a = render(d, {});
        EXPECT_NE(a.key.key.find(" amp=2 "), std::string::npos) << a.key.key;
        expectTaken(a);
        raw::CompileRequest bare = a.request;
        bare.fields.amplification = 1;
        expectRefusedAt(bare, "amp=2");
    }
}

// NativeStageInTest's kernel: a float2 per thread from buffer 1 at a static and at the dynamic stride
TEST(CompileRequestForms, AComputeStageInput)
{
    for (const NSUInteger stride : {NSUInteger(16), NSUInteger(MTLBufferLayoutStrideDynamic)}) {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        MTLStageInputOutputDescriptor *in = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        in.attributes[0].format = MTLAttributeFormatFloat2;
        in.attributes[0].bufferIndex = 1;
        in.layouts[1].stride = stride;
        in.layouts[1].stepFunction = MTLStepFunctionThreadPositionInGridX;
        d.stageInputDescriptor = in;
        raw::AirHash air;
        air.fill(0x30);
        const std::string constants = raw::stageInText(raw::stageInFieldsOf(d.stageInputDescriptor));
        EXPECT_EQ(constants, stride == 16 ? " | in a0=29:0:1:16:5:1 i=0:0" : " | in a0=29:0:1:4294967295:5:1 i=0:0");
        raw::CompileRequest r;
        r.stack = kStack;
        r.kind = raw::RequestKind::Kernel;
        r.libraries.push_back(data(made_metallib::universal({"stage_sum"}, 0x30)));
        r.stages = {stage("k", "stage_sum")};
        r.kernel.maxThreads = 0;
        r.kernel.threads[0] = 65;
        r.kernel.threads[1] = r.kernel.threads[2] = 1;
        r.kernel.stageIn = raw::stageInFieldsOf(d.stageInputDescriptor);
        r.key = "kernel | " + raw::computeKey(air, constants, 0, false, r.kernel.threads, "", "", "");
        std::string made;
        ASSERT_EQ(taken(r, &made), "");
        EXPECT_EQ(made, r.key);
        raw::CompileRequest bare = r;
        bare.kernel.stageIn = {};
        const std::string why = taken(bare);
        EXPECT_NE(why.find("a kernel request whose key is not its function's: first differing field \"| in\""),
                  std::string::npos)
            << why;
    }
}

TEST(CompileRequestForms, TheCompilersDescriptorCarriesEveryFieldTheClientRead)
{
    raw::PipelineFields f;
    f.samples = 4;
    f.topology = MTLPrimitiveTopologyClassTriangle;
    f.depthFormat = MTLPixelFormatDepth32Float;
    f.stencilFormat = MTLPixelFormatStencil8;
    f.rasterization = 0;
    f.amplification = 2;
    f.tessellation = {MTLTessellationPartitionModeFractionalOdd,
                      MTLTessellationFactorFormatHalf,
                      MTLTessellationFactorStepFunctionPerPatch,
                      MTLWindingCounterClockwise,
                      MTLTessellationControlPointIndexTypeUInt32,
                      32,
                      1};
    const uint64_t formats[8] = {70, 71, 80, 81, 90, 94, 115, 125};
    for (uint64_t i = 0; i < 8; i++)
        f.color[i] = {formats[i], 15 - i, i & 1, 4, 5, 1, 6, 7, 2};
    for (const uint64_t i : {0, 3, 30})
        f.attributes[i] = {MTLVertexFormatFloat4, 4 * i, i, 16 + 4 * i, MTLVertexStepFunctionPerInstance, 2};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    raw::applyFields(f, d);
    const raw::PipelineFields back = raw::fieldsOf(d);
    EXPECT_EQ(memcmp(&back, &f, sizeof f), 0);

    raw::StageInFields in;
    in.attributes[2] = {29, 8, 4, 24, 5, 1};
    in.attributes[7] = {31, 0, 6, UINT64_MAX, 6, 1};
    in.indexType = 1;
    in.indexBuffer = 9;
    const raw::StageInFields stageBack = raw::stageInFieldsOf(raw::stageInOf(in));
    EXPECT_EQ(memcmp(&stageBack, &in, sizeof in), 0);
    EXPECT_EQ(raw::stageInOf(raw::StageInFields()), nil);

    raw::RenderVariant v;
    v.cubic.samplers.push_back({1, 2, 2, 0, 1, false});
    v.fragmentTests = 3;
    v.twoPlane = true;
    v.xrSlots = 6;
    v.written = {{2, 63}};
    const raw::ProbeTextures t = raw::probeTexturesOf(v);
    EXPECT_EQ(t.cubic.key(), v.cubic.key());
    EXPECT_EQ(t.fragmentTests, 3);
    EXPECT_TRUE(t.twoPlane);
    EXPECT_EQ(t.xrSlots, 6);
    EXPECT_TRUE(t.written == v.written);
}

// Every field of a request, set: it reads back whole, and a change to any one field changes the bytes sent
TEST(CompileRequestForms, EveryFieldGoesOverTheWire)
{
    raw::CompileRequest r = render(blurDownsample(), {}).request;
    r.form = raw::RequestForm::Split;
    r.fields.amplification = 2;
    r.fields.patchType = 2;
    r.fields.tessellation = {1, 1, 2, 1, 2, 32, 1};
    r.stages.push_back(stage("vl", "vertex_fn"));
    r.stages.push_back(stage("vp", "vertex_fn"));
    r.stages.push_back(stage("fl", "fragment_fn"));
    r.stages.push_back(stage("fp", "fragment_fn"));
    r.stages[1].constants = "#0:53:01";
    r.stages[1].specialized = true;
    r.stages[1].name = "fragment_as";
    raw::RequestLibrary system;
    system.path = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
    system.system = true;
    r.libraries.push_back(system);
    r.stages[2].library = 1;
    r.variant.cubic.samplers.push_back({0, 2, 2, 2, 0, true});
    r.variant.cubic.textures.push_back({0, 80});
    r.variant.fragmentTests = 2;
    r.variant.twoPlane = true;
    r.variant.xrSlots = 3;
    r.variant.written = {{1, 23}};
    r.kernel.stageIn.attributes[0] = {29, 0, 1, 16, 5, 1};
    const std::string bytes = raw::encodeRequest(r);
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(bytes, back));
    EXPECT_EQ(raw::encodeRequest(back), bytes);

    const std::vector<std::pair<const char *, std::function<void(raw::CompileRequest &)>>> changes = {
        {"form",
         [](auto &q) {
             q.form = raw::RequestForm::Primary;
         }},
        {"samples",
         [](auto &q) {
             q.fields.samples = 4;
         }},
        {"rasterization",
         [](auto &q) {
             q.fields.rasterization = 0;
         }},
        {"amplification",
         [](auto &q) {
             q.fields.amplification = 3;
         }},
        {"patch type",
         [](auto &q) {
             q.fields.patchType = 1;
         }},
        {"tessellation",
         [](auto &q) {
             q.fields.tessellation.factorScale = 0;
         }},
        {"colour",
         [](auto &q) {
             q.fields.color[7].operationAlpha = 4;
         }},
        {"attribute",
         [](auto &q) {
             q.fields.attributes[30].rate = 9;
         }},
        {"constants",
         [](auto &q) {
             q.stages[1].constants = "#0:53:00";
         }},
        {"name",
         [](auto &q) {
             q.stages[1].name = "fragment_fn";
         }},
        {"linked library",
         [](auto &q) {
             q.stages[2].library = 0;
         }},
        {"cubic",
         [](auto &q) {
             q.variant.cubic.textures[0].format = 81;
         }},
        {"tests",
         [](auto &q) {
             q.variant.fragmentTests = 1;
         }},
        {"two planes",
         [](auto &q) {
             q.variant.twoPlane = false;
         }},
        {"sampled two-plane slots",
         [](auto &q) {
             q.variant.xrSlots = 1;
         }},
        {"written",
         [](auto &q) {
             q.variant.written[0].format = 63;
         }},
        {"stage input",
         [](auto &q) {
             q.kernel.stageIn.indexBuffer = 4;
         }},
        {"kernel textures",
         [](auto &q) {
             q.kernel.textures.push_back({1, 80, 2, 3, 1, 1, 1});
         }},
    };
    for (const auto &[what, change] : changes) {
        raw::CompileRequest changed = r;
        change(changed);
        EXPECT_NE(raw::encodeRequest(changed), bytes) << what;
        ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(changed), back)) << what;
        EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(changed)) << what;
    }

    raw::CompileRequest crowded = r;
    crowded.variant.written.assign(raw::kWrittenSlots + 1, {0, 23});
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(crowded), back));
}

// The paths no host model runs end to end (a variant's ask is answered asynchronously, a kernel's at its dispatch) fill the
// request from the same values the client's key is made of
TEST(CompileRequestForms, TheClientsRequestsCarryWhatItsKeysAreMadeOf)
{
    using switch_ios::test::Body;
    using switch_ios::test::SourceText;
    const std::string remote = SourceText("../raw/runtime/RawCompleteRemote.mm");
    const std::string render = Body(remote, "bool Completion::renderRequest(const Ask &a,");
    EXPECT_NE(render.find("r.fields = fieldsOf(d);"), std::string::npos);
    EXPECT_NE(render.find("r.variant = a.variant;"), std::string::npos);
    EXPECT_NE(Body(remote, "Compiler::Outcome Completion::remoteKernel(")
                  .find("q.stageIn = stageInFieldsOf(descriptor.stageInputDescriptor);"),
              std::string::npos);
    const std::string cubic = Body(SourceText("../raw/runtime/RawMetalCubic.mm"), "renderVariant(RMPipeline *p,");
    EXPECT_NE(cubic.find("const std::string suffix = forms.untested().name(), name = forms.name();"),
              std::string::npos);
    EXPECT_NE(cubic.find("request.key = p->_key + name;"), std::string::npos);
    EXPECT_NE(cubic.find("request.variant = forms;"), std::string::npos);
    EXPECT_NE(cubic.find("fallback.key = p->_key + suffix;"), std::string::npos);
    EXPECT_NE(cubic.find("fallback.variant = forms.untested();"), std::string::npos);
    EXPECT_NE(SourceText("../raw/runtime/RawMetalComputeState.mm")
                  .find("s->_constants = (f ? f->_constants : \"\") + "
                        "raw::stageInText(raw::stageInFieldsOf(descriptor.stageInputDescriptor));"),
              std::string::npos);
    EXPECT_NE(SourceText("../compiler/compiler_record.mm").find("raw::probeTexturesOf(p.request.variant)"),
              std::string::npos);
}

TEST(CompileRequestForms, TheRefusalNamesTheFirstFieldTheKeysPartAt)
{
    using switch_compiler::keyDifference;
    const std::string mine = "samples=1 topo=0 c0=80:15 amp=2  | v quad_vertex  | f solid_fragment | xr2p";
    EXPECT_EQ(keyDifference(mine, "samples=1 topo=0 c0=80:15  | v quad_vertex  | f solid_fragment | xr2p")
                  .rfind("first differing field \"amp=2\" in its key, \"| v\" in the compiler's", 0),
              0u);
    EXPECT_EQ(keyDifference(mine, "samples=1 topo=0 c0=80:15 amp=2  | v quad_vertex  | f solid_fragment")
                  .rfind("first differing field \"| xr2p\" in its key, \"(end)\" in the compiler's", 0),
              0u);
    const std::string longKey = std::string(2000, 'x') + " a=1";
    const std::string why = keyDifference(longKey, std::string(2000, 'x') + " a=2");
    EXPECT_LT(why.size(), 800u);
    EXPECT_NE(why.find("a=1"), std::string::npos);
    EXPECT_NE(why.find("a=2"), std::string::npos);
}

}  // namespace
