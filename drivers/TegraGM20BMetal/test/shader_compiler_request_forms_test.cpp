// SPDX-License-Identifier: MIT
// Each form of state boot 21's shader compiler refused as "a request whose fields give another key than its own" (Core
// Animation's blur chain and the solid fill into b3a8, a state's written formats, vertex amplification, a tessellation
// state, a compute stage input), keyed the way the client keys it (TegraGM20BMetalStates.mm's pipeline key and TegraGM20BMetalCubic.mm's variant name, the
// compute state's constants in TegraGM20BMetalComputeState.mm) and asked with the request the client makes of it
// (TegraGM20BMetalCompleteRemote.mm): the compiler makes the client's key of every one, and a request that leaves the form out is
// refused with the first field the two keys part at and both keys. Every field of a request goes over the wire, and the
// descriptor the compiler compiles from carries what the client's did.
#include "../TegraGM20BMetalCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../compute_key.h"
#include "StageInText.h"
#include "made_metallib.h"
#include "bazel/testing/source_text.h"

#include <gtest/gtest.h>
#include <functional>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

gm20b::RequestStage stage(const char *tag, const char *function)
{
    gm20b::RequestStage s;
    s.tag = tag;
    s.function = s.name = function;
    return s;
}

gm20b::RequestLibrary data(const std::string &bytes)
{
    gm20b::RequestLibrary l;
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
    gm20b::CacheKey key;  // the client's
    gm20b::CompileRequest request;
};

// A state of an app's own library (vertex_fn's AIR 0xA0..., fragment_fn's 0xA1...) as the client keys and asks it
Asked render(const gm20b::PipelineFields &fields, const gm20b::RenderVariant &variant)
{
    const gm20b::StageKey vertex{"vertex_fn", ""}, fragment{"fragment_fn", ""};
    const std::string packKey = gm20b::pipelineKey(fields, &vertex, &fragment, "", "") + variant.name();
    gm20b::AirHash v, f;
    v.fill(0xA0);
    f.fill(0xA1);
    Asked a;
    a.key = gm20b::renderCacheKey(kStack, packKey, v, f, {}, true);
    gm20b::CompileRequest &r = a.request;
    r.stack = kStack;
    r.key = a.key.key;
    r.fields = fields;
    r.variant = variant;
    r.libraries.push_back(data(made_metallib::universal({"vertex_fn", "fragment_fn"})));
    r.stages = {stage("v", "vertex_fn"), stage("f", "fragment_fn")};
    return a;
}

Asked render(MTLRenderPipelineDescriptor *d, const gm20b::RenderVariant &variant)
{
    return render(gm20b::fieldsOf(d), variant);
}

// The compiler's answer to `r`: "" where it takes it with the key it made in `made`, else why it refused it
std::string taken(const gm20b::CompileRequest &r, std::string *made = nullptr)
{
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    if (!switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why))
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

void expectRefusedAt(const gm20b::CompileRequest &r, const std::string &field)
{
    const std::string why = taken(r);
    EXPECT_NE(why.find("first differing field \"" + field + "\" in its key"), std::string::npos) << why;
    EXPECT_NE(why.find("; its key "), std::string::npos) << why;
    EXPECT_NE(why.find("; the compiler's "), std::string::npos) << why;
}

TEST(CompileRequestForms, WrittenFormatsAndSampledTwoPlaneTextures)
{
    gm20b::RenderVariant written;
    written.written = {{0, 23}, {3, 53}};
    const Asked a = render(quad(MTLPixelFormatRGBA8Unorm), written);
    EXPECT_NE(a.key.key.find(" | wtex=0:23,3:53"), std::string::npos) << a.key.key;
    expectTaken(a);
    gm20b::CompileRequest bare = a.request;
    bare.variant = {};
    expectRefusedAt(bare, "| wtex=0:23,3:53");

    gm20b::RenderVariant sampled;
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
        gm20b::CompileRequest bare = a.request;
        bare.fields.amplification = 1;
        expectRefusedAt(bare, "amp=2");
    }
}

// NativeM2vMissingTest's patch state: a quad patch's post-tessellation vertex function (the patch type fieldsOf reads off
// it), integer partitioning, factor 16 at most
TEST(CompileRequestForms, ATessellationState)
{
    MTLRenderPipelineDescriptor *d = quad(MTLPixelFormatRGBA8Unorm);
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    v.attributes[0].format = MTLVertexFormatFloat4;
    v.layouts[0].stride = 16;
    v.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
    d.vertexDescriptor = v;
    d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
    d.maxTessellationFactor = 16;
    gm20b::PipelineFields fields = gm20b::fieldsOf(d);
    fields.patchType = MTLPatchTypeQuad;
    const Asked a = render(fields, {});
    EXPECT_NE(a.key.key.find(" t=1:0:0:0:0:16:0 "), std::string::npos) << a.key.key;
    expectTaken(a);
    gm20b::CompileRequest bare = a.request;
    bare.fields.patchType = 0;
    expectRefusedAt(bare, "t=1:0:0:0:0:16:0");
}

// fieldsOf reads the patch type off the vertex function itself: this Mac's Metal makes a quad patch's
TEST(CompileRequestForms, TheVertexFunctionsPatchTypeIsAField)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
        GTEST_SKIP() << "no Metal device on this host";
    NSString *source = @"#include <metal_stdlib>\n"
                        "using namespace metal;\n"
                        "struct ControlPoint { float4 position [[attribute(0)]]; };\n"
                        "struct QuadPatch { patch_control_point<ControlPoint> points; };\n"
                        "[[patch(quad, 4)]] vertex float4 missing_post(QuadPatch patch [[stage_in]],\n"
                        "                                              float2 uv [[position_in_patch]]) {\n"
                        "    return mix(patch.points[0].position, patch.points[2].position, uv.x);\n"
                        "}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    ASSERT_TRUE(library != nil) << (error ? error.localizedDescription.UTF8String : "");
    MTLRenderPipelineDescriptor *d = quad(MTLPixelFormatRGBA8Unorm);
    d.vertexFunction = [library newFunctionWithName:@"missing_post"];
    EXPECT_EQ(gm20b::fieldsOf(d).patchType, uint64_t(MTLPatchTypeQuad));
}

// The key text pipelineKey makes of a descriptor's fields is the one it made of the descriptor itself before (pinned from
// that form's output, boot 21's log and its packs): the blur downsample's is the key iOS 17's pack holds its record under
TEST(CompileRequestForms, TheKeyTextIsTheDescriptorsOwn)
{
    const gm20b::StageKey v{"downsample_blur_vert_lph", ""}, f{"downsample_4_frag_lph", ""};
    const std::string blur = gm20b::pipelineKey(blurDownsample(), &v, &f, "", "QuartzCore");
    EXPECT_EQ(blur, "samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 "
                    "a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1  | v downsample_blur_vert_lph  | f "
                    "downsample_4_frag_lph");
    EXPECT_EQ(gm20b::Pack::fnv(blur.data(), blur.size()), 0x9caf7830314b781eull);

    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.rasterSampleCount = 4;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.colorAttachments[0].blendingEnabled = YES;
    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationMax;
    d.colorAttachments[7].pixelFormat = MTLPixelFormatRGBA16Float;
    d.colorAttachments[7].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskAlpha;
    MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
    layout.attributes[0].format = MTLVertexFormatFloat2;
    layout.attributes[0].bufferIndex = 1;
    layout.attributes[3].format = MTLVertexFormatUChar4Normalized;
    layout.attributes[3].offset = 8;
    layout.attributes[3].bufferIndex = 3;
    layout.attributes[30].format = MTLVertexFormatFloat4;
    layout.attributes[30].bufferIndex = 5;
    layout.layouts[1].stride = 8;
    layout.layouts[3].stride = 20;
    layout.layouts[3].stepFunction = MTLVertexStepFunctionPerInstance;
    layout.layouts[3].stepRate = 2;
    layout.layouts[5].stride = MTLBufferLayoutStrideDynamic;
    d.vertexDescriptor = layout;
    d.maxVertexAmplificationCount = 2;
    const gm20b::StageKey cv{"vertex_fn", "#0:33:05000000"}, cf{"fragment_fn", "fc_generic:53:01"};
    EXPECT_EQ(
        gm20b::pipelineKey(d, &cv, &cf, " | fl helper  ", "RenderBox"),
        "samples=4 topo=3 ds=260/260 c0=80:15:1:4:5:0:1:0:4 c7=115:9:0:1:0:0:1:0:0 a0=29:0:1:8:1:1 a3=9:8:3:20:2:2 "
        "a30=31:0:5:4294967295:1:1 amp=2  | v vertex_fn #0:33:05000000 | f fragment_fn fc_generic:53:01 | fl "
        "helper   | l RenderBox");
    gm20b::PipelineFields patched = gm20b::fieldsOf(d);
    patched.patchType = MTLPatchTypeTriangle;
    patched.tessellation = {MTLTessellationPartitionModeFractionalEven,
                            MTLTessellationFactorFormatHalf,
                            MTLTessellationFactorStepFunctionPerPatchAndPerInstance,
                            MTLWindingCounterClockwise,
                            MTLTessellationControlPointIndexTypeUInt16,
                            32,
                            1};
    EXPECT_EQ(
        gm20b::pipelineKey(patched, &cv, &cf, "", ""),
        "samples=4 topo=3 ds=260/260 c0=80:15:1:4:5:0:1:0:4 c7=115:9:0:1:0:0:1:0:0 a0=29:0:1:8:1:1 a3=9:8:3:20:2:2 "
        "a30=31:0:5:4294967295:1:1 t=3:0:3:1:1:32:1 amp=2  | v vertex_fn #0:33:05000000 | f fragment_fn "
        "fc_generic:53:01");
    EXPECT_EQ(gm20b::pipelineKey(gm20b::PipelineFields(), nullptr, nullptr, "", ""),
              "samples=1 topo=0 ds=0/0  | v -  | f -");
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
        gm20b::AirHash air;
        air.fill(0x30);
        const std::string constants = gm20b::stageInText(gm20b::stageInFieldsOf(d.stageInputDescriptor));
        EXPECT_EQ(constants, stride == 16 ? " | in a0=29:0:1:16:5:1 i=0:0" : " | in a0=29:0:1:4294967295:5:1 i=0:0");
        gm20b::CompileRequest r;
        r.stack = kStack;
        r.kind = gm20b::RequestKind::Kernel;
        r.libraries.push_back(data(made_metallib::universal({"stage_sum"}, 0x30)));
        r.stages = {stage("k", "stage_sum")};
        r.kernel.maxThreads = 0;
        r.kernel.threads[0] = 65;
        r.kernel.threads[1] = r.kernel.threads[2] = 1;
        r.kernel.stageIn = gm20b::stageInFieldsOf(d.stageInputDescriptor);
        r.key = "kernel | " + gm20b::computeKey(air, constants, 0, false, r.kernel.threads, "", "", "");
        std::string made;
        ASSERT_EQ(taken(r, &made), "");
        EXPECT_EQ(made, r.key);
        gm20b::CompileRequest bare = r;
        bare.kernel.stageIn = {};
        const std::string why = taken(bare);
        EXPECT_NE(why.find("a kernel request whose key is not its function's: first differing field \"| in\""),
                  std::string::npos)
            << why;
    }
}

TEST(CompileRequestForms, TheCompilersDescriptorCarriesEveryFieldTheClientRead)
{
    gm20b::PipelineFields f;
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
    gm20b::applyFields(f, d);
    const gm20b::PipelineFields back = gm20b::fieldsOf(d);
    EXPECT_EQ(memcmp(&back, &f, sizeof f), 0);

    gm20b::StageInFields in;
    in.attributes[2] = {29, 8, 4, 24, 5, 1};
    in.attributes[7] = {31, 0, 6, UINT64_MAX, 6, 1};
    in.indexType = 1;
    in.indexBuffer = 9;
    const gm20b::StageInFields stageBack = gm20b::stageInFieldsOf(gm20b::stageInOf(in));
    EXPECT_EQ(memcmp(&stageBack, &in, sizeof in), 0);
    EXPECT_EQ(gm20b::stageInOf(gm20b::StageInFields()), nil);
    // the frontend's journal writes a D line's stage input as the runtime keys it (pack/kernel_shapes.h reads it back)
    EXPECT_EQ(stageInText(gm20b::stageInOf(in)), gm20b::stageInText(in));

    gm20b::RenderVariant v;
    v.cubic.samplers.push_back({1, 2, 2, 0, 1, false});
    v.fragmentTests = 3;
    v.xrSlots = 6;
    v.written = {{2, 63}};
    const gm20b::ProbeTextures t = gm20b::probeTexturesOf(v);
    EXPECT_EQ(t.cubic.key(), v.cubic.key());
    EXPECT_EQ(t.fragmentTests, 3);
    EXPECT_EQ(t.xrSlots, 6);
    EXPECT_TRUE(t.written == v.written);
}

// Every field of a request, set: it reads back whole, and a change to any one field changes the bytes sent
TEST(CompileRequestForms, EveryFieldGoesOverTheWire)
{
    gm20b::CompileRequest r = render(blurDownsample(), {}).request;
    r.form = gm20b::RequestForm::Split;
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
    gm20b::RequestLibrary system;
    system.path = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
    system.system = true;
    r.libraries.push_back(system);
    r.stages[2].library = 1;
    r.variant.cubic.samplers.push_back({0, 2, 2, 2, 0, true});
    r.variant.cubic.textures.push_back({0, 80});
    r.variant.fragmentTests = 2;
    r.variant.xrSlots = 3;
    r.variant.written = {{1, 23}};
    r.kernel.stageIn.attributes[0] = {29, 0, 1, 16, 5, 1};
    const std::string bytes = gm20b::encodeRequest(r);
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(bytes, back));
    EXPECT_EQ(gm20b::encodeRequest(back), bytes);

    const std::vector<std::pair<const char *, std::function<void(gm20b::CompileRequest &)>>> changes = {
        {"form",
         [](auto &q) {
             q.form = gm20b::RequestForm::Primary;
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
             q.kernel.textures.push_back({1, 0, 80, 2, 3, 1, 1, 1});
         }},
    };
    for (const auto &[what, change] : changes) {
        gm20b::CompileRequest changed = r;
        change(changed);
        EXPECT_NE(gm20b::encodeRequest(changed), bytes) << what;
        ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(changed), back)) << what;
        EXPECT_EQ(gm20b::encodeRequest(back), gm20b::encodeRequest(changed)) << what;
    }

    gm20b::CompileRequest crowded = r;
    crowded.variant.written.assign(gm20b::kWrittenSlots + 1, {0, 23});
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(crowded), back));
}

// The paths no host model runs end to end (a variant's ask is answered asynchronously, a kernel's at its dispatch) fill the
// request from the same values the client's key is made of
TEST(CompileRequestForms, TheClientsRequestsCarryWhatItsKeysAreMadeOf)
{
    using switch_ios::test::Body;
    using switch_ios::test::SourceText;
    const std::string remote = SourceText("../TegraGM20BMetalCompleteRemote.mm");
    const std::string render = Body(remote, "bool Completion::renderRequest(const Ask &a,");
    EXPECT_NE(render.find("r.fields = fieldsOf(d);"), std::string::npos);
    EXPECT_NE(render.find("r.variant = a.variant;"), std::string::npos);
    EXPECT_NE(Body(remote, "Compiler::Outcome Completion::remoteKernel(")
                  .find("q.stageIn = stageInFieldsOf(descriptor.stageInputDescriptor);"),
              std::string::npos);
    EXPECT_NE(SourceText("../compiler/compiler_record.mm").find("gm20b::probeTexturesOf(p.request.variant)"),
              std::string::npos);
}

// A compute state's key carries its function's constants, its stage input and its linked functions in the order
// TegraGM20BMetalComputeState.mm keys it by (gm20b::computeStateConstants); the compiler remakes that key from a request holding
// the three, and refuses a request missing any one of them
TEST(CompileRequestForms, AComputeStatesConstantsStageInputAndLinkedFunctionsAreItsKey)
{
    MTLStageInputOutputDescriptor *in = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    in.attributes[0].format = MTLAttributeFormatFloat2;
    in.attributes[0].bufferIndex = 1;
    in.layouts[1].stride = 16;
    in.layouts[1].stepFunction = MTLStepFunctionThreadPositionInGridX;
    const std::string constants = "c0=29:0000803f", helperConstants = "c1=33:02000000";
    gm20b::LinkedComputeFunction helper;
    helper.air.fill(0x31);
    helper.constants = helperConstants;
    helper.symbol = helper.name = "helper";
    gm20b::AirHash air;
    air.fill(0x30);
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Kernel;
    r.libraries.push_back(data(made_metallib::universal({"stage_sum", "helper"}, 0x30)));
    r.stages = {stage("k", "stage_sum"), stage("kl", "helper")};
    r.stages[0].constants = constants;
    r.stages[1].constants = helperConstants;
    r.kernel.threads[0] = 65;
    r.kernel.threads[1] = r.kernel.threads[2] = 1;
    r.kernel.stageIn = gm20b::stageInFieldsOf(in);
    const std::string state =
        gm20b::computeStateConstants(constants, gm20b::stageInText(gm20b::stageInFieldsOf(in)), {helper});
    r.key = "kernel | " + gm20b::computeKey(air, state, 0, false, r.kernel.threads, "", "", "");
    std::string made;
    ASSERT_EQ(taken(r, &made), "");
    EXPECT_EQ(made, r.key);
    gm20b::CompileRequest unconstant = r, bare = r, unlinked = r;
    unconstant.stages[0].constants.clear();
    bare.kernel.stageIn = {};
    unlinked.stages.pop_back();
    const std::pair<const char *, const gm20b::CompileRequest *> lacking[] = {
        {"constants", &unconstant}, {"stage input", &bare}, {"linked function", &unlinked}};
    for (const auto &[what, request] : lacking) {
        const std::string why = taken(*request);
        EXPECT_NE(why.find("a kernel request whose key is not its function's"), std::string::npos)
            << what << ": " << why;
    }
}

TEST(CompileRequestForms, TheRefusalNamesTheFirstFieldTheKeysPartAt)
{
    using switch_compiler::keyDifference;
    const std::string mine = "samples=1 topo=0 c0=80:15 amp=2  | v quad_vertex  | f solid_fragment | tests=2";
    EXPECT_EQ(keyDifference(mine, "samples=1 topo=0 c0=80:15  | v quad_vertex  | f solid_fragment | tests=2")
                  .rfind("first differing field \"amp=2\" in its key, \"| v\" in the compiler's", 0),
              0u);
    EXPECT_EQ(keyDifference(mine, "samples=1 topo=0 c0=80:15 amp=2  | v quad_vertex  | f solid_fragment")
                  .rfind("first differing field \"| tests=2\" in its key, \"(end)\" in the compiler's", 0),
              0u);
    const std::string longKey = std::string(2000, 'x') + " a=1";
    const std::string why = keyDifference(longKey, std::string(2000, 'x') + " a=2");
    EXPECT_LT(why.size(), 800u);
    EXPECT_NE(why.find("a=1"), std::string::npos);
    EXPECT_NE(why.find("a=2"), std::string::npos);
}

}  // namespace
