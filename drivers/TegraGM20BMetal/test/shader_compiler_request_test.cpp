// SPDX-License-Identifier: MIT
// A compile request (raw/runtime/raw_compile_request.h) as the client makes it and the compiler takes it
// (compiler/compiler_request.h): it round-trips, its fields key a descriptor as the descriptor itself keys, the compiler
// derives the runtime's own key from it and refuses one whose claimed key differs, and the store name binds what the
// key does not (a library's bytes, rasterization, a kernel's dispatch).
#include "../raw/runtime/RawCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../raw/runtime/raw_compute_key.h"
#include "compiler_request_layout03.h"
#include "../raw/runtime/raw_function_attributes.h"
#include "made_metallib.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

raw::RequestStage stage(const char *tag, const char *function, uint32_t library = 0, const char *constants = "")
{
    raw::RequestStage s;
    s.tag = tag;
    s.function = function;
    s.name = function;
    s.constants = constants;
    s.specialized = constants[0] != 0;
    s.library = library;
    return s;
}

raw::RequestLibrary data(const std::string &bytes)
{
    raw::RequestLibrary l;
    l.bytes = bytes;
    return l;
}

raw::RequestLibrary systemLibrary(const char *path)
{
    raw::RequestLibrary l;
    l.path = path;
    l.system = true;
    return l;
}

MTLRenderPipelineDescriptor *descriptor()
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.rasterSampleCount = 4;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.rasterizationEnabled = NO;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.colorAttachments[0].blendingEnabled = YES;
    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[2].pixelFormat = MTLPixelFormatRGBA16Float;
    d.colorAttachments[2].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskAlpha;
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    v.attributes[0].format = MTLVertexFormatFloat2;
    v.attributes[0].bufferIndex = 1;
    v.attributes[3].format = MTLVertexFormatUChar4Normalized;
    v.attributes[3].offset = 8;
    v.attributes[3].bufferIndex = 3;
    v.layouts[1].stride = 8;
    v.layouts[3].stride = 20;
    v.layouts[3].stepFunction = MTLVertexStepFunctionPerInstance;
    v.layouts[3].stepRate = 2;
    d.vertexDescriptor = v;
    return d;
}

// A render request over a made-up library holding vertex_fn (HASH 0xA0...) and fragment_fn (0xA1...)
raw::CompileRequest renderRequest()
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.fields = raw::fieldsOf(descriptor());
    r.libraries.push_back(data(made_metallib::universal({"vertex_fn", "fragment_fn"})));
    r.stages = {stage("v", "vertex_fn"), stage("f", "fragment_fn", 0, "#0:33:05000000")};
    return r;
}

// The CacheKey the runtime makes for `r`'s state (RawComplete.mm keyOf over RawRenderKey.mm)
raw::CacheKey runtimeKey(const raw::CompileRequest &r)
{
    bool own = false;
    const std::string key = raw::requestPackKey(r, own);
    raw::AirHash vertex, fragment;
    vertex.fill(0xA0);
    fragment.fill(0xA1);
    return raw::renderCacheKey(r.stack, key, vertex, fragment, {}, own);
}

std::vector<raw::LibraryIdentity> identities(const raw::CompileRequest &r)
{
    std::vector<raw::LibraryIdentity> out;
    for (const raw::RequestLibrary &l : r.libraries)
        out.push_back(raw::identityOf(l));
    return out;
}

TEST(CompileRequest, RoundTripsWhole)
{
    raw::CompileRequest r = renderRequest();
    r.stages.push_back(stage("fl", "fragment_fn", 0));
    r.variant.cubic.samplers.push_back({2, 1, 1, 0, 3, true});
    r.variant.cubic.textures.push_back({2, 80});
    r.variant.fragmentTests = 3;
    r.variant.xrShadows = 5;
    r.key = "a key";
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(r));
    EXPECT_EQ(back.variant.xrShadows, 5);

    raw::CompileRequest k;
    k.stack = kStack;
    k.kind = raw::RequestKind::Kernel;
    k.key = "kernel | a key";
    k.libraries.push_back(data("bytes"));
    k.stages = {stage("k", "kernel_fn")};
    k.kernel.maxThreads = 256;
    k.kernel.threads[0] = 64;
    k.kernel.memory[2] = 1024;
    k.kernel.textures.push_back({1, 0, 80, 2, 3, 1, 1, 1});
    NmbDirectBoundSampler sampler = {};
    sampler.slot = 4;
    sampler.descriptor.max_anisotropy = 8;
    k.kernel.samplers.push_back(sampler);
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(k), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(k));
    EXPECT_EQ(back.kernel.dispatch().memory[2], 1024u);
}

// A vertex stage's cubic binds keep their stage through the request (the sampler's flag byte's bit 1, the texture's
// slot byte's top bit) and name their variant "vs" and "vt"; a fragment-only request's bytes and variant name are as
// packed
TEST(CompileRequest, AVertexCubicBindKeepsItsStage)
{
    raw::CompileRequest fragment = renderRequest();
    fragment.key = "a key";
    fragment.variant.cubic.samplers.push_back({0, 0, 0, 0, 0, true});
    fragment.variant.cubic.textures.push_back({0, 80});
    EXPECT_EQ(fragment.variant.cubic.key(), " | cubic s0:0000n t0:80");
    raw::CompileRequest both = fragment;
    both.variant.cubic.samplers.push_back({3, 2, 2, 0, 1, false, raw::kCubicVertex});
    both.variant.cubic.textures.push_back({5, 70, raw::kCubicVertex});
    EXPECT_EQ(both.variant.cubic.key(), " | cubic s0:0000n vs3:2201p t0:80 vt5:70");
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(both), back));
    const raw::CubicBinds &cubic = back.variant.cubic;
    ASSERT_EQ(cubic.samplers.size(), 2u);
    ASSERT_EQ(cubic.textures.size(), 2u);
    EXPECT_EQ(cubic.samplers[0].stage, raw::kCubicFragment);
    EXPECT_TRUE(cubic.samplers[0].normalized);
    EXPECT_EQ(cubic.samplers[1].stage, raw::kCubicVertex);
    EXPECT_EQ(cubic.samplers[1].slot, 3u);
    EXPECT_FALSE(cubic.samplers[1].normalized);
    EXPECT_EQ(cubic.textures[1].stage, raw::kCubicVertex);
    EXPECT_EQ(cubic.textures[1].slot, 5u);
    EXPECT_EQ(cubic.key(), both.variant.cubic.key());
    const std::string bytes = raw::encodeRequest(fragment);
    const std::string flags = std::string("\0\0\0\0\0\1", 6);
    EXPECT_NE(bytes.find(flags), std::string::npos) << "a fragment sampler's six bytes, its flags only normalized";
}

// A render request with every variant form layout 03 carried: a fragment cubic bind, a test class, two-plane colours,
// sampled two-plane slots and a written texture
raw::CompileRequest layout03Request()
{
    raw::CompileRequest r = renderRequest();
    r.key = "a key";
    r.variant.cubic.samplers.push_back({9, 2, 1, 2, 3, true});
    r.variant.cubic.textures.push_back({9, 80});
    r.variant.fragmentTests = 2;
    r.variant.twoPlane = true;
    r.variant.xrSlots = 4;
    r.variant.written = {{2, 23}};
    return r;
}

TEST(CompileRequest, ItsLayoutIsSWCREQ05)
{
    EXPECT_EQ(raw::encodeRequest(layout03Request()).substr(0, 8), "SWCREQ05");
}

TEST(CompileRequest, ARequestOfLayout04IsRefused)
{
    std::string bytes = layout03::encode(layout03Request(), true);
    bytes.replace(0, 8, "SWCREQ04");
    raw::CompileRequest back;
    EXPECT_FALSE(raw::decodeRequest(bytes, back));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared prepared;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(bytes, kStack, system, prepared, why));
    EXPECT_EQ(why, "an unreadable request");
}

// A request of layout 03, which a client of an older build sends or the compiler's request log (compiler_log.h) kept,
// is refused whole by the reader and by prepare, which the log's replay runs
TEST(CompileRequest, ARequestOfLayout03IsRefused)
{
    const raw::CompileRequest r = layout03Request();
    const std::string bytes = layout03::encode(r);
    raw::CompileRequest back;
    EXPECT_FALSE(raw::decodeRequest(bytes, back));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(bytes, kStack, system, p, why));
    EXPECT_EQ(why, "an unreadable request");

    raw::CompileRequest merged = r;
    merged.variant.cubic.samplers.push_back({3, 2, 2, 0, 1, false, raw::kCubicVertex});
    merged.variant.cubic.textures.push_back({5, 70, raw::kCubicVertex});
    merged.variant.xrShadows = 1;
    EXPECT_FALSE(raw::decodeRequest(layout03::encode(merged, true), back));
}

TEST(CompileRequest, VertexCubicBindsAndShadowedColoursRoundTrip)
{
    raw::CompileRequest r = layout03Request();
    r.variant.cubic.samplers.push_back({3, 2, 2, 0, 1, false, raw::kCubicVertex});
    r.variant.cubic.textures.push_back({5, 70, raw::kCubicVertex});
    r.variant.xrShadows = 0x81;
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(r));
    EXPECT_EQ(back.variant.name(), r.variant.name());
    EXPECT_EQ(back.variant.xrShadows, 0x81);
    ASSERT_EQ(back.variant.cubic.samplers.size(), 2u);
    ASSERT_EQ(back.variant.cubic.textures.size(), 2u);
    EXPECT_EQ(back.variant.cubic.samplers[1].stage, raw::kCubicVertex);
    EXPECT_EQ(back.variant.cubic.textures[1].stage, raw::kCubicVertex);
    EXPECT_EQ(back.variant.cubic.textures[1].slot, 5u);
    EXPECT_EQ(back.variant.fragmentTests, 2);
    EXPECT_TRUE(back.variant.twoPlane);
    EXPECT_EQ(back.variant.xrSlots, 4);
}

// A cubic sampler's flag byte holds normalized (bit 0) and the vertex stage (bit 1), nothing else
TEST(CompileRequest, ASamplerFlagThisLayoutDoesNotDefineIsRefused)
{
    std::string bytes = raw::encodeRequest(layout03Request());
    const size_t at = bytes.find(std::string("\x09\x02\x01\x02\x03\x01", 6));
    ASSERT_NE(at, std::string::npos);
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(bytes, back));
    bytes[at + 5] = 0x05;
    EXPECT_FALSE(raw::decodeRequest(bytes, back));
}

TEST(CompileRequest, ItsFieldsKeyADescriptorAsTheDescriptorKeysItself)
{
    MTLRenderPipelineDescriptor *d = descriptor(), *made = [MTLRenderPipelineDescriptor new];
    const raw::PipelineFields fields = raw::fieldsOf(d);
    raw::applyFields(fields, made);
    const raw::StageKey v{"vertex_fn", ""}, f{"fragment_fn", "#0:33:05000000"};
    EXPECT_EQ(raw::pipelineKey(made, &v, &f, "", ""), raw::pipelineKey(d, &v, &f, "", ""));
    EXPECT_EQ(raw::fieldsOf(made).rasterization, 0u);
    EXPECT_FALSE(made.rasterizationEnabled);
}

TEST(CompileRequest, ItsPackKeyIsTheFacadesForFrameworkAndOwnLibraries)
{
    raw::CompileRequest r = renderRequest();
    r.libraries = {systemLibrary("/System/Library/Frameworks/RenderBox.framework/default.metallib")};
    bool own = true;
    const raw::StageKey v{"vertex_fn", ""}, f{"fragment_fn", "#0:33:05000000"};
    EXPECT_EQ(raw::requestPackKey(r, own), raw::pipelineKey(descriptor(), &v, &f, "", "RenderBox"));
    EXPECT_FALSE(own);
    r.variant.cubic.samplers.push_back({0, 2, 2, 2, 0, true});
    r.variant.fragmentTests = 2;
    EXPECT_EQ(raw::requestPackKey(r, own),
              raw::pipelineKey(descriptor(), &v, &f, "", "RenderBox") + r.variant.cubic.key() + " | tests=2");
    r.variant.xrShadows = 1;
    EXPECT_EQ(raw::requestPackKey(r, own),
              raw::pipelineKey(descriptor(), &v, &f, "", "RenderBox") + r.variant.cubic.key() + " | tests=2 | xrsh=1");
    raw::CompileRequest data = renderRequest();
    EXPECT_EQ(raw::requestPackKey(data, own), raw::pipelineKey(descriptor(), &v, &f, "", ""));
    EXPECT_TRUE(own);
}

TEST(CompileRequest, TheCompilerKeysItAsTheRuntimeDoes)
{
    raw::CompileRequest r = renderRequest();
    const raw::CacheKey k = runtimeKey(r);
    r.key = k.key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, k.key);
    EXPECT_EQ(memcmp(p.key.vertexAir, k.vertexAir, 32), 0);
    EXPECT_EQ(memcmp(p.key.fragmentAir, k.fragmentAir, 32), 0);
    EXPECT_EQ(p.name, raw::storeName(k, r, identities(r)));
}

TEST(CompileRequest, OneWhoseClaimedKeyDiffersIsRefused)
{
    raw::CompileRequest r = renderRequest();
    r.key = runtimeKey(r).key + " ";
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why));
    EXPECT_NE(why.find("another key"), std::string::npos) << why;
    r.key = runtimeKey(r).key;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(r), kStack + 1, system, p, why));
}

TEST(CompileRequest, TheStoreNameBindsLibraryBytesAndRasterization)
{
    raw::CompileRequest r = renderRequest();
    const raw::CacheKey k = runtimeKey(r);
    const raw::Digest name = raw::storeName(k, r, identities(r));
    EXPECT_EQ(raw::storeName(k, r, identities(r)), name);
    raw::CompileRequest alike = r;
    alike.libraries[0] = data(made_metallib::universal({"vertex_fn", "fragment_fn"}, 0xA0, 0x11));
    EXPECT_EQ(runtimeKey(alike).key, k.key);
    EXPECT_NE(raw::storeName(k, alike, identities(alike)), name);
    raw::CompileRequest rasterizing = r;
    rasterizing.fields.rasterization = 1;
    EXPECT_NE(raw::storeName(k, rasterizing, identities(rasterizing)), name);
}

// A state of no fragment function (RenderBox's path_edges) is asked by its vertex stage alone: the request round-trips, keys
// "f -" with no fragment AIR, as the facade keys it, and the compiler's descriptor of it has no fragment function
TEST(CompileRequest, AStateOfNoFragmentFunctionIsAskedByItsVertexStageAlone)
{
    raw::CompileRequest r = renderRequest();
    r.stages = {stage("v", "vertex_fn"), stage("vl", "fragment_fn")};
    EXPECT_EQ(raw::fragmentStage(r), raw::kNoStage);
    EXPECT_EQ(raw::firstLinked(r), 1u);
    bool own = false;
    const raw::StageKey v{"vertex_fn", ""};
    const std::string packKey = raw::requestPackKey(r, own);
    EXPECT_EQ(packKey, raw::pipelineKey(descriptor(), &v, nullptr, " | vl fragment_fn ", ""));
    EXPECT_NE(packKey.find(" | f -"), std::string::npos) << packKey;
    raw::AirHash vertex, none{}, linked;
    vertex.fill(0xA0);
    linked.fill(0xA1);
    const raw::CacheKey k = raw::renderCacheKey(kStack, packKey, vertex, none, {{'v', linked}}, own);
    r.key = k.key;
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(r));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, k.key);
    EXPECT_EQ(memcmp(p.key.fragmentAir, none.data(), 32), 0);

    raw::CompileRequest two = renderRequest();
    EXPECT_EQ(raw::fragmentStage(two), 1u);
    EXPECT_EQ(raw::firstLinked(two), 2u);
    raw::CompileRequest misplaced = renderRequest();
    misplaced.stages = {stage("v", "vertex_fn"), stage("vl", "vertex_fn"), stage("f", "fragment_fn")};
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(misplaced), back));
}

// A state Metal does not rasterize keys rast=0 after its attachments and vertex layout; one it does names nothing
TEST(CompileRequest, TheKeyNamesAStateMetalDoesNotRasterize)
{
    MTLRenderPipelineDescriptor *d = descriptor();
    const raw::StageKey v{"vertex_fn", ""};
    const std::string off = raw::pipelineKey(d, &v, nullptr, "", "");
    EXPECT_NE(off.find(" a3=9:8:3:20:2:2 rast=0  | v vertex_fn"), std::string::npos) << off;
    d.rasterizationEnabled = YES;
    EXPECT_EQ(raw::pipelineKey(d, &v, nullptr, "", "").find("rast="), std::string::npos);
}

// A Reflection asks a render state's reflection by the render request's stages and fields, keyed as the state is and named
// apart from its record; a compute state's by its kernel's one stage and limits, keyed by its computeStateKey alone
TEST(CompileRequest, AReflectionIsKeyedAsItsStateAndNamedApart)
{
    raw::CompileRequest render = renderRequest();
    const raw::CacheKey k = runtimeKey(render);
    render.key = k.key;
    raw::CompileRequest reflection = render;
    reflection.kind = raw::RequestKind::Reflection;
    EXPECT_FALSE(raw::computeReflection(reflection));
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(reflection), back));
    EXPECT_EQ(back.kind, raw::RequestKind::Reflection);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(reflection), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, k.key);
    EXPECT_NE(p.name, raw::storeName(k, render, identities(render)));

    raw::CompileRequest compute;
    compute.stack = kStack;
    compute.kind = raw::RequestKind::Reflection;
    compute.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    compute.stages = {stage("k", "kernel_fn")};
    compute.kernel.maxThreads = 512;
    raw::AirHash air;
    air.fill(0x30);
    const std::string state = raw::computeStateKey(air, "", 512, false);
    compute.key = raw::computeReflectionKey(kStack, state).key;
    EXPECT_TRUE(raw::computeReflection(compute));
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(compute), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);
    raw::CompileRequest limits = compute;
    limits.kernel.maxThreads = 256;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(limits), kStack, system, p, why));
    raw::CompileRequest split = compute;
    split.form = raw::RequestForm::Split;
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(split), back));
}

TEST(CompileRequest, AKernelIsKeyedByItsFunctionAndNamedByItsDispatch)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Kernel;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn")};
    r.kernel.maxThreads = 1024;
    r.kernel.threads[0] = 64;
    r.kernel.threads[1] = r.kernel.threads[2] = 1;
    r.kernel.textures.push_back({0, 0, 80, 2, 0x17, 1, 1, 1});
    raw::AirHash air;
    air.fill(0x30);
    const std::string compute = raw::computeKey(air, "", 1024, false, r.kernel.threads, "", "0:80:2:1:1:00010203", "");
    r.key = "kernel | " + compute;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, compute);
    EXPECT_EQ(p.key.key, raw::kernelCacheKey(kStack, compute).key);

    raw::CompileRequest used = r;
    used.kernel.textures[0].usage = 0x13;
    EXPECT_NE(raw::storeName(p.key, used, identities(used)), p.name);

    raw::CompileRequest region = r;
    region.kernel.regionMode = true;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(region), kStack, system, p, why));
    raw::CompileRequest other = r;
    other.libraries[0] = data(made_metallib::universal({"kernel_fn"}, 0x31));
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(other), kStack, system, p, why));
}

// A kernel's store name binds its dispatch as before requests carried a stage input (which its key binds): a store a
// device kept from an earlier build still names each kernel it holds (the digest is the one format 02's requests made)
TEST(CompileRequest, AKernelKeepsTheStoreNameItHadBeforeItsRequestCarriedItsStageInput)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Kernel;
    r.libraries.push_back(data("library bytes"));
    r.stages = {stage("k", "kernel_fn")};
    r.kernel.maxThreads = 1024;
    r.kernel.threads[0] = 64;
    r.kernel.threads[1] = r.kernel.threads[2] = 1;
    r.kernel.memory[2] = 4096;
    NmbDirectBoundTexture t;
    memset(&t, 0, sizeof t);
    t.pixelFormat = 80;
    t.textureType = 2;
    t.usage = 0x17;
    t.levels = t.arrayLength = t.samples = 1;
    r.kernel.textures.push_back(t);
    const raw::Digest name = raw::storeName(raw::kernelCacheKey(kStack, "a compute key"), r, identities(r));
    std::string hex;
    for (uint8_t b : name) {
        char two[3];
        snprintf(two, sizeof two, "%02x", b);
        hex += two;
    }
    EXPECT_EQ(hex, "ef9afce2f43ca2e100a94e8bff343b186d9d01edc8efbbb3bdc6b84a8eae1758");
}

// A compute state's facts: keyed by its function's AIR, constants and limits as the pack keys PackComputeFacts, named apart
// from a kernel request of the same function and limits, and refused when its claimed key names another state
TEST(CompileRequest, ComputeFactsAreKeyedAsThePacksStateKey)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Facts;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn", 0, "#0:33:05000000")};
    r.kernel.maxThreads = 256;
    r.kernel.multipleOfWidth = true;
    raw::AirHash air;
    air.fill(0x30);
    const std::string state = raw::computeStateKey(air, "#0:33:05000000", 256, true);
    r.key = raw::factsCacheKey(kStack, state).key;
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(back.kind, raw::RequestKind::Facts);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);
    EXPECT_EQ(p.key.key, "facts | " + state);

    raw::CompileRequest kernel = r;
    kernel.kind = raw::RequestKind::Kernel;
    EXPECT_NE(raw::storeName(p.key, kernel, identities(kernel)), p.name);
    raw::CompileRequest wider = r;
    wider.kernel.maxThreads = 512;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(wider), kStack, system, p, why));
    raw::CompileRequest two = r;
    two.stages.push_back(stage("k", "kernel_fn"));
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(two), back)) << "a facts request names one kernel";
}

// A kernel's linked functions (MPSGraph's kernels link their DAG library's privately) are its request's stages after the
// kernel, "kl" visible then "kp" private, and its key names each by AIR and constants past the kernel's own constants
// (raw_compute_key.h computeLinkedText), for its facts, its dispatches and its reflection alike. The same kernel without
// them is another state; a linked tag cannot stand first.
TEST(CompileRequest, AKernelsLinkedFunctionsAreItsStagesAndPartOfItsKey)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Facts;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.libraries.push_back(data(made_metallib::universal({"visible_fn", "prefixPrimary_i8"}, 0x40)));
    r.stages = {stage("k", "kernel_fn"), stage("kl", "visible_fn", 1, "#1:29:01"), stage("kp", "prefixPrimary_i8", 1)};
    r.kernel.maxThreads = 256;
    raw::AirHash kernelAir, visibleAir, privateAir;
    kernelAir.fill(0x30);
    visibleAir.fill(0x40);
    privateAir.fill(0x41);
    const std::string linked = raw::computeLinkedText({{false, visibleAir, "#1:29:01"}, {true, privateAir, ""}});
    EXPECT_EQ(raw::airText(privateAir).size(), 64u);
    EXPECT_EQ(raw::airText(privateAir).substr(0, 6), "414141");
    EXPECT_EQ(linked, " | kl " + raw::airText(visibleAir) + " #1:29:01 | kp " + raw::airText(privateAir));
    const std::string state = raw::computeStateKey(kernelAir, linked, 256, false);
    r.key = raw::factsCacheKey(kStack, state).key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);

    raw::CompileRequest alone = r;
    alone.stages.resize(1);
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(alone), kStack, system, p, why))
        << "the kernel alone claiming the linked state's key";
    alone.key = raw::factsCacheKey(kStack, raw::computeStateKey(kernelAir, "", 256, false)).key;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(alone), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey.find(" | kp "), std::string::npos);

    raw::CompileRequest kernel = r;
    kernel.kind = raw::RequestKind::Kernel;
    kernel.kernel.threads[0] = 64;
    kernel.kernel.threads[1] = kernel.kernel.threads[2] = 1;
    const std::string compute = raw::computeKey(kernelAir, linked, 256, false, kernel.kernel.threads, "", "", "");
    EXPECT_EQ(compute.rfind(state, 0), 0u);
    kernel.key = "kernel | " + compute;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(kernel), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, compute);

    raw::CompileRequest reflection = r;
    reflection.kind = raw::RequestKind::Reflection;
    reflection.key = raw::computeReflectionKey(kStack, state).key;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(reflection), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);

    raw::CompileRequest first = r;
    first.stages[0].tag = "kp";
    raw::CompileRequest back;
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(first), back));
    raw::CompileRequest render = r;
    render.stages[1].tag = "vl";
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(render), back)) << "a render stage's tag on a kernel";
}

// A stage-input kernel's facts: its stage input travels beside its constants and keys the state after them, as the pack
// keys it; a request whose stage input is not its key's is refused
TEST(CompileRequest, AStageInputKernelsFactsAreKeyedWithItsStageInput)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Facts;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn")};
    r.kernel.stageIn.attributes[0] = {29, 0, 1, 8, 1, 1};
    raw::AirHash air;
    air.fill(0x30);
    const std::string state = raw::computeStateKey(air, raw::stageInText(r.kernel.stageIn), 0, false);
    ASSERT_NE(state.find(" | in a0="), std::string::npos);
    r.key = raw::factsCacheKey(kStack, state).key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);
    raw::CompileRequest other = r;
    other.kernel.stageIn.attributes[0].offset = 4;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(other), kStack, system, p, why));
}

TEST(CompileRequest, BinaryFunctionKeysKeepTheirDefinitionAndSpecializedLinkName)
{
    raw::CompileRequest request;
    request.stack = kStack;
    request.kind = raw::RequestKind::Facts;
    request.libraries = {data(made_metallib::universal({"kernel_fn"}, 0x30, 0xee, {2})),
                         data(made_metallib::universal({"helper"}, 0x40, 0xee, {4}))};
    request.stages = {stage("k", "kernel_fn"), stage("kb", "helper", 1)};
    request.stages[1].name = "transform";
    raw::AirHash kernel;
    kernel.fill(0x30);
    raw::LinkedComputeFunction linked;
    linked.air.fill(0x40);
    linked.binary = true;
    linked.symbol = "helper";
    linked.name = "transform";
    request.key =
        raw::factsCacheKey(kStack, raw::computeStateKey(kernel, raw::computeLinkedText({linked}), 0, false)).key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared prepared;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(request), kStack, system, prepared, why)) << why;
    raw::CompileRequest renamed = request;
    renamed.stages[1].name = "another_transform";
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(renamed), kStack, system, prepared, why));
    raw::CompileRequest ordinary = request;
    ordinary.stages[1].tag = "kl";
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(ordinary), kStack, system, prepared, why));
}

TEST(CompileRequest, GroupMembershipAndOrderAreKeptInEveryComputeRequest)
{
    raw::CompileRequest request;
    request.stack = kStack;
    request.kind = raw::RequestKind::Facts;
    request.libraries = {data(made_metallib::universal({"kernel_fn"}, 0x30)),
                         data(made_metallib::universal({"helper", "other"}, 0x40))};
    request.stages = {stage("k", "kernel_fn"), stage("kg", "helper", 1, "#1:29:01"), stage("kg", "other", 1)};
    request.stages[1].name = "transform";
    request.stages[1].group = request.stages[2].group = std::string("choice\0other", 12) + " | kg :";
    raw::AirHash kernel;
    kernel.fill(0x30);
    raw::LinkedComputeFunction first, second;
    first.air.fill(0x40);
    first.symbol = "helper";
    first.name = "transform";
    first.constants = "#1:29:01";
    first.group = request.stages[1].group;
    second.air.fill(0x41);
    second.symbol = second.name = "other";
    second.group = first.group;
    const std::string state = raw::computeStateKey(kernel, raw::computeLinkedText({first, second}), 0, false);
    raw::LinkedComputeFunction prefix = first;
    prefix.group = "choice";
    EXPECT_NE(raw::computeLinkedText({prefix, second}), raw::computeLinkedText({first, second}));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared prepared;
    std::string why;
    for (const auto kind : {raw::RequestKind::Facts, raw::RequestKind::Reflection, raw::RequestKind::Kernel}) {
        request.kind = kind;
        request.key = kind == raw::RequestKind::Facts ? raw::factsCacheKey(kStack, state).key
                      : kind == raw::RequestKind::Reflection
                          ? raw::computeReflectionKey(kStack, state).key
                          : "kernel | " + state + " | tg=0,0,0 | tgmem= | tex= | smp=";
        raw::CompileRequest back;
        ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(request), back));
        EXPECT_EQ(back.stages[1].group, request.stages[1].group);
        EXPECT_EQ(back.stages[1].name, "transform");
        ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(back), kStack, system, prepared, why)) << why;
        const raw::Digest original = prepared.name;
        for (const unsigned mutation : {0u, 1u, 2u, 3u}) {
            raw::CompileRequest changed = request;
            if (mutation == 0)
                changed.stages[1].group += "x";
            else if (mutation == 1)
                std::swap(changed.stages[1], changed.stages[2]);
            else if (mutation == 2)
                changed.stages[1].constants = "#1:29:00";
            else {
                changed.stages[1].tag = "kl";
                changed.stages[1].group.clear();
            }
            EXPECT_NE(raw::storeName(prepared.key, changed, identities(changed)), original);
            EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(changed), kStack, system, prepared, why));
        }
    }
    raw::CompileRequest misplaced = request, back;
    misplaced.stages[0].group = "choice";
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(misplaced), back));
}

// A specialized function's attributes: one stage tagged "a", keyed by its AIR, constants and the name it was made under,
// refused with any other stage or a key naming other constants; the blob carries both lists, nil apart from empty
TEST(CompileRequest, AFunctionsAttributesAreKeyedByItsAirConstantsAndName)
{
    raw::CompileRequest r;
    r.stack = kStack;
    r.kind = raw::RequestKind::Reflection;
    r.libraries.push_back(data(made_metallib::universal({"vertex_fn"}, 0x30)));
    r.stages = {stage("a", "vertex_fn", 0, "#4:53:00")};
    raw::AirHash air;
    air.fill(0x30);
    const std::string function = raw::functionAttributesKey(air, "#4:53:00", "vertex_fn");
    r.key = raw::functionAttributesCacheKey(kStack, function).key;
    EXPECT_TRUE(raw::functionAttributes(r));
    EXPECT_FALSE(raw::computeReflection(r));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, "attributes | " + function);
    raw::CompileRequest other = r;
    other.stages[0].constants = "#4:53:01";
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(other), kStack, system, p, why));
    raw::CompileRequest two = r;
    two.stages.push_back(stage("a", "vertex_fn"));
    raw::CompileRequest back;
    EXPECT_FALSE(raw::decodeRequest(raw::encodeRequest(two), back)) << "an attributes request names one function";

    raw::FunctionAttributeTag position, normal;
    position.name = "position";
    position.type = 6;
    position.active = true;
    normal.name = "normal";
    normal.index = 5;
    normal.type = 5;
    normal.patchData = normal.controlPointData = true;
    const raw::AttributeList vertex = std::vector<raw::FunctionAttributeTag>{position, normal};
    raw::AttributeList readVertex, readStage = std::vector<raw::FunctionAttributeTag>{};
    ASSERT_TRUE(raw::decodeAttributeLists(raw::encodeAttributeLists(vertex, std::nullopt), readVertex, readStage));
    ASSERT_TRUE(readVertex.has_value());
    EXPECT_FALSE(readStage.has_value()) << "nil, not empty";
    ASSERT_EQ(readVertex->size(), 2u);
    EXPECT_EQ((*readVertex)[1].name, "normal");
    EXPECT_EQ((*readVertex)[1].index, 5);
    EXPECT_TRUE((*readVertex)[1].patchData && (*readVertex)[1].controlPointData && !(*readVertex)[1].active);
    EXPECT_FALSE(
        raw::decodeAttributeLists(raw::encodeAttributeLists(vertex, std::nullopt) + "x", readVertex, readStage));
}

TEST(CompileRequest, TheCompilerRefusesWhatItCannotTrust)
{
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    raw::CompileRequest r = renderRequest();
    r.key = runtimeKey(r).key;

    raw::CompileRequest claimed = r;
    claimed.libraries[0].system = true;
    claimed.libraries[0].path = "/System/Library/Frameworks/RenderBox.framework/default.metallib";
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(claimed), kStack, system, p, why));

    raw::CompileRequest leaving = r;
    leaving.libraries[0] = systemLibrary("/System/Library/../../private/var/mobile/x.metallib");
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(leaving), kStack, system, p, why));

    raw::CompileRequest empty = r;
    empty.libraries[0].bytes.clear();
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(empty), kStack, system, p, why));

    raw::CompileRequest nowhere = r;
    nowhere.stages[1].library = 3;
    EXPECT_FALSE(switch_compiler::prepare(raw::encodeRequest(nowhere), kStack, system, p, why));

    std::string cut = raw::encodeRequest(r);
    cut.pop_back();
    EXPECT_FALSE(switch_compiler::prepare(cut, kStack, system, p, why));

    std::string real;
    EXPECT_FALSE(switch_compiler::SystemLibraries::resolve("", "/System/Library/../../etc/x.metallib", real, why));
}

TEST(CompileRequest, ConstantsReadBackAsTheirText)
{
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    const uint32_t word = 0x01020304;
    const float half4[4] = {1, 2, 3, 4};
    [values setConstantValue:&word type:MTLDataTypeUInt atIndex:3];
    [values setConstantValue:half4 type:MTLDataTypeFloat4 withName:@"tint"];
    const std::string text = raw::constantsText(values);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(raw::constantsText(raw::constantValuesOf(text)), text);
    EXPECT_EQ(raw::constantValuesOf("#0:9999:00"), nil);
}

}  // namespace
