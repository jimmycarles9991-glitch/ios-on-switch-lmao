// SPDX-License-Identifier: MIT
// A render state the pack lacks, created on the facade over the kernel model with the shader compiler broker's answers
// set by each test: where the compiler makes no record the creation fails with an NSError as Metal's does, never handing
// back a state whose draws go elsewhere, and the client compiles nothing itself, with no compiler at all too. The m2v stand-in makes each library's twin
// of the same metallib (facade_compute.h), so the facade keys the state as on the device.
#include "facade_compute.h"
#include "../archive.h"
#include "../ca_catch_all.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../compile_request.h"
#include "request_store_key.h"
#include "../render_key.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "bazel/testing/fresh_process.h"
#include "../compiler/compiler_request.h"
#include "../compile_request.h"
#include "../device_answers.h"
#include "bazel/testing/source_text.h"

#include <gtest/gtest.h>
#include <filesystem>

// Another device's vertex function, as a render state's creation reads one
@interface ForeignVertexFunction : NSObject
@end
@implementation ForeignVertexFunction

- (NSString *)name
{
    return @"v";
}

- (MTLFunctionType)functionType
{
    return MTLFunctionTypeVertex;
}

- (MTLPatchType)patchType
{
    return MTLPatchTypeNone;
}

@end

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
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    if (!device)
        return {};
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
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

TEST(GM20BFacadeCreation, NoCompilerComingFailsTheCreationWithNoAsk)
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
TEST(GM20BFacadeCreation, NoCompilerAtAllFailsTheCreationByNameWithNoCompileHere)
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
TEST(GM20BFacadeCreation, DebugAndReleaseClientsCompleteStatesAlike)
{
    for (const char *file : {"../TegraGM20BMetalComplete.h", "../TegraGM20BMetalComplete.mm",
                             "../TegraGM20BMetalCompleteForms.mm", "../TegraGM20BMetalCompleteRemote.mm"}) {
        const std::string source = switch_ios::test::SourceText(file);
        EXPECT_EQ(switch_ios::test::Configured(source, "SWITCH_DEBUG", true),
                  switch_ios::test::Configured(source, "SWITCH_DEBUG", false))
            << file;
    }
}

TEST(GM20BFacadeCreation, ClientRuntimeUsesNoThreadCpuClockOrThreadInfo)
{
    size_t checked = 0;
    for (const char *dir : {"..", "../plugin"}) {
        const std::filesystem::path root = switch_ios::test::PackagePath(dir);
        ASSERT_TRUE(std::filesystem::is_directory(root)) << root;
        for (const auto &entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_regular_file())
                continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".mm" && ext != ".cpp" && ext != ".h")
                continue;
            const std::string rel = std::string(dir) + "/" + entry.path().filename().string();
            const std::string source = switch_ios::test::SourceText(rel);
            ++checked;
            for (const char *forbidden :
                 {"CLOCK_THREAD_CPUTIME_ID", "CLOCK_PROCESS_CPUTIME_ID", "thread_selfusage", "thread_info("}) {
                EXPECT_TRUE(switch_ios::test::Occurrences(source, forbidden).empty()) << rel << " uses " << forbidden;
            }
        }
    }
    EXPECT_GT(checked, 0u);
}

TEST(GM20BFacadeCreation, ARequestTheCompilerDoesNotTakeIsAskedOnceMoreThenFails)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler did not take the state"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 2u);
    });
}

TEST(GM20BFacadeCreation, ARecordTheStoreDoesNotKeepIsAskedOnceMoreThenFails)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileMade});
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler's store does not keep the state"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 2u);
    });
}

TEST(GM20BFacadeCreation, ACompileThatCouldNotRunFailsTheCreationAfterOneRetry)
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
TEST(GM20BFacadeCreation, AnotherDevicesLinkedFunctionFailsTheCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileMade});
        ASSERT_TRUE(library != nil);
        MTLRenderPipelineDescriptor *d = unpacked(library);
        HarnessFunction *foreign = [HarnessFunction new];
        foreign->_name = @"f";
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

TEST(GM20BFacadeCreation, ACompletionHandlersCreationFailsAsTheSynchronousOneDoes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        const std::string why = createdLater(device, unpacked(library));
        EXPECT_FALSE(why.empty());
        EXPECT_NE(why, "nil with no error");
    });
}

TEST(GM20BFacadeCreation, ACompletionHandlersCreationWithNoCompilerAtAllFailsByName)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(TegraGM20BCompilerCapable);
        ASSERT_TRUE(library != nil);
        const std::string why = createdLater(device, unpacked(library));
        EXPECT_NE(why.find("no shader compiler comes this boot"), std::string::npos) << why;
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

// A state of another device's vertex function, or of none, is Metal's validation error: nil with the library's error,
// synchronously and through a completion handler, with nothing asked of the compiler
TEST(GM20BFacadeCreation, AStateOfAnotherDevicesFunctionOrNoneFailsTheCreationWithAnError)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileMade});
        ASSERT_TRUE(library != nil);
        MTLRenderPipelineDescriptor *foreign = unpacked(library);
        foreign.vertexFunction = (id<MTLFunction>)[ForeignVertexFunction new];
        MTLRenderPipelineDescriptor *none = unpacked(library);
        none.vertexFunction = nil;
        for (MTLRenderPipelineDescriptor *d : {foreign, none}) {
            NSError *error = nil;
            EXPECT_EQ([device newRenderPipelineStateWithDescriptor:d error:&error], nil);
            EXPECT_EQ(text(error.domain), text(MTLLibraryErrorDomain));
            EXPECT_EQ(error.code, MTLLibraryErrorInternal);
            EXPECT_NE(text(error.localizedDescription).find("not made by this device"), std::string::npos)
                << text(error);
            const std::string later = createdLater(device, d);
            EXPECT_NE(later.find("not made by this device"), std::string::npos) << later;
        }
        EXPECT_EQ(kernel_model::model.compiles, 0u);
    });
}

// The requests the client sent for a state of the most vertex amplification this kind answers YES for: the compiler keys
// each as the client did (boot 21 refused every amplified state's: its fields carried no amplification count)
TEST(GM20BFacadeCreation, TheCompilerKeysAnAmplifiedStatesRequestAsTheClientDoes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const auto [device, library] = opened(kServing, {TegraGM20BCompileUnavailable});
        ASSERT_TRUE(library != nil);
        MTLRenderPipelineDescriptor *d = unpacked(library);
        const uint64_t most = gm20b::answers::thisKindLastYes("supportsVertexAmplificationCount:");
        d.maxVertexAmplificationCount = most > 1 ? most : 1;
        created(device, d);
        ASSERT_FALSE(kernel_model::model.compileRequests.empty());
        for (const std::string &bytes : kernel_model::model.compileRequests) {
            gm20b::CompileRequest r;
            ASSERT_TRUE(gm20b::decodeRequest(bytes, r));
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

// The shader compiler refusing every render state it is asked for, its refusal kept in its store as SwitchShaderCompiler
// keeps one, under the name the request's own key and libraries give
struct RefusingCompiler {
    switch_seed_pack::StoreWriter store;

    bool serve()
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0x55;
        std::string why;
        if (!store.open(dir() + "/store", identity, 64, 1u << 20, why))
            return false;
        kernel_model::model.compilerFlags = kServing | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            gm20b::CompileRequest r;
            if (!gm20b::decodeRequest(bytes, r) || r.kind != gm20b::RequestKind::Render || r.stages.size() < 2)
                return int32_t(TegraGM20BCompileUnavailable);
            std::map<std::string, gm20b::AirHash> hashes;
            const std::string &library = r.libraries[r.stages[0].library].bytes;
            if (!gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(library.data()), library.size(), hashes))
                return int32_t(TegraGM20BCompileUnavailable);
            bool own = false;
            const std::string packKey = gm20b::requestPackKey(r, own);
            const gm20b::CacheKey k = gm20b::renderCacheKey(r.stack, packKey, hashes[r.stages[0].function],
                                                            hashes[r.stages[1].function], {}, own);
            const gm20b::StoreKey key = harness::requestStoreKey(k, r);
            const std::string entry = gm20b::encodeEntry(key.content, nullptr, "a state this compiler refuses");
            store.put(key.name.data(), reinterpret_cast<const uint8_t *>(entry.data()), uint32_t(entry.size()));
            return int32_t(TegraGM20BCompileRefused);
        };
        return true;
    }

    // last: goes first, before what the compiler hook captures, which the device may still call
    kernel_model::HooksCleared cleared;
};

// A state the shader compiler refused fails its creation with the refusal, as Metal's does for a state it could not
// compile, synchronously and through a completion handler; it is never m2v's
TEST(GM20BFacadeCreation, AStateTheCompilerRefusedFailsTheCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        RefusingCompiler compiler;
        ASSERT_TRUE(compiler.serve());
        const auto [device, library] = opened(kernel_model::model.compilerFlags);
        ASSERT_TRUE(library != nil);
        const std::string why = created(device, unpacked(library));
        EXPECT_NE(why.find("the shader compiler refused the state: a state this compiler refuses"), std::string::npos)
            << why;
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block id<MTLRenderPipelineState> state = nil;
        __block NSError *error = nil;
        [device newRenderPipelineStateWithDescriptor:unpacked(library)
                                   completionHandler:^(id<MTLRenderPipelineState> made, NSError *failed) {
                                       state = made;
                                       error = failed;
                                       dispatch_semaphore_signal(answered);
                                   }];
        ASSERT_EQ(dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
        EXPECT_EQ(state, nil);
        EXPECT_NE(text(error.localizedDescription).find("the shader compiler refused the state"), std::string::npos)
            << text(error);
    });
}

std::string packArchivesSection(const std::string &path, std::vector<uint64_t> keys)
{
    std::sort(keys.begin(), keys.end());
    const std::string canonical = gm20b::inventoryPath(path);
    const uint32_t counts[2] = {1, uint32_t(keys.size())};
    const gm20b::PackArchive entry = {gm20b::Pack::fnv(canonical.data(), canonical.size()), 0, uint32_t(keys.size())};
    std::string out(sizeof counts + sizeof entry + keys.size() * sizeof(uint64_t), '\0');
    memcpy(out.data(), counts, sizeof counts);
    memcpy(out.data() + sizeof counts, &entry, sizeof entry);
    if (!keys.empty())
        memcpy(out.data() + sizeof counts + sizeof entry, keys.data(), keys.size() * sizeof(uint64_t));
    return out;
}

TEST(GM20BFacadeCreation, FailOnBinaryArchiveMissSkipsCompileForExcludedArchiveState)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        const std::string archivePath = dir() + "/archive.metallib";
        const uint8_t header[8] = {'M', 'T', 'L', 'B', 1, 0, 2, 0};
        std::ofstream(archivePath, std::ios::binary).write(reinterpret_cast<const char *>(header), sizeof header);
        const std::string declaredKey = packKey(MTLPixelFormatRGBA8Unorm);
        const std::string section =
            packArchivesSection(archivePath, {gm20b::Pack::fnv(declaredKey.data(), declaredKey.size())});
        kernel_model::model.compilerFlags = kServing;
        kernel_model::model.compileResults = {TegraGM20BCompileUnavailable};
        id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, {}, section));
        ASSERT_TRUE(device != nil);
        const std::string cache = dir() + "/cache";
        mkdir(cache.c_str(), 0700);
        metal_complete_states(cache.c_str(), nullptr);
        NSError *error = nil;
        id<MTLLibrary> library =
            [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:&error];
        ASSERT_TRUE(library != nil);
        MTLBinaryArchiveDescriptor *ad = [MTLBinaryArchiveDescriptor new];
        ad.url = [NSURL fileURLWithPath:@(archivePath.c_str())];
        id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:ad error:&error];
        ASSERT_TRUE(archive != nil) << text(error);

        MTLRenderPipelineDescriptor *excluded = descriptor(library, MTLPixelFormatRGBA16Float);
        excluded.binaryArchives = @[ archive ];
        id<MTLRenderPipelineState> missing =
            [device newRenderPipelineStateWithDescriptor:excluded
                                                 options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                              reflection:nil
                                                   error:&error];
        EXPECT_EQ(missing, nil);
        ASSERT_TRUE(error != nil);
        EXPECT_EQ(error.code, 3);
        EXPECT_EQ(kernel_model::model.compiles, 0u);

        MTLRenderPipelineDescriptor *declared = descriptor(library, MTLPixelFormatRGBA8Unorm);
        declared.binaryArchives = @[ archive ];
        error = nil;
        EXPECT_EQ([device newRenderPipelineStateWithDescriptor:declared
                                                       options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                    reflection:nil
                                                         error:&error],
                  nil);
        EXPECT_GT(kernel_model::model.compiles, 0u);
    });
}

TEST(GM20BFacadeCreation, StandInTargetStateHonorsDeviceArchiveAndArchiveMiss)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        kernel_model::model.compilerFlags = kServing;
        kernel_model::model.compileResults = {TegraGM20BCompileUnavailable};
        id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatRGBA8Unorm_sRGB)));
        ASSERT_TRUE(device != nil);
        const std::string cache = dir() + "/cache";
        mkdir(cache.c_str(), 0700);
        metal_complete_states(cache.c_str(), nullptr);
        NSError *error = nil;
        id<MTLLibrary> frameworkLib =
            [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:&error];
        ASSERT_TRUE(frameworkLib != nil);
        const std::string bytes = made_metallib::universal({"v", "f"}, 0x61, 0xEE, {0, 1});
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> appLib = [device newLibraryWithData:data error:&error];
        ASSERT_TRUE(appLib != nil);

        id<MTLBinaryArchive> empty =
            [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
        ASSERT_TRUE(empty != nil);

        MTLRenderPipelineDescriptor *packedStandIn = descriptor(frameworkLib, MTLPixelFormatR8Unorm_sRGB);
        packedStandIn.binaryArchives = @[ empty ];
        id<MTLRenderPipelineState> hit =
            [device newRenderPipelineStateWithDescriptor:packedStandIn
                                                 options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                              reflection:nil
                                                   error:&error];
        ASSERT_TRUE(hit != nil) << text(error);
        EXPECT_TRUE(((SwitchMetalPipeline *)hit)->_packed);
        EXPECT_EQ(error, nil);
        EXPECT_EQ(kernel_model::model.compiles, 0u);

        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block id<MTLRenderPipelineState> asyncHit = nil;
        __block NSError *asyncError = nil;
        [device newRenderPipelineStateWithDescriptor:packedStandIn
                                             options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                   completionHandler:^(id<MTLRenderPipelineState> made, MTLRenderPipelineReflection *,
                                                       NSError *failed) {
                                       asyncHit = made;
                                       asyncError = failed;
                                       dispatch_semaphore_signal(answered);
                                   }];
        ASSERT_EQ(dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
        EXPECT_TRUE(asyncHit != nil) << text(asyncError);
        EXPECT_EQ(asyncError, nil);
        EXPECT_EQ(kernel_model::model.compiles, 0u);

        MTLRenderPipelineDescriptor *unpackedStandIn = descriptor(frameworkLib, MTLPixelFormatA1BGR5Unorm);
        unpackedStandIn.binaryArchives = @[ empty ];
        id<MTLRenderPipelineState> missed =
            [device newRenderPipelineStateWithDescriptor:unpackedStandIn
                                                 options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                              reflection:nil
                                                   error:&error];
        EXPECT_EQ(missed, nil);
        ASSERT_TRUE(error != nil);
        EXPECT_EQ(error.code, 3);
        EXPECT_EQ(kernel_model::model.compiles, 0u);

        id<MTLBinaryArchive> freshEmpty =
            [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
        ASSERT_TRUE(freshEmpty != nil);
        MTLRenderPipelineDescriptor *appStandIn = descriptor(appLib, MTLPixelFormatR8Unorm_sRGB);
        appStandIn.binaryArchives = @[ freshEmpty ];
        error = nil;
        EXPECT_EQ([device newRenderPipelineStateWithDescriptor:appStandIn
                                                       options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                    reflection:nil
                                                         error:&error],
                  nil);
        ASSERT_TRUE(error != nil);
        EXPECT_EQ(error.code, 3);
        EXPECT_EQ(kernel_model::model.compiles, 0u);

        error = nil;
        ASSERT_TRUE([empty addRenderPipelineFunctionsWithDescriptor:unpackedStandIn error:&error]) << text(error);
        EXPECT_EQ([device newRenderPipelineStateWithDescriptor:unpackedStandIn
                                                       options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                    reflection:nil
                                                         error:&error],
                  nil);
        EXPECT_GT(kernel_model::model.compiles, 0u);
    });
}

TEST(GM20BFacadeCreation, AnInProcessCoreAnimationMissLogsOncePerLabel)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        RefusingCompiler compiler;
        ASSERT_TRUE(compiler.serve());
        const auto [device, library] = opened(kernel_model::model.compilerFlags);
        ASSERT_TRUE(library != nil);

        MTLRenderPipelineDescriptor *first = unpacked(library);
        first.label = @"com.apple.coreanimation.PBGRAXm_TimgA2Xhf_Isrc";
        EXPECT_NE(created(device, first).find("refused"), std::string::npos);
        EXPECT_EQ(gm20b::gInProcessCaLogged.load(std::memory_order_relaxed), 0u);

        EXPECT_NE(created(device, first).find("refused"), std::string::npos);
        EXPECT_EQ(gm20b::gInProcessCaLogged.load(std::memory_order_relaxed), 1u);

        EXPECT_NE(created(device, first).find("refused"), std::string::npos);
        EXPECT_EQ(gm20b::gInProcessCaLogged.load(std::memory_order_relaxed), 1u);

        MTLRenderPipelineDescriptor *plain = unpacked(library);
        plain.label = @"not.coreanimation";
        EXPECT_NE(created(device, plain).find("refused"), std::string::npos);
        EXPECT_EQ(gm20b::gInProcessCaLogged.load(std::memory_order_relaxed), 1u);

        MTLRenderPipelineDescriptor *second = unpacked(library);
        second.label = @"com.apple.coreanimation.PBGRAXmw_TimgA3Xhf_Isrc";
        EXPECT_NE(created(device, second).find("refused"), std::string::npos);
        EXPECT_EQ(gm20b::gInProcessCaLogged.load(std::memory_order_relaxed), 2u);

        MTLRenderPipelineDescriptor *asyncDesc = unpacked(library);
        asyncDesc.label = @"com.apple.coreanimation.PBGRAXmw_TimgA3Xhfp_Isrc";
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        [device newRenderPipelineStateWithDescriptor:asyncDesc
                                   completionHandler:^(id<MTLRenderPipelineState>, NSError *) {
                                       dispatch_semaphore_signal(answered);
                                   }];
        ASSERT_EQ(dispatch_semaphore_wait(answered, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))), 0);
        EXPECT_EQ(gm20b::gInProcessCaLogged.load(std::memory_order_relaxed), 3u);
    });
}

}  // namespace
