// SPDX-License-Identifier: MIT
// A compile request (raw/runtime/raw_compile_request.h) as the client makes it and the compiler takes it
// (compiler/compiler_request.h): it round-trips, its fields key a descriptor as the descriptor itself keys, the compiler
// derives the runtime's own key from it and refuses one whose claimed key differs, and the store name binds what the
// key does not (a library's bytes, rasterization, a kernel's dispatch).
#include "../raw/runtime/RawCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../raw/runtime/raw_compute_key.h"
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
    r.cubic.samplers.push_back({2, 1, 1, 0, 3, true});
    r.cubic.textures.push_back({2, 80});
    r.fragmentTests = 3;
    r.key = "a key";
    raw::CompileRequest back;
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(r), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(r));

    raw::CompileRequest k;
    k.stack = kStack;
    k.kind = raw::RequestKind::Kernel;
    k.key = "kernel | a key";
    k.libraries.push_back(data("bytes"));
    k.stages = {stage("k", "kernel_fn")};
    k.kernel.maxThreads = 256;
    k.kernel.threads[0] = 64;
    k.kernel.memory[2] = 1024;
    k.kernel.textures.push_back({1, 80, 2, 3, 1, 1, 1});
    NmbDirectBoundSampler sampler = {};
    sampler.slot = 4;
    sampler.descriptor.max_anisotropy = 8;
    k.kernel.samplers.push_back(sampler);
    ASSERT_TRUE(raw::decodeRequest(raw::encodeRequest(k), back));
    EXPECT_EQ(raw::encodeRequest(back), raw::encodeRequest(k));
    EXPECT_EQ(back.kernel.dispatch().memory[2], 1024u);
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
    r.cubic.samplers.push_back({0, 2, 2, 2, 0, true});
    r.fragmentTests = 2;
    EXPECT_EQ(raw::requestPackKey(r, own),
              raw::pipelineKey(descriptor(), &v, &f, "", "RenderBox") + r.cubic.key() + " | tests=2");
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
    r.kernel.textures.push_back({0, 80, 2, 0x17, 1, 1, 1});
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
