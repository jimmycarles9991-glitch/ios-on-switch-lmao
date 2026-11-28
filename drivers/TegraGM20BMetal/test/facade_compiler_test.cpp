// SPDX-License-Identifier: MIT
// What the system shader compiler answers in place of m2v (TegraGM20BMetalCompleteRemote.mm, compiler/): a compute state's
// facts the pack lacks, made once and then read from its store, a kernel it refused, whose command buffer fails, and a
// kernel with linked functions (MPSGraph's). The kernel model runs a stand-in compiler over a store, as
// SwitchShaderCompiler fills it.
#include "facade_harness.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../compile_request.h"
#include "../compute_facts_entry.h"
#include "../compute_key.h"
#include "request_store_key.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "bazel/testing/fresh_process.h"

#include <dispatch/dispatch.h>
#include <gtest/gtest.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

NSURL *fileURL(const std::string &path)
{
    return [NSURL fileURLWithPath:@(path.c_str())];
}

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;
constexpr uint32_t kFacts[3] = {640, 32, 2048};

// The shader compiler as the kernel model runs it: each facts request answered with kFacts in the store, under the name
// the request's own state key and libraries give
struct FactsCompiler {
    switch_seed_pack::StoreWriter store;
    std::vector<std::string> asked;
    std::vector<gm20b::CompileRequest> requests;

    bool serve()
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0x99;
        std::string why;
        if (!store.open(dir() + "/store", identity, 64, 1u << 20, why))
            return false;
        kernel_model::model.compilerFlags = kServing | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            gm20b::CompileRequest r;
            constexpr char kFactsPrefix[] = "facts | ", kKernelPrefix[] = "kernel | ";
            if (!gm20b::decodeRequest(bytes, r))
                return int32_t(TegraGM20BCompileUnavailable);
            asked.push_back(r.key);
            requests.push_back(r);
            const bool facts = r.kind == gm20b::RequestKind::Facts && !r.key.rfind(kFactsPrefix, 0);
            const bool kernel = r.kind == gm20b::RequestKind::Kernel && !r.key.rfind(kKernelPrefix, 0);
            if (!facts && !kernel)
                return int32_t(TegraGM20BCompileUnavailable);
            const gm20b::CacheKey k = facts ? gm20b::factsCacheKey(r.stack, r.key.substr(sizeof kFactsPrefix - 1))
                                            : gm20b::kernelCacheKey(r.stack, r.key.substr(sizeof kKernelPrefix - 1));
            const gm20b::StoreKey key = harness::requestStoreKey(k, r);
            const std::string entry =
                facts ? gm20b::encodeFactsEntry(key.content, kFacts, "")
                      : gm20b::encodeKernelEntry(key.content, nullptr, "a kernel this compiler refuses");
            store.put(key.name.data(), reinterpret_cast<const uint8_t *>(entry.data()), uint32_t(entry.size()));
            return int32_t(facts ? TegraGM20BCompileMade : TegraGM20BCompileRefused);
        };
        return true;
    }

    // last: goes first, before what the compiler hook captures, which the device may still call
    kernel_model::HooksCleared cleared;
};

id<MTLDevice> completing()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return device;
}

// A compute state the pack has no facts of is answered by the shader compiler at its creation, once for every state of
// the same key (the second is the store's), and its answers are the compiler's
void computeFactsFromTheCompiler()
{
    FactsCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    const std::string metallib = writeMetallib();
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(metallib) error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    id<MTLComputePipelineState> made = [device newComputePipelineStateWithFunction:kernel error:&error];
    ASSERT_TRUE(made != nil) << text(error);
    EXPECT_EQ(made.maxTotalThreadsPerThreadgroup, kFacts[0]);
    EXPECT_EQ(made.threadExecutionWidth, kFacts[1]);
    EXPECT_EQ(made.staticThreadgroupMemoryLength, kFacts[2]);
    std::map<std::string, gm20b::AirHash> hashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(metallib, hashes));
    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0], "facts | " + gm20b::computeStateKey(hashes["k"], "", 0, false));
    id<MTLComputePipelineState> again = [device newComputePipelineStateWithFunction:kernel error:&error];
    ASSERT_TRUE(again != nil) << text(error);
    EXPECT_EQ(again.staticThreadgroupMemoryLength, kFacts[2]);
    EXPECT_EQ(compiler.asked.size(), 1u) << "the second state's facts are the store's";
}

// A kernel with a stage input the pack has no facts of: its facts request carries the stage input beside the constants, and
// is keyed with it as the pack keys the state, so the compiler compiles the kernel the client creates
void stageInputFactsFromTheCompiler()
{
    FactsCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    const std::string metallib = writeMetallib();
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(metallib) error:&error];
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = [library newFunctionWithName:@"k"];
    d.stageInputDescriptor.attributes[0].format = MTLAttributeFormatFloat2;
    d.stageInputDescriptor.attributes[0].bufferIndex = 1;
    d.stageInputDescriptor.layouts[1].stride = 8;
    id<MTLComputePipelineState> made =
        [device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
    ASSERT_TRUE(made != nil) << text(error);
    EXPECT_EQ(made.maxTotalThreadsPerThreadgroup, kFacts[0]);
    const std::string stageIn = gm20b::stageInText(gm20b::stageInFieldsOf(d.stageInputDescriptor));
    ASSERT_FALSE(stageIn.empty());
    std::map<std::string, gm20b::AirHash> hashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(metallib, hashes));
    ASSERT_EQ(compiler.requests.size(), 1u);
    EXPECT_EQ(compiler.requests[0].key, "facts | " + gm20b::computeStateKey(hashes["k"], stageIn, 0, false));
    EXPECT_EQ(gm20b::stageInText(compiler.requests[0].kernel.stageIn), stageIn)
        << "the request carries the stage input its key names";
}

// A dispatch whose kernel the shader compiler refused fails its command buffer, as Metal's does for a pipeline it could not
// compile, whether the refusal comes before the command buffer is recorded or at its submission; nothing goes to m2v
void refusedKernel()
{
    FactsCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:kernel error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLBuffer> out = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (int round = 0; round < 2; round++) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:state];
        [e setBuffer:out offset:0 atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusError) << "round " << round;
        EXPECT_NE(text(cb.error.localizedDescription).find("refused"), std::string::npos) << text(cb.error);
    }
}

// MPSGraph's kernels link the functions of the library MPSKernelDAG stitched as private functions. Such a state is keyed
// with them (compute_key.h computeLinkedText): its facts and its dispatch's kernel are asked of the shader compiler with
// them as stages after the kernel ("kp"), as is its reflection, and the same kernel without them is another state. Nothing
// goes to m2v.
void linkedKernel()
{
    FactsCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    const std::string metallib = writeMetallib();
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(metallib) error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    const std::string stitched =
        made_metallib::thin({"prefixPrimary_i8", "postfixPrimary_i8"}, {4, 4},
                            {made_metallib::wrappedBitcode(0x81), made_metallib::wrappedBitcode(0x82)});
    dispatch_data_t data =
        dispatch_data_create(stitched.data(), stitched.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> dag = [device newLibraryWithData:data error:&error];
    ASSERT_TRUE(dag != nil) << text(error);
    MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
    linked.privateFunctions =
        @[ [dag newFunctionWithName:@"prefixPrimary_i8"], [dag newFunctionWithName:@"postfixPrimary_i8"] ];
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = kernel;
    d.linkedFunctions = linked;
    id<MTLComputePipelineState> state =
        [device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    EXPECT_EQ(state.threadExecutionWidth, kFacts[1]);

    std::map<std::string, gm20b::AirHash> hashes, dagHashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(metallib, hashes));
    ASSERT_TRUE(
        gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(stitched.data()), stitched.size(), dagHashes));
    const std::string stateKey =
        gm20b::computeStateKey(hashes["k"],
                               gm20b::computeLinkedText({{true, dagHashes["prefixPrimary_i8"], ""},
                                                         {true, dagHashes["postfixPrimary_i8"], ""}}),
                               0, false);
    ASSERT_EQ(compiler.requests.size(), 1u);
    const gm20b::CompileRequest &facts = compiler.requests[0];
    EXPECT_EQ(facts.key, "facts | " + stateKey);
    ASSERT_EQ(facts.stages.size(), 3u);
    EXPECT_EQ(facts.stages[0].tag, "k");
    EXPECT_EQ(facts.stages[1].tag, "kp");
    EXPECT_EQ(facts.stages[1].function, "prefixPrimary_i8");
    EXPECT_EQ(facts.stages[2].function, "postfixPrimary_i8");
    EXPECT_EQ(facts.libraries.size(), 2u);

    id<MTLBuffer> out = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:state];
    [e setBuffer:out offset:0 atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(compiler.requests.size(), 2u);
    const gm20b::CompileRequest &dispatched = compiler.requests[1];
    EXPECT_EQ(dispatched.kind, gm20b::RequestKind::Kernel);
    EXPECT_EQ(dispatched.key.rfind("kernel | " + stateKey + " | tg=64,1,1", 0), 0u) << dispatched.key;
    EXPECT_EQ(dispatched.stages.size(), 3u);
    EXPECT_NE(text(cb.error.localizedDescription).find("refused"), std::string::npos) << text(cb.error);

    EXPECT_NE(stateKey, gm20b::computeStateKey(hashes["k"], "", 0, false)) << "the kernel alone is another state";

    MTLAutoreleasedComputePipelineReflection reflection = nil;
    [device newComputePipelineStateWithDescriptor:d
                                          options:MTLPipelineOptionBufferTypeInfo
                                       reflection:&reflection
                                            error:&error];
    ASSERT_GE(compiler.requests.size(), 3u) << "its reflection is asked of the compiler as its facts were";
    const gm20b::CompileRequest &reflected = compiler.requests[2];
    EXPECT_EQ(reflected.kind, gm20b::RequestKind::Reflection);
    EXPECT_EQ(reflected.key, gm20b::computeReflectionKey(reflected.stack, stateKey).key);
    EXPECT_EQ(reflected.stages.size(), 3u);
}

// With no compiler this boot the creation fails as Metal's does when it cannot compile, never through m2v
void computeFactsWithNoCompiler()
{
    kernel_model::model.compilerFlags = TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined;
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    EXPECT_EQ([device newComputePipelineStateWithFunction:kernel error:&error], nil);
    EXPECT_NE(text(error.localizedDescription).find("no shader compiler comes this boot"), std::string::npos)
        << text(error);
    EXPECT_EQ(kernel_model::model.compiles, 0u);
}

}  // namespace

TEST(GM20BFacadeCompiler, AStageInputKernelsFactsAreTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(stageInputFactsFromTheCompiler);
}

TEST(GM20BFacadeCompiler, AComputeStateThePackLacksIsAnsweredByTheShaderCompiler)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeFactsFromTheCompiler);
}

TEST(GM20BFacadeCompiler, AKernelTheCompilerRefusedFailsItsCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(refusedKernel);
}

TEST(GM20BFacadeCompiler, AKernelsLinkedFunctionsAreKeyedAndAskedOfTheShaderCompiler)
{
    switch_ios::test::ExpectPassesInFreshProcess(linkedKernel);
}

TEST(GM20BFacadeCompiler, AComputeStateWithNoCompilerFailsItsCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeFactsWithNoCompiler);
}
