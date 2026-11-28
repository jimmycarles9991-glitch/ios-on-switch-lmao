// SPDX-License-Identifier: MIT
// A compile request (compile_request.h) as the client makes it and the compiler takes it
// (compiler/compiler_request.h): it round-trips, its fields key a descriptor as the descriptor itself keys, the compiler
// derives the runtime's own key from it and refuses one whose claimed key differs, and the store name binds what the
// key does not (a library's bytes, rasterization, a kernel's dispatch).
#include "../TegraGM20BMetalCompileRequest.mm"
#include "../compiler/compiler_request.mm"
#include "../compute_key.h"
#include "compiler_request_layout03.h"
#include "../function_attributes.h"
#include "made_metallib.h"
#include "request_store_key.h"

#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>

namespace {

constexpr uint64_t kStack = 0x5357495443480003ull;

gm20b::RequestStage stage(const char *tag, const char *function, uint32_t library = 0, const char *constants = "")
{
    gm20b::RequestStage s;
    s.tag = tag;
    s.function = function;
    s.name = function;
    s.constants = constants;
    s.specialized = constants[0] != 0;
    s.library = library;
    return s;
}

gm20b::RequestLibrary data(const std::string &bytes)
{
    gm20b::RequestLibrary l;
    l.bytes = bytes;
    return l;
}

gm20b::RequestLibrary systemLibrary(const char *path)
{
    gm20b::RequestLibrary l;
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
gm20b::CompileRequest renderRequest()
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.fields = gm20b::fieldsOf(descriptor());
    r.libraries.push_back(data(made_metallib::universal({"vertex_fn", "fragment_fn"})));
    r.stages = {stage("v", "vertex_fn"), stage("f", "fragment_fn", 0, "#0:33:05000000")};
    return r;
}

// The CacheKey the runtime makes for `r`'s state (TegraGM20BMetalComplete.mm keyOf over TegraGM20BMetalRenderKey.mm)
gm20b::CacheKey runtimeKey(const gm20b::CompileRequest &r)
{
    bool own = false;
    const std::string key = gm20b::requestPackKey(r, own);
    gm20b::AirHash vertex, fragment;
    vertex.fill(0xA0);
    fragment.fill(0xA1);
    return gm20b::renderCacheKey(r.stack, key, vertex, fragment, {}, own);
}

// The store name the compiler gives `r` of CacheKey `k`
gm20b::Digest nameOf(const gm20b::CacheKey &k, const gm20b::CompileRequest &r)
{
    return harness::requestStoreKey(k, r).name;
}

TEST(CompileRequest, RoundTripsWhole)
{
    gm20b::CompileRequest r = renderRequest();
    r.stages.push_back(stage("fl", "fragment_fn", 0));
    r.variant.cubic.samplers.push_back({2, 1, 1, 0, 3, true});
    r.variant.cubic.textures.push_back({2, 80});
    r.variant.fragmentTests = 3;
    r.key = "a key";
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(r), back));
    EXPECT_EQ(gm20b::encodeRequest(back), gm20b::encodeRequest(r));
    EXPECT_EQ(back.variant.fragmentTests, 3);

    gm20b::CompileRequest k;
    k.stack = kStack;
    k.kind = gm20b::RequestKind::Kernel;
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
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(k), back));
    EXPECT_EQ(gm20b::encodeRequest(back), gm20b::encodeRequest(k));
    EXPECT_EQ(back.kernel.dispatch().memory[2], 1024u);
}

// A vertex stage's cubic binds keep their stage through the request (the sampler's flag byte's bit 1, the texture's
// slot byte's top bit) and name their variant "vs" and "vt"; a fragment-only request's bytes and variant name are as
// packed
TEST(CompileRequest, AVertexCubicBindKeepsItsStage)
{
    gm20b::CompileRequest fragment = renderRequest();
    fragment.key = "a key";
    fragment.variant.cubic.samplers.push_back({0, 0, 0, 0, 0, true});
    fragment.variant.cubic.textures.push_back({0, 80});
    EXPECT_EQ(fragment.variant.cubic.key(), " | cubic s0:0000n t0:80");
    gm20b::CompileRequest both = fragment;
    both.variant.cubic.samplers.push_back({3, 2, 2, 0, 1, false, gm20b::kCubicVertex});
    both.variant.cubic.textures.push_back({5, 70, gm20b::kCubicVertex});
    EXPECT_EQ(both.variant.cubic.key(), " | cubic s0:0000n vs3:2201p t0:80 vt5:70");
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(both), back));
    const gm20b::CubicBinds &cubic = back.variant.cubic;
    ASSERT_EQ(cubic.samplers.size(), 2u);
    ASSERT_EQ(cubic.textures.size(), 2u);
    EXPECT_EQ(cubic.samplers[0].stage, gm20b::kCubicFragment);
    EXPECT_TRUE(cubic.samplers[0].normalized);
    EXPECT_EQ(cubic.samplers[1].stage, gm20b::kCubicVertex);
    EXPECT_EQ(cubic.samplers[1].slot, 3u);
    EXPECT_FALSE(cubic.samplers[1].normalized);
    EXPECT_EQ(cubic.textures[1].stage, gm20b::kCubicVertex);
    EXPECT_EQ(cubic.textures[1].slot, 5u);
    EXPECT_EQ(cubic.key(), both.variant.cubic.key());
    const std::string bytes = gm20b::encodeRequest(fragment);
    const std::string flags = std::string("\0\0\0\0\0\1", 6);
    EXPECT_NE(bytes.find(flags), std::string::npos) << "a fragment sampler's six bytes, its flags only normalized";
}

// A render request with every variant form layout 03 carried that later layouts keep: a fragment cubic bind, a test
// class, sampled two-plane slots and a written texture
gm20b::CompileRequest layout03Request()
{
    gm20b::CompileRequest r = renderRequest();
    r.key = "a key";
    r.variant.cubic.samplers.push_back({9, 2, 1, 2, 3, true});
    r.variant.cubic.textures.push_back({9, 80});
    r.variant.fragmentTests = 2;
    r.variant.xrSlots = 4;
    r.variant.written = {{2, 23}};
    return r;
}

TEST(CompileRequest, ItsLayoutIsSWCREQ08)
{
    EXPECT_EQ(gm20b::encodeRequest(layout03Request()).substr(0, 8), "SWCREQ08");
}

// Layouts 04-07 do not carry RequestLibrary::libraryId/digest (added in SWCREQ08)
TEST(CompileRequest, ARequestOfAnOlderLayoutIsRefused)
{
    for (const char *magic : {"SWCREQ04", "SWCREQ05", "SWCREQ06", "SWCREQ07"}) {
        std::string bytes = layout03::encode(layout03Request(), true);
        bytes.replace(0, 8, magic);
        gm20b::CompileRequest back;
        EXPECT_FALSE(gm20b::decodeRequest(bytes, back)) << magic;
        switch_compiler::SystemLibraries system;
        switch_compiler::Prepared prepared;
        std::string why;
        EXPECT_FALSE(switch_compiler::prepare(bytes, kStack, system, prepared, why)) << magic;
        EXPECT_EQ(why, "an unreadable request") << magic;
    }
}

TEST(CompileRequest, AlphaToCoverageRoundTripsAndKeysSeparately)
{
    MTLRenderPipelineDescriptor *d = descriptor();
    d.alphaToCoverageEnabled = YES;
    gm20b::CompileRequest r = renderRequest();
    r.key = "a key";
    r.fields = gm20b::fieldsOf(d);
    EXPECT_EQ(r.fields.alphaToCoverage, 1u);
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(r), back));
    EXPECT_EQ(back.fields.alphaToCoverage, 1u);
    MTLRenderPipelineDescriptor *made = [MTLRenderPipelineDescriptor new];
    gm20b::applyFields(back.fields, made);
    EXPECT_TRUE(made.alphaToCoverageEnabled);
    bool own = false;
    const std::string flaggedKey = gm20b::requestPackKey(back, own);
    const std::string plainKey = gm20b::requestPackKey(renderRequest(), own);
    EXPECT_NE(flaggedKey.find(" alphacov=1 "), std::string::npos) << flaggedKey;
    EXPECT_EQ(plainKey.find("alphacov="), std::string::npos) << plainKey;
    EXPECT_NE(flaggedKey, plainKey);
}

// A request of layout 03, which a client of an older build sends or the compiler's request log (compiler_log.h) kept,
// is refused whole by the reader and by prepare, which the log's replay runs
TEST(CompileRequest, ARequestOfLayout03IsRefused)
{
    const gm20b::CompileRequest r = layout03Request();
    const std::string bytes = layout03::encode(r);
    gm20b::CompileRequest back;
    EXPECT_FALSE(gm20b::decodeRequest(bytes, back));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(bytes, kStack, system, p, why));
    EXPECT_EQ(why, "an unreadable request");

    gm20b::CompileRequest merged = r;
    merged.variant.cubic.samplers.push_back({3, 2, 2, 0, 1, false, gm20b::kCubicVertex});
    merged.variant.cubic.textures.push_back({5, 70, gm20b::kCubicVertex});
    EXPECT_FALSE(gm20b::decodeRequest(layout03::encode(merged, true), back));
}

TEST(CompileRequest, VertexCubicBindsRoundTrip)
{
    gm20b::CompileRequest r = layout03Request();
    r.variant.cubic.samplers.push_back({3, 2, 2, 0, 1, false, gm20b::kCubicVertex});
    r.variant.cubic.textures.push_back({5, 70, gm20b::kCubicVertex});
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(r), back));
    EXPECT_EQ(gm20b::encodeRequest(back), gm20b::encodeRequest(r));
    EXPECT_EQ(back.variant.name(), r.variant.name());
    ASSERT_EQ(back.variant.cubic.samplers.size(), 2u);
    ASSERT_EQ(back.variant.cubic.textures.size(), 2u);
    EXPECT_EQ(back.variant.cubic.samplers[1].stage, gm20b::kCubicVertex);
    EXPECT_EQ(back.variant.cubic.textures[1].stage, gm20b::kCubicVertex);
    EXPECT_EQ(back.variant.cubic.textures[1].slot, 5u);
    EXPECT_EQ(back.variant.fragmentTests, 2);
    EXPECT_EQ(back.variant.xrSlots, 4);
}

// A cubic sampler's flag byte holds normalized (bit 0) and the vertex stage (bit 1), nothing else
TEST(CompileRequest, ASamplerFlagThisLayoutDoesNotDefineIsRefused)
{
    std::string bytes = gm20b::encodeRequest(layout03Request());
    const size_t at = bytes.find(std::string("\x09\x02\x01\x02\x03\x01", 6));
    ASSERT_NE(at, std::string::npos);
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(bytes, back));
    bytes[at + 5] = 0x05;
    EXPECT_FALSE(gm20b::decodeRequest(bytes, back));
}

TEST(CompileRequest, ItsFieldsKeyADescriptorAsTheDescriptorKeysItself)
{
    MTLRenderPipelineDescriptor *d = descriptor(), *made = [MTLRenderPipelineDescriptor new];
    const gm20b::PipelineFields fields = gm20b::fieldsOf(d);
    gm20b::applyFields(fields, made);
    const gm20b::StageKey v{"vertex_fn", ""}, f{"fragment_fn", "#0:33:05000000"};
    EXPECT_EQ(gm20b::pipelineKey(made, &v, &f, "", ""), gm20b::pipelineKey(d, &v, &f, "", ""));
    EXPECT_EQ(gm20b::fieldsOf(made).rasterization, 0u);
    EXPECT_FALSE(made.rasterizationEnabled);
}

TEST(CompileRequest, ItsPackKeyIsTheFacadesForFrameworkAndOwnLibraries)
{
    gm20b::CompileRequest r = renderRequest();
    r.libraries = {systemLibrary("/System/Library/Frameworks/RenderBox.framework/default.metallib")};
    bool own = true;
    const gm20b::StageKey v{"vertex_fn", ""}, f{"fragment_fn", "#0:33:05000000"};
    EXPECT_EQ(gm20b::requestPackKey(r, own), gm20b::pipelineKey(descriptor(), &v, &f, "", "RenderBox"));
    EXPECT_FALSE(own);
    r.variant.cubic.samplers.push_back({0, 2, 2, 2, 0, true});
    r.variant.fragmentTests = 2;
    EXPECT_EQ(gm20b::requestPackKey(r, own),
              gm20b::pipelineKey(descriptor(), &v, &f, "", "RenderBox") + r.variant.cubic.key() + " | tests=2");
    gm20b::CompileRequest data = renderRequest();
    EXPECT_EQ(gm20b::requestPackKey(data, own), gm20b::pipelineKey(descriptor(), &v, &f, "", ""));
    EXPECT_TRUE(own);
}

TEST(CompileRequest, TheCompilerKeysItAsTheRuntimeDoes)
{
    gm20b::CompileRequest r = renderRequest();
    const gm20b::CacheKey k = runtimeKey(r);
    r.key = k.key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, k.key);
    EXPECT_EQ(memcmp(p.key.vertexAir, k.vertexAir, 32), 0);
    EXPECT_EQ(memcmp(p.key.fragmentAir, k.fragmentAir, 32), 0);
    EXPECT_EQ(p.name, nameOf(k, r));
}

TEST(CompileRequest, OneWhoseClaimedKeyDiffersIsRefused)
{
    gm20b::CompileRequest r = renderRequest();
    r.key = runtimeKey(r).key + " ";
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why));
    EXPECT_NE(why.find("another key"), std::string::npos) << why;
    r.key = runtimeKey(r).key;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack + 1, system, p, why));
}

// The compiler's replay of its request log (compiler_service.mm warm) after a build whose stack key moved
TEST(CompileRequest, ARequestAnEarlierBuildLoggedPreparesUnderThisBuildsKeyAndName)
{
    gm20b::CompileRequest r = renderRequest();
    r.key = runtimeKey(r).key;
    gm20b::CompileRequest earlier = r;
    earlier.stack = kStack + 1;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(switch_compiler::restacked(gm20b::encodeRequest(earlier), kStack), kStack,
                                         system, p, why))
        << why;
    EXPECT_EQ(p.key.stack, kStack);
    EXPECT_EQ(p.name, nameOf(runtimeKey(r), r));
}

// The replay restamps a logged request's stack, never its layout: a request the build before the extended-range
// stand-in logged (SWCREQ05, with the shadowed and two-plane colours' bytes) is unreadable to restacked as to prepare,
// so the replay drops it whole (its line's dropped count) and makes nothing of fields read in this layout
TEST(CompileRequest, ALoggedRequestOfLayout05IsDroppedByTheReplay)
{
    gm20b::CompileRequest r = layout03Request();
    r.stack = kStack + 1;
    std::string logged = layout03::encode(r, true);
    logged.replace(0, 8, "SWCREQ05");
    EXPECT_EQ(switch_compiler::restacked(logged, kStack), logged);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(switch_compiler::restacked(logged, kStack), kStack, system, p, why));
    EXPECT_EQ(why, "an unreadable request");
    EXPECT_EQ(switch_compiler::restacked("no request", kStack), "no request");
}

// The compiler's preparation of render request `r`, keyed as the runtime keys it, for an asker of `scope`
switch_compiler::Prepared prepared(gm20b::CompileRequest r, switch_compiler::SystemLibraries &system,
                                   const gm20b::StoreScope &scope = {})
{
    r.key = runtimeKey(r).key;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why, 0, nullptr, scope)) << why;
    return p;
}

const gm20b::StoreScope kMaps = {1, "com.apple.Maps"}, kOtherApp = {2, std::string(20, char(0x5a))};

// A system volume under a test directory holding `bytes` as RenderBox's default library
std::string systemRoot(const std::string &bytes)
{
    char made[] = "/tmp/store-key-XXXXXX";
    char real[PATH_MAX];
    const std::string root = realpath(mkdtemp(made), real);
    const std::string dir = root + "/System/Library/Frameworks/RenderBox.framework";
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/default.metallib", std::ios::binary) << bytes;
    return root;
}

// A framework's shader is one store entry for every process: the compiler names it and heads its entry by the functions'
// AIR, facts and constants and the state's fields, whoever asks. A shader of an app's own library is its app's: one entry
// for the app whatever bytes surround it, and none another app reaches, as a library off the system volume could carry
// any tags. An entry one process made is another's only where their contents are alike byte for byte.
TEST(CompileRequest, AFrameworksShaderIsEveryProcesssAndAnAppsShaderItsOwn)
{
    const gm20b::CompileRequest r = renderRequest();
    gm20b::CompileRequest framework = r;
    framework.libraries[0] = systemLibrary("/System/Library/Frameworks/RenderBox.framework/default.metallib");
    switch_compiler::SystemLibraries none, volume(systemRoot(r.libraries[0].bytes));
    const switch_compiler::Prepared forMaps = prepared(framework, volume, kMaps),
                                    forOther = prepared(framework, volume, kOtherApp);
    EXPECT_EQ(forOther.name, forMaps.name);
    EXPECT_EQ(forOther.content.key, forMaps.content.key);

    gm20b::CompileRequest moved = r;
    moved.libraries[0] = data(made_metallib::universal({"helper_fn", "vertex_fn", "fragment_fn"}, 0x9F, 0x11));
    const switch_compiler::Prepared own = prepared(r, none, kMaps), ownMoved = prepared(moved, none, kMaps),
                                    other = prepared(r, none, kOtherApp);
    EXPECT_NE(own.request.libraries[0].bytes, ownMoved.request.libraries[0].bytes);
    EXPECT_EQ(ownMoved.name, own.name);
    EXPECT_EQ(ownMoved.content.key, own.content.key);
    EXPECT_NE(other.name, own.name);
    EXPECT_NE(own.name, forMaps.name);

    gm20b::RuntimeRecord made, taken;
    made.keyString = "samples=4";
    std::string why;
    const std::string entry = gm20b::encodeEntry(own.content, &made, "");
    ASSERT_EQ(gm20b::decodeEntry(entry, ownMoved.content, taken, why), gm20b::RecordCache::Found::Record);
    EXPECT_EQ(gm20b::decodeEntry(entry, other.content, taken, why), gm20b::RecordCache::Found::None);
    const std::string shared = gm20b::encodeEntry(forMaps.content, &made, "");
    ASSERT_EQ(gm20b::decodeEntry(shared, forOther.content, taken, why), gm20b::RecordCache::Found::Record);
}

// What a library says of a function outside its bitcode (its public metadata: the vertex attributes, their types, the
// constants it declares) is in the key beside the AIR: one bitcode paired with other attributes is another entry
TEST(CompileRequest, AFunctionsPublicMetadataIsInItsKey)
{
    const auto attributes = [](uint8_t index) {
        std::vector<uint8_t> v;
        const uint8_t vatt[] = {1, 0, 'p', 0, index, 0x80};
        made_metallib::tag(v, "VATT", vatt, sizeof vatt);
        return v;
    };
    const auto library = [&](uint8_t index) {
        return data(made_metallib::thin({"vertex_fn", "fragment_fn"}, {0, 1},
                                        {made_metallib::wrappedBitcode(1), made_metallib::wrappedBitcode(2)},
                                        {attributes(index), {}}));
    };
    const auto keyed = [](gm20b::CompileRequest q) {
        gm20b::AirHash vertex, fragment;
        vertex.fill(0xB0);
        fragment.fill(0xB1);
        bool own = false;
        q.key = gm20b::renderCacheKey(q.stack, gm20b::requestPackKey(q, own), vertex, fragment, {}, own).key;
        return q;
    };
    gm20b::CompileRequest a = renderRequest(), b = a, c = a;
    a.libraries[0] = library(0);
    b.libraries[0] = library(3);
    c.libraries[0] = library(0);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared pa, pb, pc;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(keyed(a)), kStack, system, pa, why, 0, nullptr, kMaps))
        << why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(keyed(b)), kStack, system, pb, why, 0, nullptr, kMaps))
        << why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(keyed(c)), kStack, system, pc, why, 0, nullptr, kMaps))
        << why;
    EXPECT_EQ(memcmp(pa.key.vertexAir, pb.key.vertexAir, 32), 0);
    EXPECT_NE(pb.name, pa.name);
    EXPECT_NE(pb.content.key, pa.content.key);
    EXPECT_EQ(pc.name, pa.name);
}

// Two requests share an entry only where everything that reaches the record is alike: one constant, one descriptor
// field, the variant, a function's AIR or a linked function's name apart gives another name, and the entry one made is
// no answer to the other (its head is compared whole)
TEST(CompileRequest, OneInputApartIsAnotherStoreEntry)
{
    switch_compiler::SystemLibraries system;
    const gm20b::CompileRequest r = renderRequest();
    const switch_compiler::Prepared base = prepared(r, system);
    std::vector<gm20b::CompileRequest> apart(6, r);
    apart[0].stages[1].constants = "#0:33:06000000";
    apart[1].fields.color[0].destinationRGB = MTLBlendFactorOne;
    apart[2].fields.rasterization = 1;
    apart[3].variant.fragmentTests = 1;
    apart[4].libraries[0] = data(made_metallib::universal({"vertex_fn", "fragment_fn"}, 0xB0));
    apart[5].libraries[0] = data(made_metallib::universal({"vertex_fn", "fragment_fn", "linked_fn"}));
    apart[5].stages.push_back(stage("fl", "linked_fn"));
    gm20b::CompileRequest renamed = apart[5];
    renamed.stages[2].name = "linked_as";
    apart.push_back(renamed);
    gm20b::RuntimeRecord record;
    const std::string entry = gm20b::encodeEntry(base.content, &record, "");
    std::vector<gm20b::Digest> names = {base.name};
    for (size_t i = 0; i < apart.size(); ++i) {
        gm20b::CompileRequest q = apart[i];
        if (i >= 4) {
            std::map<std::string, gm20b::AirHash> hashes;
            ASSERT_TRUE(gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(q.libraries[0].bytes.data()),
                                                 q.libraries[0].bytes.size(), hashes));
            std::vector<gm20b::LinkedStageAir> linked;
            for (size_t s = 2; s < q.stages.size(); ++s)
                linked.push_back({'f', hashes[q.stages[s].function]});
            bool own = false;
            q.key = gm20b::renderCacheKey(q.stack, gm20b::requestPackKey(q, own), hashes["vertex_fn"],
                                          hashes["fragment_fn"], linked, own)
                        .key;
        } else {
            q.key = runtimeKey(q).key;
        }
        switch_compiler::Prepared p;
        std::string why;
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(q), kStack, system, p, why)) << i << ": " << why;
        for (const gm20b::Digest &seen : names)
            EXPECT_NE(p.name, seen) << i;
        names.push_back(p.name);
        gm20b::RuntimeRecord taken;
        EXPECT_EQ(gm20b::decodeEntry(entry, p.content, taken, why), gm20b::RecordCache::Found::None) << i;
    }
}

// Core Image's, CoreUI's and Vision's libraries state one HASH for many functions: a stage's function is in its key
TEST(CompileRequest, TwoFunctionsOfOneStatedHashAreTwoEntries)
{
    switch_compiler::SystemLibraries system;
    std::string bytes = made_metallib::universal({"vertex_fn", "fragment_fn", "fragment_two"});
    const std::string own(32, char(0xA2)), shared(32, char(0xA1));
    const size_t at = bytes.find(own);
    ASSERT_NE(at, std::string::npos);
    bytes.replace(at, own.size(), shared);
    const std::string &library = bytes;
    gm20b::CompileRequest one = renderRequest(), two = one;
    one.libraries[0] = two.libraries[0] = data(library);
    two.stages[1].function = "fragment_two";
    const switch_compiler::Prepared a = prepared(one, system), b = prepared(two, system);
    EXPECT_NE(a.name, b.name);
    gm20b::RuntimeRecord record, taken;
    std::string why;
    const std::string entry = gm20b::encodeEntry(a.content, &record, "");
    EXPECT_EQ(gm20b::decodeEntry(entry, b.content, taken, why), gm20b::RecordCache::Found::None);
}

// The compiler applies the fields as the key names them: two requests apart only in what the key does not name (an
// unused attachment's blend factors, rounding mode 1 against 0) are one content and give the compiler one descriptor,
// and a literal stride the key would name as the dynamic one is refused
TEST(CompileRequest, TheCompilerAppliesOnlyFieldsTheKeyNames)
{
    switch_compiler::SystemLibraries system;
    gm20b::CompileRequest r = renderRequest(), loose = r;
    loose.fields.color[5].sourceRGB = MTLBlendFactorOne;
    loose.fields.color[5].writeMask = MTLColorWriteMaskAll;
    loose.fields.textureWriteRounding = 1;
    const switch_compiler::Prepared a = prepared(r, system), b = prepared(loose, system);
    EXPECT_EQ(b.content.key, a.content.key);
    EXPECT_EQ(memcmp(&b.request.fields, &a.request.fields, sizeof a.request.fields), 0);
    gm20b::CompileRequest literal = r;
    literal.fields.attributes[0].stride = UINT32_MAX;
    literal.key = runtimeKey(literal).key;
    switch_compiler::Prepared p;
    std::string why;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(literal), kStack, system, p, why));
    EXPECT_NE(why.find("stride"), std::string::npos) << why;
}

// A storage form's barrier twin is asked as its state's request in RequestForm::Barrier: keyed as the state, so the
// compiler checks it the same, and filed apart from the state's own record; no compute request takes the form
TEST(CompileRequest, ABarrierTwinIsItsStatesKeyInAStoreEntryOfItsOwn)
{
    switch_compiler::SystemLibraries system;
    gm20b::CompileRequest twin = renderRequest();
    twin.form = gm20b::RequestForm::Barrier;
    twin.key = runtimeKey(twin).key;
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(twin), back));
    EXPECT_EQ(back.form, gm20b::RequestForm::Barrier);
    const switch_compiler::Prepared state = prepared(renderRequest(), system), barrier = prepared(twin, system);
    EXPECT_EQ(barrier.key.key, state.key.key);
    EXPECT_NE(barrier.name, state.name);
    EXPECT_NE(barrier.content.key, state.content.key);

    gm20b::CompileRequest kernel;
    kernel.stack = kStack;
    kernel.kind = gm20b::RequestKind::Kernel;
    kernel.form = gm20b::RequestForm::Barrier;
    kernel.key = "kernel | k";
    kernel.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    kernel.stages = {stage("k", "kernel_fn")};
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(kernel), back));
    std::string bytes = gm20b::encodeRequest(twin);
    bytes[8 + 8 + 1] = uint8_t(gm20b::RequestForm::BarrierFull) + 1;
    EXPECT_FALSE(gm20b::decodeRequest(bytes, back));
}

// A full form (the state's, Full, or its twin's, BarrierFull) is an entry of its own, and is filed besides under the
// content its plain form keys to, which is what the state's or twin's own lookup reads
TEST(CompileRequest, AFullFormIsAnEntryOfItsOwnAndFiledUnderItsPlainFormsContent)
{
    switch_compiler::SystemLibraries system;
    const std::pair<gm20b::RequestForm, gm20b::RequestForm> forms[] = {
        {gm20b::RequestForm::Full, gm20b::RequestForm::Primary},
        {gm20b::RequestForm::BarrierFull, gm20b::RequestForm::Barrier}};
    for (const auto &[full, plain] : forms) {
        gm20b::CompileRequest r = renderRequest(), q = r;
        r.form = full;
        q.form = plain;
        gm20b::CompileRequest back;
        r.key = runtimeKey(r).key;
        ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(r), back));
        EXPECT_EQ(back.form, full);
        const switch_compiler::Prepared f = prepared(r, system), p = prepared(q, system);
        EXPECT_NE(f.content.key, p.content.key);
        EXPECT_EQ(f.primaryContent.key, p.content.key);
        EXPECT_EQ(f.primaryName, p.name);
        EXPECT_EQ(gm20b::withoutFull(full), plain);
        EXPECT_TRUE(gm20b::fullForm(full));
        EXPECT_FALSE(gm20b::fullForm(plain));
        EXPECT_TRUE(p.primaryContent.key.empty());
    }
}

// A state of no fragment function (RenderBox's path_edges) is asked by its vertex stage alone: the request round-trips, keys
// "f -" with no fragment AIR, as the facade keys it, and the compiler's descriptor of it has no fragment function
TEST(CompileRequest, AStateOfNoFragmentFunctionIsAskedByItsVertexStageAlone)
{
    gm20b::CompileRequest r = renderRequest();
    r.stages = {stage("v", "vertex_fn"), stage("vl", "fragment_fn")};
    EXPECT_EQ(gm20b::fragmentStage(r), gm20b::kNoStage);
    EXPECT_EQ(gm20b::firstLinked(r), 1u);
    bool own = false;
    const gm20b::StageKey v{"vertex_fn", ""};
    const std::string packKey = gm20b::requestPackKey(r, own);
    EXPECT_EQ(packKey, gm20b::pipelineKey(descriptor(), &v, nullptr, " | vl fragment_fn ", ""));
    EXPECT_NE(packKey.find(" | f -"), std::string::npos) << packKey;
    gm20b::AirHash vertex, none{}, linked;
    vertex.fill(0xA0);
    linked.fill(0xA1);
    const gm20b::CacheKey k = gm20b::renderCacheKey(kStack, packKey, vertex, none, {{'v', linked}}, own);
    r.key = k.key;
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(r), back));
    EXPECT_EQ(gm20b::encodeRequest(back), gm20b::encodeRequest(r));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, k.key);
    EXPECT_EQ(memcmp(p.key.fragmentAir, none.data(), 32), 0);

    gm20b::CompileRequest two = renderRequest();
    EXPECT_EQ(gm20b::fragmentStage(two), 1u);
    EXPECT_EQ(gm20b::firstLinked(two), 2u);
    gm20b::CompileRequest misplaced = renderRequest();
    misplaced.stages = {stage("v", "vertex_fn"), stage("vl", "vertex_fn"), stage("f", "fragment_fn")};
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(misplaced), back));
}

// A state Metal does not rasterize keys rast=0 after its attachments and vertex layout; one it does names nothing
TEST(CompileRequest, TheKeyNamesAStateMetalDoesNotRasterize)
{
    MTLRenderPipelineDescriptor *d = descriptor();
    const gm20b::StageKey v{"vertex_fn", ""};
    const std::string off = gm20b::pipelineKey(d, &v, nullptr, "", "");
    EXPECT_NE(off.find(" a3=9:8:3:20:2:2 rast=0  | v vertex_fn"), std::string::npos) << off;
    d.rasterizationEnabled = YES;
    EXPECT_EQ(gm20b::pipelineKey(d, &v, nullptr, "", "").find("rast="), std::string::npos);
}

// A Reflection asks a render state's reflection by the render request's stages and fields, keyed as the state is and named
// apart from its record; a compute state's by its kernel's one stage and limits, keyed by its computeStateKey alone
TEST(CompileRequest, AReflectionIsKeyedAsItsStateAndNamedApart)
{
    gm20b::CompileRequest render = renderRequest();
    const gm20b::CacheKey k = runtimeKey(render);
    render.key = k.key;
    gm20b::CompileRequest reflection = render;
    reflection.kind = gm20b::RequestKind::Reflection;
    EXPECT_FALSE(gm20b::computeReflection(reflection));
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(reflection), back));
    EXPECT_EQ(back.kind, gm20b::RequestKind::Reflection);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(reflection), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, k.key);
    EXPECT_NE(p.name, nameOf(k, render));

    gm20b::CompileRequest compute;
    compute.stack = kStack;
    compute.kind = gm20b::RequestKind::Reflection;
    compute.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    compute.stages = {stage("k", "kernel_fn")};
    compute.kernel.maxThreads = 512;
    gm20b::AirHash air;
    air.fill(0x30);
    const std::string state = gm20b::computeStateKey(air, "", 512, false);
    compute.key = gm20b::computeReflectionKey(kStack, state).key;
    EXPECT_TRUE(gm20b::computeReflection(compute));
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(compute), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);
    gm20b::CompileRequest limits = compute;
    limits.kernel.maxThreads = 256;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(limits), kStack, system, p, why));
    for (const gm20b::RequestForm form : {gm20b::RequestForm::Full, gm20b::RequestForm::BarrierFull}) {
        gm20b::CompileRequest full = compute;
        full.form = form;
        EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(full), back));
    }
}

TEST(CompileRequest, AKernelIsKeyedByItsFunctionAndNamedByItsDispatch)
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Kernel;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn")};
    r.kernel.maxThreads = 1024;
    r.kernel.threads[0] = 64;
    r.kernel.threads[1] = r.kernel.threads[2] = 1;
    r.kernel.textures.push_back({0, 0, 80, 2, 0x17, 1, 1, 1});
    gm20b::AirHash air;
    air.fill(0x30);
    const std::string compute =
        gm20b::computeKey(air, "", 1024, false, r.kernel.threads, "", "0:80:2:1:1:00010203", "");
    r.key = "kernel | " + compute;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, compute);
    EXPECT_EQ(p.key.key, gm20b::kernelCacheKey(kStack, compute).key);

    gm20b::CompileRequest used = r;
    used.kernel.textures[0].usage = 0x13;
    EXPECT_NE(nameOf(p.key, used), p.name);

    gm20b::CompileRequest region = r;
    region.kernel.regionMode = true;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(region), kStack, system, p, why));
    gm20b::CompileRequest other = r;
    other.libraries[0] = data(made_metallib::universal({"kernel_fn"}, 0x31));
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(other), kStack, system, p, why));
}

// A kernel's store name is its function's AIR, its key and its dispatch: the bytes of the library around the function do
// not move it, a texture's usage in the dispatch does
TEST(CompileRequest, AKernelsStoreNameIsItsFunctionNotItsLibrary)
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Kernel;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn")};
    r.kernel.maxThreads = 1024;
    r.kernel.threads[0] = 64;
    r.kernel.threads[1] = r.kernel.threads[2] = 1;
    const gm20b::CacheKey k = gm20b::kernelCacheKey(kStack, "a compute key");
    gm20b::CompileRequest moved = r;
    moved.libraries[0] = data(made_metallib::universal({"other_fn", "kernel_fn"}, 0x2F, 0x11));
    moved.stages[0].library = 0;
    EXPECT_EQ(nameOf(k, moved), nameOf(k, r));
    EXPECT_EQ(harness::requestStoreKey(k, moved).content.key, harness::requestStoreKey(k, r).content.key);
    gm20b::CompileRequest bound = r;
    bound.kernel.memory[2] = 4096;
    EXPECT_NE(nameOf(k, bound), nameOf(k, r));
    gm20b::CompileRequest system = r;
    system.libraries[0] = data(made_metallib::universal({"kernel_fn"}, 0x31));
    EXPECT_NE(nameOf(k, system), nameOf(k, r));
}

// A compute state's facts: keyed by its function's AIR, constants and limits as the pack keys PackComputeFacts, named apart
// from a kernel request of the same function and limits, and refused when its claimed key names another state
TEST(CompileRequest, ComputeFactsAreKeyedAsThePacksStateKey)
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Facts;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn", 0, "#0:33:05000000")};
    r.kernel.maxThreads = 256;
    r.kernel.multipleOfWidth = true;
    gm20b::AirHash air;
    air.fill(0x30);
    const std::string state = gm20b::computeStateKey(air, "#0:33:05000000", 256, true);
    r.key = gm20b::factsCacheKey(kStack, state).key;
    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(r), back));
    EXPECT_EQ(back.kind, gm20b::RequestKind::Facts);
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);
    EXPECT_EQ(p.key.key, "facts | " + state);

    gm20b::CompileRequest kernel = r;
    kernel.kind = gm20b::RequestKind::Kernel;
    EXPECT_NE(nameOf(p.key, kernel), p.name);
    gm20b::CompileRequest wider = r;
    wider.kernel.maxThreads = 512;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(wider), kStack, system, p, why));
    gm20b::CompileRequest two = r;
    two.stages.push_back(stage("k", "kernel_fn"));
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(two), back)) << "a facts request names one kernel";
}

// A kernel's linked functions (MPSGraph's kernels link their DAG library's privately) are its request's stages after the
// kernel, "kl" visible then "kp" private, and its key names each by AIR and constants past the kernel's own constants
// (compute_key.h computeLinkedText), for its facts, its dispatches and its reflection alike. The same kernel without
// them is another state; a linked tag cannot stand first.
TEST(CompileRequest, AKernelsLinkedFunctionsAreItsStagesAndPartOfItsKey)
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Facts;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.libraries.push_back(data(made_metallib::universal({"visible_fn", "prefixPrimary_i8"}, 0x40)));
    r.stages = {stage("k", "kernel_fn"), stage("kl", "visible_fn", 1, "#1:29:01"), stage("kp", "prefixPrimary_i8", 1)};
    r.kernel.maxThreads = 256;
    gm20b::AirHash kernelAir, visibleAir, privateAir;
    kernelAir.fill(0x30);
    visibleAir.fill(0x40);
    privateAir.fill(0x41);
    const std::string linked = gm20b::computeLinkedText({{false, visibleAir, "#1:29:01"}, {true, privateAir, ""}});
    EXPECT_EQ(gm20b::airText(privateAir).size(), 64u);
    EXPECT_EQ(gm20b::airText(privateAir).substr(0, 6), "414141");
    EXPECT_EQ(linked, " | kl " + gm20b::airText(visibleAir) + " #1:29:01 | kp " + gm20b::airText(privateAir));
    const std::string state = gm20b::computeStateKey(kernelAir, linked, 256, false);
    r.key = gm20b::factsCacheKey(kStack, state).key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);

    gm20b::CompileRequest alone = r;
    alone.stages.resize(1);
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(alone), kStack, system, p, why))
        << "the kernel alone claiming the linked state's key";
    alone.key = gm20b::factsCacheKey(kStack, gm20b::computeStateKey(kernelAir, "", 256, false)).key;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(alone), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey.find(" | kp "), std::string::npos);

    gm20b::CompileRequest kernel = r;
    kernel.kind = gm20b::RequestKind::Kernel;
    kernel.kernel.threads[0] = 64;
    kernel.kernel.threads[1] = kernel.kernel.threads[2] = 1;
    const std::string compute = gm20b::computeKey(kernelAir, linked, 256, false, kernel.kernel.threads, "", "", "");
    EXPECT_EQ(compute.rfind(state, 0), 0u);
    kernel.key = "kernel | " + compute;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(kernel), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, compute);

    gm20b::CompileRequest reflection = r;
    reflection.kind = gm20b::RequestKind::Reflection;
    reflection.key = gm20b::computeReflectionKey(kStack, state).key;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(reflection), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);

    gm20b::CompileRequest first = r;
    first.stages[0].tag = "kp";
    gm20b::CompileRequest back;
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(first), back));
    gm20b::CompileRequest render = r;
    render.stages[1].tag = "vl";
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(render), back)) << "a render stage's tag on a kernel";
}

// A stage-input kernel's facts: its stage input travels beside its constants and keys the state after them, as the pack
// keys it; a request whose stage input is not its key's is refused
TEST(CompileRequest, AStageInputKernelsFactsAreKeyedWithItsStageInput)
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Facts;
    r.libraries.push_back(data(made_metallib::universal({"kernel_fn"}, 0x30)));
    r.stages = {stage("k", "kernel_fn")};
    r.kernel.stageIn.attributes[0] = {29, 0, 1, 8, 1, 1};
    gm20b::AirHash air;
    air.fill(0x30);
    const std::string state = gm20b::computeStateKey(air, gm20b::stageInText(r.kernel.stageIn), 0, false);
    ASSERT_NE(state.find(" | in a0="), std::string::npos);
    r.key = gm20b::factsCacheKey(kStack, state).key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.packKey, state);
    gm20b::CompileRequest other = r;
    other.kernel.stageIn.attributes[0].offset = 4;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(other), kStack, system, p, why));
}

TEST(CompileRequest, BinaryFunctionKeysKeepTheirDefinitionAndSpecializedLinkName)
{
    gm20b::CompileRequest request;
    request.stack = kStack;
    request.kind = gm20b::RequestKind::Facts;
    request.libraries = {data(made_metallib::universal({"kernel_fn"}, 0x30, 0xee, {2})),
                         data(made_metallib::universal({"helper"}, 0x40, 0xee, {4}))};
    request.stages = {stage("k", "kernel_fn"), stage("kb", "helper", 1)};
    request.stages[1].name = "transform";
    gm20b::AirHash kernel;
    kernel.fill(0x30);
    gm20b::LinkedComputeFunction linked;
    linked.air.fill(0x40);
    linked.binary = true;
    linked.symbol = "helper";
    linked.name = "transform";
    request.key =
        gm20b::factsCacheKey(kStack, gm20b::computeStateKey(kernel, gm20b::computeLinkedText({linked}), 0, false)).key;
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared prepared;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(request), kStack, system, prepared, why)) << why;
    gm20b::CompileRequest renamed = request;
    renamed.stages[1].name = "another_transform";
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(renamed), kStack, system, prepared, why));
    gm20b::CompileRequest ordinary = request;
    ordinary.stages[1].tag = "kl";
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(ordinary), kStack, system, prepared, why));
}

TEST(CompileRequest, GroupMembershipAndOrderAreKeptInEveryComputeRequest)
{
    gm20b::CompileRequest request;
    request.stack = kStack;
    request.kind = gm20b::RequestKind::Facts;
    request.libraries = {data(made_metallib::universal({"kernel_fn"}, 0x30)),
                         data(made_metallib::universal({"helper", "other"}, 0x40))};
    request.stages = {stage("k", "kernel_fn"), stage("kg", "helper", 1, "#1:29:01"), stage("kg", "other", 1)};
    request.stages[1].name = "transform";
    request.stages[1].group = request.stages[2].group = std::string("choice\0other", 12) + " | kg :";
    gm20b::AirHash kernel;
    kernel.fill(0x30);
    gm20b::LinkedComputeFunction first, second;
    first.air.fill(0x40);
    first.symbol = "helper";
    first.name = "transform";
    first.constants = "#1:29:01";
    first.group = request.stages[1].group;
    second.air.fill(0x41);
    second.symbol = second.name = "other";
    second.group = first.group;
    const std::string state = gm20b::computeStateKey(kernel, gm20b::computeLinkedText({first, second}), 0, false);
    gm20b::LinkedComputeFunction prefix = first;
    prefix.group = "choice";
    EXPECT_NE(gm20b::computeLinkedText({prefix, second}), gm20b::computeLinkedText({first, second}));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared prepared;
    std::string why;
    for (const auto kind : {gm20b::RequestKind::Facts, gm20b::RequestKind::Reflection, gm20b::RequestKind::Kernel}) {
        request.kind = kind;
        request.key = kind == gm20b::RequestKind::Facts ? gm20b::factsCacheKey(kStack, state).key
                      : kind == gm20b::RequestKind::Reflection
                          ? gm20b::computeReflectionKey(kStack, state).key
                          : "kernel | " + state + " | tg=0,0,0 | tgmem= | tex= | smp=";
        gm20b::CompileRequest back;
        ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(request), back));
        EXPECT_EQ(back.stages[1].group, request.stages[1].group);
        EXPECT_EQ(back.stages[1].name, "transform");
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(back), kStack, system, prepared, why)) << why;
        const gm20b::Digest original = prepared.name;
        for (const unsigned mutation : {0u, 1u, 2u, 3u}) {
            gm20b::CompileRequest changed = request;
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
            EXPECT_NE(nameOf(prepared.key, changed), original);
            EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(changed), kStack, system, prepared, why));
        }
    }
    gm20b::CompileRequest misplaced = request, back;
    misplaced.stages[0].group = "choice";
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(misplaced), back));
}

// A specialized function's attributes: one stage tagged "a", keyed by its AIR, constants and the name it was made under,
// refused with any other stage or a key naming other constants; the blob carries both lists, nil apart from empty
TEST(CompileRequest, AFunctionsAttributesAreKeyedByItsAirConstantsAndName)
{
    gm20b::CompileRequest r;
    r.stack = kStack;
    r.kind = gm20b::RequestKind::Reflection;
    r.libraries.push_back(data(made_metallib::universal({"vertex_fn"}, 0x30)));
    r.stages = {stage("a", "vertex_fn", 0, "#4:53:00")};
    gm20b::AirHash air;
    air.fill(0x30);
    const std::string function = gm20b::functionAttributesKey(air, "#4:53:00", "vertex_fn");
    r.key = gm20b::functionAttributesCacheKey(kStack, function).key;
    EXPECT_TRUE(gm20b::functionAttributes(r));
    EXPECT_FALSE(gm20b::computeReflection(r));
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(r), kStack, system, p, why)) << why;
    EXPECT_EQ(p.key.key, "attributes | " + function);
    gm20b::CompileRequest other = r;
    other.stages[0].constants = "#4:53:01";
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(other), kStack, system, p, why));
    gm20b::CompileRequest two = r;
    two.stages.push_back(stage("a", "vertex_fn"));
    gm20b::CompileRequest back;
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(two), back)) << "an attributes request names one function";

    gm20b::FunctionAttributeTag position, normal;
    position.name = "position";
    position.type = 6;
    position.active = true;
    normal.name = "normal";
    normal.index = 5;
    normal.type = 5;
    normal.patchData = normal.controlPointData = true;
    const gm20b::AttributeList vertex = std::vector<gm20b::FunctionAttributeTag>{position, normal};
    gm20b::AttributeList readVertex, readStage = std::vector<gm20b::FunctionAttributeTag>{};
    ASSERT_TRUE(gm20b::decodeAttributeLists(gm20b::encodeAttributeLists(vertex, std::nullopt), readVertex, readStage));
    ASSERT_TRUE(readVertex.has_value());
    EXPECT_FALSE(readStage.has_value()) << "nil, not empty";
    ASSERT_EQ(readVertex->size(), 2u);
    EXPECT_EQ((*readVertex)[1].name, "normal");
    EXPECT_EQ((*readVertex)[1].index, 5);
    EXPECT_TRUE((*readVertex)[1].patchData && (*readVertex)[1].controlPointData && !(*readVertex)[1].active);
    EXPECT_FALSE(
        gm20b::decodeAttributeLists(gm20b::encodeAttributeLists(vertex, std::nullopt) + "x", readVertex, readStage));
}

TEST(CompileRequest, TheCompilerRefusesWhatItCannotTrust)
{
    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared p;
    std::string why;
    gm20b::CompileRequest r = renderRequest();
    r.key = runtimeKey(r).key;

    gm20b::CompileRequest claimed = r;
    claimed.libraries[0].system = true;
    claimed.libraries[0].path = "/System/Library/Frameworks/RenderBox.framework/default.metallib";
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(claimed), kStack, system, p, why));

    gm20b::CompileRequest leaving = r;
    leaving.libraries[0] = systemLibrary("/System/Library/../../private/var/mobile/x.metallib");
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(leaving), kStack, system, p, why));

    gm20b::CompileRequest empty = r;
    empty.libraries[0].bytes.clear();
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(empty), kStack, system, p, why));

    gm20b::CompileRequest nowhere = r;
    nowhere.stages[1].library = 3;
    EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(nowhere), kStack, system, p, why));

    std::string cut = gm20b::encodeRequest(r);
    cut.pop_back();
    EXPECT_FALSE(switch_compiler::prepare(cut, kStack, system, p, why));

    std::string real;
    EXPECT_FALSE(switch_compiler::SystemLibraries::resolve("", "/System/Library/../../etc/x.metallib", real, why));
}

// A refusal's one error line names the kind of request as its reason key, then what was refused, of which functions by the
// names their client made them under, for which process, with the compiler's reason (MPS's facts request of its identity
// kernel, whose private functions were the front end's specializations and stitches, was refused with no line at all)
TEST(CompileRequest, ARefusalsLineNamesItsFunctionsAndReason)
{
    gm20b::CompileRequest facts;
    facts.kind = gm20b::RequestKind::Facts;
    facts.stages = {stage("k", "ndArrayIdentity", 0, "#122:33:01000100"), stage("kp", "prefixPrimary_f", 1),
                    stage("kp", "prefixPrimary_i8", 2)};
    facts.stages[1].function = "prefixPrimary_i8_to_f";
    const switch_compiler::RefusalLine refused =
        switch_compiler::refusalLine(facts, 595, "no compute prepare: Linking");
    EXPECT_STREQ(refused.reason, "facts");
    EXPECT_EQ(
        refused.text,
        "pid 595's compute state of ndArrayIdentity with 2 linked functions refused: no compute prepare: Linking");

    gm20b::CompileRequest kernel = facts;
    kernel.kind = gm20b::RequestKind::Kernel;
    kernel.stages.resize(2);
    kernel.kernel.threads[0] = 8;
    kernel.kernel.threads[1] = 8;
    kernel.kernel.threads[2] = 1;
    const switch_compiler::RefusalLine dispatch = switch_compiler::refusalLine(kernel, 7, "plan: Format");
    EXPECT_STREQ(dispatch.reason, "kernel");
    EXPECT_EQ(dispatch.text,
              "pid 7's kernel ndArrayIdentity at 8,8,1 threads with 1 linked functions refused: plan: Format");

    gm20b::CompileRequest render = renderRequest();
    render.stages[1].name = "fetch_add";
    const switch_compiler::RefusalLine state = switch_compiler::refusalLine(render, 604, "plan: FramebufferFetch");
    EXPECT_STREQ(state.reason, "state");
    EXPECT_EQ(state.text,
              "pid 604's render state of vertex_fn and fetch_add (fragment_fn) refused: plan: FramebufferFetch");

    gm20b::CompileRequest library;
    library.kind = gm20b::RequestKind::Library;
    library.codeGenType = gm20b::kCodeGenSource;
    EXPECT_STREQ(switch_compiler::refusalLine(library, 1, "error").reason, "library");
    EXPECT_EQ(switch_compiler::refusalLine(library, 1, "error").text,
              "pid 1's library of front end request type 13 refused: error");
}

// Boot 2: a kernel whose translation fails at every try (its dispatches' command buffers fail) logged only the
// compiler's own "not made" line, and no refusal line
TEST(CompileRequest, ATranslationThatFailsAtEveryTryIsRefusedForGood)
{
    EXPECT_TRUE(switch_compiler::failsForGood(
        "m2v refused its translation: fails: kernel sampler slot 0 samples texture slots 0 and 1"));
    EXPECT_FALSE(switch_compiler::failsForGood("m2v refused its translation: retry: compile: Budget(Time)"));
    EXPECT_FALSE(switch_compiler::failsForGood("the store took no entry"));
}

TEST(CompileRequest, ConstantsReadBackAsTheirText)
{
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    const uint32_t word = 0x01020304;
    const float half4[4] = {1, 2, 3, 4};
    [values setConstantValue:&word type:MTLDataTypeUInt atIndex:3];
    [values setConstantValue:half4 type:MTLDataTypeFloat4 withName:@"tint"];
    const std::string text = gm20b::constantsText(values);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(gm20b::constantsText(gm20b::constantValuesOf(text)), text);
    EXPECT_EQ(gm20b::constantValuesOf("#0:9999:00"), nil);
}

TEST(CompileRequest, ClientLibraryHandleRoundTripsAndMatchesInline)
{
    gm20b::CompileRequest inlineReq = renderRequest();
    const gm20b::CacheKey k = runtimeKey(inlineReq);
    inlineReq.key = k.key;
    const std::string libBytes = inlineReq.libraries[0].bytes;

    gm20b::CompileRequest handleReq = inlineReq;
    handleReq.libraries[0].bytes.clear();
    handleReq.libraries[0].libraryId = 7;

    gm20b::CompileRequest back;
    ASSERT_TRUE(gm20b::decodeRequest(gm20b::encodeRequest(handleReq), back));
    EXPECT_EQ(back.libraries[0].libraryId, 7u);
    EXPECT_TRUE(back.libraries[0].bytes.empty());

    gm20b::CompileRequest both = handleReq;
    both.libraries[0].bytes = libBytes;
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(both), back));
    gm20b::CompileRequest sysHandle = handleReq;
    sysHandle.libraries[0].system = true;
    sysHandle.libraries[0].path = "/System/Library/Frameworks/RenderBox.framework/default.metallib";
    EXPECT_FALSE(gm20b::decodeRequest(gm20b::encodeRequest(sysHandle), back));

    switch_compiler::SystemLibraries system;
    switch_compiler::Prepared inlinePrepared;
    std::string why;
    ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(inlineReq), kStack, system, inlinePrepared, why)) << why;

    {
        switch_compiler::Prepared rejected;
        EXPECT_FALSE(switch_compiler::prepare(gm20b::encodeRequest(handleReq), kStack, system, rejected, why));
    }

    const std::string libBytes2 = made_metallib::universal({"vertex_fn", "fragment_fn"}, 0xA0, 0x22);
    int mapCalls = 0, unmapCalls = 0;
    std::map<std::pair<uint64_t, uint64_t>, const std::string *> backing = {
        {{10, 7}, &libBytes},
        {{10, 8}, &libBytes2},
    };
    switch_compiler::ClientLibraries clientLibs(
        [&](uint64_t client, uint64_t id, const uint8_t *&ptr, size_t &sz, std::string &err) {
            mapCalls++;
            const auto it = backing.find({client, id});
            if (it == backing.end())
                return err = "not found", false;
            ptr = reinterpret_cast<const uint8_t *>(it->second->data());
            sz = it->second->size();
            return true;
        },
        [&](uint64_t, uint64_t) {
            unmapCalls++;
        },
        1, 1u << 20);

    switch_compiler::Prepared handlePrepared;
    ASSERT_TRUE(
        switch_compiler::prepare(gm20b::encodeRequest(handleReq), kStack, system, handlePrepared, why, 10, &clientLibs))
        << why;
    EXPECT_EQ(mapCalls, 1);
    EXPECT_EQ(unmapCalls, 0);
    EXPECT_EQ(handlePrepared.key.key, inlinePrepared.key.key);
    EXPECT_EQ(handlePrepared.name, inlinePrepared.name);
    ASSERT_EQ(handlePrepared.clientLibraries.size(), 1u);
    ASSERT_NE(handlePrepared.clientLibraries[0], nullptr);

    {
        switch_compiler::Prepared secondPrepared;
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(handleReq), kStack, system, secondPrepared, why, 10,
                                             &clientLibs))
            << why;
        EXPECT_EQ(mapCalls, 1);
        EXPECT_EQ(secondPrepared.clientLibraries[0].get(), handlePrepared.clientLibraries[0].get());
    }

    gm20b::CompileRequest otherReq = handleReq;
    otherReq.libraries[0].libraryId = 8;
    switch_compiler::Prepared otherPrepared;
    ASSERT_TRUE(
        switch_compiler::prepare(gm20b::encodeRequest(otherReq), kStack, system, otherPrepared, why, 10, &clientLibs))
        << why;
    EXPECT_EQ(mapCalls, 2);
    EXPECT_EQ(unmapCalls, 0) << "entry 7 is still held by handlePrepared";
    handlePrepared = {};
    EXPECT_EQ(unmapCalls, 1) << "entry 7 unmaps as soon as the last Prepared releases it";

    clientLibs.trim();
    EXPECT_EQ(unmapCalls, 1);
    otherPrepared = {};
    EXPECT_EQ(unmapCalls, 2);
    ASSERT_TRUE(
        switch_compiler::prepare(gm20b::encodeRequest(handleReq), kStack, system, handlePrepared, why, 10, &clientLibs))
        << why;
    EXPECT_EQ(mapCalls, 3);
}

}  // namespace
