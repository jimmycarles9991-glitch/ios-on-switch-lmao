// SPDX-License-Identifier: MIT
// A storage form the shader compiler made gets its barrier twin as a pack state gets its own (Device::pipeline): asked in
// RequestForm::Barrier once its record is placed, made apart from it in the store, and attached to its pipeline so rule
// fetch.twin can take its draws. A twin that is itself a storage form is never attached.
#include "facade_variant_compiler.h"
#include "bazel/testing/fresh_process.h"

#include <chrono>
#include <thread>

namespace {

using namespace harness;

id<MTLLibrary> served(RenderCompiler &compiler, const std::string &name, id<MTLDevice> __strong &device)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory))
        return nil;
    NSString *why = nil;
    // a pack of another state: the test's BGRA8 state is one the pack lacks, made by `compiler`
    device = metal_device_open(writePack(packKey(MTLPixelFormatRGBA16Float)).c_str(), 0, 0, &why);
    if (!device)
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
}

void storageForm(gm20b::RuntimeRecord &record)
{
    gm20b::PackPipeline &pp = record.pipeline;
    pp.flags |= gm20b::PipelineStorageMetadata;
    pp.storageMask = pp.storageWriteMask = 1;
    pp.storageSamples = 1;
    pp.storageTic[0] = gm20b::formatInfo(MTLPixelFormatBGRA8Unorm)->tic0;
    record.blob = {0x1224, 1, 0x1a00, 0};
}

gm20b::Pipeline *twinOf(gm20b::Pipeline *pl)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    gm20b::Pipeline *twin = nullptr;
    while (!(twin = __atomic_load_n(&pl->barrierTwin, __ATOMIC_ACQUIRE)) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return twin;
}

// The pipeline of a state the pack lacks, made by `compiler`, of a depth attachment as the test compiler's storage forms
// need (facade_variant_compiler.h: its records fetch depth then); `device` and `state` keep it alive, `why` says why none
gm20b::Pipeline *made(RenderCompiler &compiler, const char *name, id<MTLDevice> __strong &device,
                      id<MTLRenderPipelineState> __strong &state, std::string &why)
{
    id<MTLLibrary> library = served(compiler, name, device);
    if (!library)
        return why = "no device or library", nullptr;
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *error = nil;
    state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!state)
        return why = error.localizedDescription.UTF8String ?: "no state", nullptr;
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    return pipeline->_raw;
}

void madeStorageFormGetsItsTwin()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        if (request.form == gm20b::RequestForm::Barrier)
            record.pipeline.flags |= gm20b::PipelineBarrierForm;
        else
            storageForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = made(compiler, "made-twin", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_EQ(pl->storageBits, 1);
    ASSERT_TRUE(compiler.wait(2));
    const std::vector<gm20b::CompileRequest> asked = compiler.captured();
    EXPECT_EQ(asked[0].form, gm20b::RequestForm::Primary);
    EXPECT_EQ(asked[1].form, gm20b::RequestForm::Barrier);
    EXPECT_EQ(asked[1].key, asked[0].key);
    gm20b::Pipeline *twin = twinOf(pl);
    ASSERT_NE(twin, nullptr);
    EXPECT_NE(twin, pl);
    EXPECT_EQ(twin->storageBits, 0);
    EXPECT_NE(twin->rec->keyHash, pl->rec->keyHash);
    EXPECT_EQ(__atomic_load_n(&pl->blendTwin, __ATOMIC_ACQUIRE), nullptr);
}

void aTwinThatIsAStorageFormIsNotAttached()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &, gm20b::RuntimeRecord &record) {
        storageForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = made(compiler, "storage-twin", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_TRUE(compiler.wait(2));
    EXPECT_EQ(twinOf(pl), nullptr);
}

void aStateOfNoStorageFormAsksNoTwin()
{
    RenderCompiler compiler;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
    std::string why;
    gm20b::Pipeline *pl = made(compiler, "plain", device, state, why);
    ASSERT_NE(pl, nullptr) << why;
    ASSERT_EQ(pl->storageBits, 0);
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(2) * NSEC_PER_SEC));
    for (const gm20b::CompileRequest &r : compiler.captured())
        EXPECT_NE(r.form, gm20b::RequestForm::Barrier);
}

// An app that changes its descriptor for its next state right after a creation (Metal's usual reuse) still gets each
// state's own twin: the twin is asked with the descriptor as the state was made, not as the app left it
void aReusedDescriptorAsksEachStatesOwnTwin()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        if (request.form == gm20b::RequestForm::Barrier)
            record.pipeline.flags |= gm20b::PipelineBarrierForm;
        else
            storageForm(record);
    };
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "reused-descriptor", device);
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *error = nil;
    id<MTLRenderPipelineState> first = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(first != nil) << (error.localizedDescription.UTF8String ?: "");
    d.colorAttachments[0].blendingEnabled = YES;
    id<MTLRenderPipelineState> second = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(second != nil) << (error.localizedDescription.UTF8String ?: "");
    ASSERT_TRUE(gRM->completion->settleTwins(uint64_t(5) * NSEC_PER_SEC));
    uint32_t plain = 0, blended = 0;
    for (const gm20b::CompileRequest &r : compiler.captured())
        if (r.form == gm20b::RequestForm::Barrier)
            (r.fields.color[0].blending ? blended : plain)++;
    EXPECT_EQ(plain, 1u);
    EXPECT_EQ(blended, 1u);
    __unsafe_unretained SwitchMetalPipeline *a = (SwitchMetalPipeline *)first, *b = (SwitchMetalPipeline *)second;
    gm20b::Pipeline *twinA = twinOf(a->_raw), *twinB = twinOf(b->_raw);
    ASSERT_NE(twinA, nullptr);
    ASSERT_NE(twinB, nullptr);
    EXPECT_NE(twinA, twinB);
}

}  // namespace

TEST(GM20BFacadeMadeTwin, AReusedDescriptorAsksEachStatesOwnTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(aReusedDescriptorAsksEachStatesOwnTwin);
}

TEST(GM20BFacadeMadeTwin, AMadeStorageFormGetsItsBarrierTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(madeStorageFormGetsItsTwin);
}

TEST(GM20BFacadeMadeTwin, ATwinThatIsAStorageFormIsNotAttached)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTwinThatIsAStorageFormIsNotAttached);
}

TEST(GM20BFacadeMadeTwin, AStateOfNoStorageFormAsksNoTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(aStateOfNoStorageFormAsksNoTwin);
}
