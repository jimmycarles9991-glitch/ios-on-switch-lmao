// SPDX-License-Identifier: MIT
// A render state the pack lacks, created on the raw facade over the kernel model with the shader compiler broker's answers
// set by each test: where the compiler makes no record the creation fails with an NSError as Metal's does, never handing
// back a state whose draws go elsewhere, and the client compiles nothing itself, with no compiler at all too. The m2v stand-in makes each library's twin
// of the same metallib (raw_facade_compute.h), so the facade keys the state as on the device.
#include "raw_facade_compute.h"
#include "bazel/testing/fresh_process.h"
#include "../compiler/compiler_request.h"
#include "../raw/runtime/raw_compile_request.h"
#include "../raw/runtime/raw_device_answers.h"
#include "bazel/testing/source_text.h"

#include <gtest/gtest.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLLibrary> library;
};

// The facade completing the states its pack lacks, the broker answering as set, and a library of two functions
Opened opened(uint32_t flags, std::vector<int32_t> results = {TegraGM20BCompileUnavailable})
{
    kernel_model::model.compilerFlags = flags;
    kernel_model::model.compileResults = std::move(results);
    id<MTLDevice> device =
        open(writePack(packKey(MTLPixelFormatBGRA8Unorm)), [HarnessM2VMethods class], [HarnessComputeM2V class]);
    if (!device)
        return {};
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    raw_metal_complete_states(cache.c_str(), nullptr);
    const std::string bytes = made_metallib::universal({"v", "f"}, 0x61, 0xEE, {0, 1});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    return {device, [device newLibraryWithData:data error:&error]};
}

MTLRenderPipelineDescriptor *unpacked(id<MTLLibrary> library)
{
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatRGBA8Unorm);
    d.label = @"unpacked";
    return d;
}

// Its creation's error text, "" for a state made; the client compiled nothing itself
std::string created(id<MTLDevice> device, MTLRenderPipelineDescriptor *d)
{
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (state)
        return "";
    return error ? text(error.localizedDescription) : "nil with no error";
}

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;

TEST(RawFacadeCreation, NoCompilerComingFailsTheCreationWithNoAsk)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined);
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

// switch-no-bt on iOS 17: the kernel has the compiler's methods but no compiler comes, and none declined
TEST(RawFacadeCreation, NoCompilerAtAllFailsTheCreationByNameWithNoCompileHere)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(TegraGM20BCompilerCapable);
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

// Host tests build the release arm of anything under SWITCH_DEBUG (undefined here): the one behaviour above is both builds'
TEST(RawFacadeCreation, DebugAndReleaseClientsCompleteStatesAlike)
{
    for (const char *file :
         {"../raw/runtime/RawComplete.h", "../raw/runtime/RawComplete.mm", "../raw/runtime/RawCompleteRemote.mm"}) {
        const std::string source = switch_ios::test::SourceText(file);
        EXPECT_EQ(switch_ios::test::Configured(source, "SWITCH_DEBUG", true),
                  switch_ios::test::Configured(source, "SWITCH_DEBUG", false))
            << file;
    }
}

TEST(RawFacadeCreation, ARequestTheCompilerDoesNotTakeIsAskedOnceMoreThenFails)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler did not take the state"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 2u);
    });
}

TEST(RawFacadeCreation, ARecordTheStoreDoesNotKeepIsAskedOnceMoreThenFails)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileMade});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler's store does not keep the state"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 2u);
    });
}

TEST(RawFacadeCreation, ACompileThatCouldNotRunFailsTheCreationAfterOneRetry)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileFailed});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the state's compile could not run"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 1u);
    });
}

// Another device's function names no library a request can carry: Metal refuses such a state
TEST(RawFacadeCreation, AnotherDevicesLinkedFunctionFailsTheCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileMade});
        ASSERT_TRUE(library != nil);
        MTLRenderPipelineDescriptor *d = unpacked(library);
        HarnessFunction *foreign = [HarnessFunction new];
        foreign->_name = @"f";
        foreign->_metallib = [NSData dataWithBytes:"" length:0];
        MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
        linked.functions = @[ (id<MTLFunction>)foreign ];
        d.fragmentLinkedFunctions = linked;
        const std::string why = created(device, d);
        EXPECT_FALSE(why.empty());
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

// The completion handler's error text, "" for a state made
std::string createdLater(id<MTLDevice> device, MTLRenderPipelineDescriptor *d)
{
    dispatch_semaphore_t answered = dispatch_semaphore_create(0);
    __block id<MTLRenderPipelineState> state = nil;
    __block NSError *error = nil;
    [device newRenderPipelineStateWithDescriptor:d
                               completionHandler:^(id<MTLRenderPipelineState> made, NSError *failed) {
                                   state = made;
                                   error = failed;
                                   dispatch_semaphore_signal(answered);
                               }];
    EXPECT_EQ(dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
    if (state)
        return "";
    return error ? text(error.localizedDescription) : "nil with no error";
}

TEST(RawFacadeCreation, ACompletionHandlersCreationFailsAsTheSynchronousOneDoes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        const std::string why = createdLater(device, unpacked(library));
        EXPECT_FALSE(why.empty());
        EXPECT_NE(why, "nil with no error");
    });
}

TEST(RawFacadeCreation, ACompletionHandlersCreationWithNoCompilerAtAllFailsByName)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(TegraGM20BCompilerCapable);
        ASSERT_TRUE(library != nil);
        const std::string why = createdLater(device, unpacked(library));
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

// The requests the client sent for a state of the most vertex amplification this kind answers YES for: the compiler keys
// each as the client did (boot 21 refused every amplified state's: its fields carried no amplification count)
TEST(RawFacadeCreation, TheCompilerKeysAnAmplifiedStatesRequestAsTheClientDoes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        MTLRenderPipelineDescriptor *d = unpacked(library);
        const uint64_t most = raw::answers::thisKindLastYes("supportsVertexAmplificationCount:");
        d.maxVertexAmplificationCount = most > 1 ? most : 1;
        created(device, d);
        ASSERT_FALSE(kernel_model::model.compileRequests.empty());
        for (const std::string &bytes : kernel_model::model.compileRequests) {
            raw::CompileRequest r;
            ASSERT_TRUE(raw::decodeRequest(bytes, r));
            EXPECT_EQ(r.fields.amplification, d.maxVertexAmplificationCount);
            switch_compiler::SystemLibraries system;
            switch_compiler::Prepared p;
            std::string why;
            EXPECT_TRUE(switch_compiler::prepare(bytes, r.stack, system, p, why)) << why;
            EXPECT_EQ(p.key.key, r.key);
            if (most > 1)
                EXPECT_NE(r.key.find(" amp=" + std::to_string(most) + " "), std::string::npos) << r.key;
        }
    });
}

}  // namespace
