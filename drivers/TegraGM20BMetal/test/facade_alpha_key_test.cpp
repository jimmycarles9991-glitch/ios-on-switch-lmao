// SPDX-License-Identifier: MIT
// alphaToCoverageEnabled joins the render pipeline key (text and fast), the compile request (SWCREQ07) and the
// translator descriptor: descriptors differing only in the flag key apart, a pack holding only the plain state misses
// when the flag is set, and a fetching state compiled with the flag gets no storage form.
#include "facade_harness.h"
#include "bazel/testing/fresh_process.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../TegraGM20BMetalDirect.h"
#include "../TegraGM20BMetalOffline.h"
#include "../compile_request.h"
#include "../emit.h"
#include "../pack/probe_states.h"

#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

@protocol AlphaKeyStateSPI
- (NSNumber *)switchPackHit;
@end

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm;

MTLRenderPipelineDescriptor *fetchDescriptor(id<MTLLibrary> lib, bool alphaToCoverage)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.rasterSampleCount = 4;
    d.colorAttachments[0].pixelFormat = kPacked;
    d.alphaToCoverageEnabled = alphaToCoverage;
    d.vertexFunction = [lib newFunctionWithName:@"stages_vertex"];
    d.fragmentFunction = [lib newFunctionWithName:@"stages_fetch_fragment"];
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
    v.attributes[0].format = MTLVertexFormatFloat4;
    v.attributes[0].offset = 0;
    v.attributes[0].bufferIndex = 0;
    v.attributes[1].format = MTLVertexFormatFloat2;
    v.attributes[1].offset = 16;
    v.attributes[1].bufferIndex = 0;
    v.layouts[0].stride = 24;
    v.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    v.layouts[0].stepRate = 1;
    d.vertexDescriptor = v;
    return d;
}

void flaggedMissesPlainPack()
{
    const std::string pack = writePack(packKey(kPacked));
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack);
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << (error ? error.localizedDescription.UTF8String : "");

    MTLRenderPipelineDescriptor *plain = descriptor(library, kPacked);
    id<MTLRenderPipelineState> hit = [device newRenderPipelineStateWithDescriptor:plain error:&error];
    ASSERT_TRUE(hit != nil) << (error ? error.localizedDescription.UTF8String : "");
    EXPECT_TRUE([(id<AlphaKeyStateSPI>)hit switchPackHit].boolValue);

    MTLRenderPipelineDescriptor *flagged = descriptor(library, kPacked);
    flagged.alphaToCoverageEnabled = YES;
    error = nil;
    id<MTLRenderPipelineState> missed = [device newRenderPipelineStateWithDescriptor:flagged error:&error];
    EXPECT_EQ(missed, nil);
    ASSERT_TRUE(error != nil);
    EXPECT_EQ(error.domain, MTLLibraryErrorDomain);
}

TEST(GM20BAlphaKey, DescriptorsDifferingOnlyInAlphaToCoverageKeyApart)
{
    const char *path = getenv("SWITCH_STAGES_METALLIB");
    ASSERT_TRUE(path && *path) << "SWITCH_STAGES_METALLIB unset";
    id<MTLDevice> offline = (id<MTLDevice>)[GM20BOfflineDevice new];
    NSError *error = nil;
    id<MTLLibrary> lib = [offline newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
    ASSERT_TRUE(lib != nil) << (error ? error.localizedDescription.UTF8String : "");

    MTLRenderPipelineDescriptor *plain = [MTLRenderPipelineDescriptor new];
    plain.rasterSampleCount = 4;
    plain.colorAttachments[0].pixelFormat = kPacked;
    MTLRenderPipelineDescriptor *flagged = [plain copy];
    flagged.alphaToCoverageEnabled = YES;

    const gm20b::StageKey v{"stages_vertex", ""}, f{"stages_fetch_fragment", ""};
    const std::string plainText = gm20b::pipelineKey(plain, &v, &f, "", "QuartzCore");
    const std::string flaggedText = gm20b::pipelineKey(flagged, &v, &f, "", "QuartzCore");
    EXPECT_EQ(plainText.find("alphacov="), std::string::npos) << plainText;
    EXPECT_NE(flaggedText.find("ds=0/0 alphacov=1 "), std::string::npos) << flaggedText;
    EXPECT_NE(plainText, flaggedText);

    const uint64_t plainFast = gm20b::fastKey(gm20b::fieldsOf(plain), &v, &f, "", "QuartzCore");
    const uint64_t flaggedFast = gm20b::fastKey(gm20b::fieldsOf(flagged), &v, &f, "", "QuartzCore");
    EXPECT_NE(plainFast, flaggedFast);
    EXPECT_EQ(fastKeyOfText(plainText), plainFast);
    EXPECT_EQ(fastKeyOfText(flaggedText), flaggedFast);

    probe::State parsed;
    std::string why;
    const std::string line =
        "R ok=1 samples=4 topo=0 ds=0/0 alphacov=1 c0=80:15:0:1:0:0:1:0:0 | v stages_vertex | f stages_fetch_fragment";
    ASSERT_TRUE(probe::parseState(lib, line, parsed, "QuartzCore", &why)) << why;
    EXPECT_TRUE(parsed.d.alphaToCoverageEnabled);
    EXPECT_EQ(parsed.key, flaggedText);
    EXPECT_EQ(parsed.fast, flaggedFast);
}

TEST(GM20BAlphaKey, PackHoldingOnlyPlainStateMissesWhenAlphaToCoverageIsSet)
{
    switch_ios::test::ExpectPassesInFreshProcess(flaggedMissesPlainPack);
}

TEST(GM20BAlphaKey, FetchingStateWithAlphaToCoverageGetsNoStorageFormThroughCompileRequest)
{
    const char *path = getenv("SWITCH_STAGES_METALLIB");
    ASSERT_TRUE(path && *path) << "SWITCH_STAGES_METALLIB unset";
    id<MTLDevice> offline = (id<MTLDevice>)[GM20BOfflineDevice new];
    NSError *error = nil;
    id<MTLLibrary> lib = [offline newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
    ASSERT_TRUE(lib != nil) << (error ? error.localizedDescription.UTF8String : "");

    const auto compile = [&](bool alphaToCoverage, gm20b::RuntimeRecord &out) {
        MTLRenderPipelineDescriptor *src = fetchDescriptor(lib, alphaToCoverage);
        gm20b::CompileRequest req;
        req.stack = 0x5357495443480003ull;
        req.fields = gm20b::fieldsOf(src);
        gm20b::RequestLibrary rl;
        rl.path = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
        rl.system = true;
        req.libraries.push_back(rl);
        req.stages = {{"v", "stages_vertex", "", "stages_vertex", false, 0, ""},
                      {"f", "stages_fetch_fragment", "", "stages_fetch_fragment", false, 0, ""}};
        bool own = false;
        req.key = gm20b::requestPackKey(req, own);
        const std::string wire = gm20b::encodeRequest(req);
        EXPECT_EQ(wire.substr(0, 8), "SWCREQ07");
        gm20b::CompileRequest back;
        ASSERT_TRUE(gm20b::decodeRequest(wire, back));

        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        gm20b::applyFields(back.fields, d);
        d.vertexFunction = [lib newFunctionWithName:@"stages_vertex"];
        d.fragmentFunction = [lib newFunctionWithName:@"stages_fetch_fragment"];
        EXPECT_EQ(bool(d.alphaToCoverageEnabled), alphaToCoverage);

        gm20b::PackRules rules;
        rules.varying = gm20b::emittableMethods();
        gm20b::Direct direct(std::move(rules), false, offline::translator());
        std::string why;
        const gm20b::CompileOutcome outcome = direct.make(d, gm20b::requestPackKey(back, own), out, why);
        ASSERT_EQ(outcome, gm20b::CompileOutcome::Made) << why;
    };

    gm20b::RuntimeRecord plain, flagged;
    ASSERT_NO_FATAL_FAILURE(compile(false, plain));
    ASSERT_NO_FATAL_FAILURE(compile(true, flagged));
    EXPECT_NE(plain.pipeline.storageMask, 0u) << "plain fetching state should use a storage form";
    EXPECT_EQ(flagged.pipeline.storageMask, 0u) << "alpha-to-coverage fetching state must refuse the storage form";
    EXPECT_NE(flagged.programs[1].image, plain.programs[1].image);
}

}  // namespace
