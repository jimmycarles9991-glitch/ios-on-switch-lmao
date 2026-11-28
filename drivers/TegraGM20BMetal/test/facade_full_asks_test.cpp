// SPDX-License-Identifier: MIT
// How a device-made state's full form is asked (TegraGM20BMetalCompleteForms.mm): as its creation asks, every library's
// bytes the second time where the first answer left no record; again at the state's next creation where no compiler
// took the ask; and for a storage form's barrier twin once the twin is placed (RequestForm::BarrierFull). A second
// creation reads the full record compilerd filed under the state's plain content, and asks no full form.
#include "facade_full_form.h"
#include "bazel/testing/fresh_process.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using namespace harness;

using Forms = std::vector<gm20b::RequestForm>;
constexpr gm20b::RequestForm kPrimary = gm20b::RequestForm::Primary, kFull = gm20b::RequestForm::Full,
                             kBarrier = gm20b::RequestForm::Barrier, kBarrierFull = gm20b::RequestForm::BarrierFull;

void lateFullForm(const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record)
{
    residualUniform(record);
    if (request.form == kFull)
        addFullForm(record);
    else
        primaryForm(record);
}

void aFullFormNoCompilerTookIsAskedAgainAtTheStatesNextCreation()
{
    RenderCompiler compiler;
    auto taking = std::make_shared<std::atomic<bool>>(false);
    compiler.unavailable = [taking](const gm20b::CompileRequest &request) {
        return request.form == kFull && !taking->load();
    };
    compiler.shapes = lateFullForm;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = servedFull(compiler, "untaken", device);
    ASSERT_TRUE(library != nil);
    std::string why;
    id<MTLRenderPipelineState> first = createdState(device, library, false, why);
    gm20b::Pipeline *pl = rawOf(first);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), (Forms{kPrimary, kFull, kFull}));
    EXPECT_EQ(tableOf(pl), nullptr);
    taking->store(true);
    id<MTLRenderPipelineState> second = createdState(device, library, false, why);
    ASSERT_EQ(rawOf(second), pl) << why;
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), (Forms{kPrimary, kFull, kFull, kFull}));
    EXPECT_NE(tableOf(pl), nullptr);
    EXPECT_NE(splitOf(pl), nullptr);
}

// The first ask of each form answered made with nothing stored: the creation asks again with every library's bytes, and
// so does the full form's
void aFullFormIsAskedAgainWhereItsFirstAnswerLeftNoRecord()
{
    RenderCompiler compiler;
    auto asked = std::make_shared<std::atomic<uint32_t>>(0);
    compiler.misses = [asked](const gm20b::CompileRequest &request) {
        const uint32_t form = 1u << uint32_t(request.form);
        return !(asked->fetch_or(form) & form);
    };
    compiler.shapes = lateFullForm;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = madeFull(compiler, "missed", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), (Forms{kPrimary, kPrimary, kFull, kFull}));
    EXPECT_NE(tableOf(pl), nullptr);
    EXPECT_NE(splitOf(pl), nullptr);
}

gm20b::Pipeline *twinOf(gm20b::Pipeline *pl)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    gm20b::Pipeline *twin = nullptr;
    while (!(twin = __atomic_load_n(&pl->barrierTwin, __ATOMIC_ACQUIRE)) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return twin;
}

void aBarrierTwinGetsItsFullFormOncePlaced()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        if (request.form == kPrimary)
            return storageForm(record);
        record.pipeline.flags |= gm20b::PipelineBarrierForm;
        if (request.form == kBarrierFull)
            addFullForm(record);
        else
            primaryForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = madeFull(compiler, "twin-full", device, state, why, true);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_EQ(pl->storageBits, 1);
    gm20b::Pipeline *twin = twinOf(pl);
    ASSERT_NE(twin, nullptr);
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), (Forms{kPrimary, kBarrier, kBarrierFull}));
    ASSERT_NE(tableOf(twin), nullptr);
    EXPECT_NE(gRM->raw->variant(*twin, gm20b::AssumeUniformAligned), twin);
    EXPECT_NE(splitOf(twin), nullptr);
    EXPECT_EQ(tableOf(pl), nullptr);
}

void aBarrierTwinsFullFormNoCompilerTookIsAskedAgainAtTheStatesNextCreation()
{
    RenderCompiler compiler;
    auto taking = std::make_shared<std::atomic<bool>>(false);
    compiler.unavailable = [taking](const gm20b::CompileRequest &request) {
        return request.form == kBarrierFull && !taking->load();
    };
    compiler.shapes = [](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        if (request.form == kPrimary)
            return storageForm(record);
        record.pipeline.flags |= gm20b::PipelineBarrierForm;
        if (request.form == kBarrierFull)
            addFullForm(record);
        else
            primaryForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = servedFull(compiler, "twin-full-retry", device);
    ASSERT_TRUE(library != nil);
    std::string why;
    id<MTLRenderPipelineState> first = createdState(device, library, true, why);
    gm20b::Pipeline *pl = rawOf(first);
    ASSERT_NE(pl, nullptr) << why;
    gm20b::Pipeline *twin = twinOf(pl);
    ASSERT_NE(twin, nullptr);
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), (Forms{kPrimary, kBarrier, kBarrierFull, kBarrierFull}));
    EXPECT_EQ(tableOf(twin), nullptr);
    EXPECT_EQ(splitOf(twin), nullptr);
    taking->store(true);
    id<MTLRenderPipelineState> second = createdState(device, library, true, why);
    ASSERT_EQ(rawOf(second), pl) << why;
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), (Forms{kPrimary, kBarrier, kBarrierFull, kBarrierFull, kBarrierFull}));
    EXPECT_NE(tableOf(twin), nullptr);
    EXPECT_NE(splitOf(twin), nullptr);
}

// A creation in a client that never saw the state (a fresh Device and Completion, its own process cache) after the full
// form was made: its store lookup reads the full record compilerd filed under the plain content, which has the full
// form's own entry gone, so a full form asked would reach the compiler
void aSecondCreationReadsTheFullRecordFiledUnderItsPlainContent()
{
    RenderCompiler compiler;
    compiler.shapes = lateFullForm;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = servedFull(compiler, "re-headed", device);
    ASSERT_TRUE(library != nil);
    std::string why;
    id<MTLRenderPipelineState> first = createdState(device, library, false, why);
    ASSERT_NE(rawOf(first), nullptr) << why;
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    ASSERT_EQ(forms(compiler), (Forms{kPrimary, kFull}));
    compiler.retireFullForms();
    gRM->completion.reset();
    gm20b::Device fresh;
    std::string err;
    ASSERT_TRUE(fresh.open(writePack(packKey(MTLPixelFormatRGBA16Float)).c_str(), 0, err, gm20b::Device::Role::Client))
        << err;
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        ASSERT_FALSE(kernel_model::model.others.empty());
        auto &other = *kernel_model::model.others.rbegin()->second;
        other.compilerFlags = kernel_model::model.compilerFlags;
        other.store = kernel_model::model.store;
        other.storeBytes = kernel_model::model.storeBytes;
        other.storeGeneration = kernel_model::model.storeGeneration;
        other.compiler = kernel_model::model.compiler;
    }
    const std::string cache = dir() + "/re-headed-second-" + std::to_string(getpid());
    mkdir(cache.c_str(), 0700);
    gm20b::Completion second(fresh, cache.c_str(), 0);
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    gm20b::Completion::Ask ask;
    ask.descriptor = ask.facade = d;
    ask.resolveDescriptor = true;
    ask.key = metal_pipeline_key(d).UTF8String;
    gm20b::Pipeline *pl2 = second.makeNow(ask, why);
    ASSERT_NE(pl2, nullptr) << why;
    EXPECT_NE(tableOf(pl2), nullptr);
    EXPECT_NE(splitOf(pl2), nullptr);
    ASSERT_TRUE(second.settleTwins(uint64_t(5) * NSEC_PER_SEC));
    EXPECT_EQ(forms(compiler), (Forms{kPrimary, kFull}));
}

}  // namespace

TEST(GM20BFacadeFullAsks, AFullFormNoCompilerTookIsAskedAgainAtTheStatesNextCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(aFullFormNoCompilerTookIsAskedAgainAtTheStatesNextCreation);
}

TEST(GM20BFacadeFullAsks, AFullFormIsAskedAgainWhereItsFirstAnswerLeftNoRecord)
{
    switch_ios::test::ExpectPassesInFreshProcess(aFullFormIsAskedAgainWhereItsFirstAnswerLeftNoRecord);
}

TEST(GM20BFacadeFullAsks, ABarrierTwinGetsItsFullFormOncePlaced)
{
    switch_ios::test::ExpectPassesInFreshProcess(aBarrierTwinGetsItsFullFormOncePlaced);
}

TEST(GM20BFacadeFullAsks, ABarrierTwinsFullFormNoCompilerTookIsAskedAgainAtTheStatesNextCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(
        aBarrierTwinsFullFormNoCompilerTookIsAskedAgainAtTheStatesNextCreation);
}

TEST(GM20BFacadeFullAsks, ASecondCreationReadsTheFullRecordFiledUnderItsPlainContent)
{
    switch_ios::test::ExpectPassesInFreshProcess(aSecondCreationReadsTheFullRecordFiledUnderItsPlainContent);
}
