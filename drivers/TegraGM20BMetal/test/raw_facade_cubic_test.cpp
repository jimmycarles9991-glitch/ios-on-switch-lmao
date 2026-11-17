// SPDX-License-Identifier: MIT
#include "raw_facade_harness.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/raw_compile_request.h"
#include "../raw/runtime/raw_fragment_tests.h"
#include "../raw/runtime/raw_mme.h"
#include "../raw/runtime/raw_render_key.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "bazel/testing/fresh_process.h"
#include <IOKit/OSMessageNotification.h>
#import <IOSurface/IOSurface.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <unistd.h>

extern "C" {
struct record_count {
    uint64_t sends, locks, mallocs, frees, weaks, retains, dispatches;
};

void record_count_start(void);
void record_count_stop(struct record_count *out);
}

extern "C" kern_return_t IOConnectCallAsyncStructMethod(mach_port_t connection, uint32_t method, mach_port_t port,
                                                        uint64_t *reference, uint32_t referenceCount, const void *input,
                                                        size_t inputBytes, void *, size_t *)
{
    if (method != TegraGM20BMethodCompile || inputBytes != sizeof(TegraGM20BAbiCompile) ||
        referenceCount != kOSAsyncRef64Count)
        return kIOReturnUnsupported;
    TegraGM20BAbiCompileReply reply = {};
    size_t replyBytes = sizeof reply;
    const kern_return_t result = IOConnectCallStructMethod(connection, method, input, inputBytes, &reply, &replyBytes);
    if (result != KERN_SUCCESS)
        return result;
    const auto &request = *static_cast<const TegraGM20BAbiCompile *>(input);
    const uint64_t words[TegraGM20BCompileAnswerWords] = {request.cookie, uint64_t(uint32_t(reply.result)) |
                                                                              uint64_t(reply.generation) << 32};
    alignas(8)
        uint8_t bytes[sizeof(mach_msg_header_t) + sizeof(OSNotificationHeader64) + sizeof result + sizeof words] = {};
    auto *header = reinterpret_cast<mach_msg_header_t *>(bytes);
    auto *notification = reinterpret_cast<OSNotificationHeader64 *>(bytes + sizeof *header);
    uint8_t *content = bytes + sizeof *header + sizeof *notification;
    header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    header->msgh_size = sizeof bytes;
    header->msgh_remote_port = port;
    header->msgh_id = kOSNotificationMessageID;
    notification->size = sizeof result + sizeof words;
    notification->type = kIOAsyncCompletionNotificationType;
    memcpy(notification->reference, reference, sizeof notification->reference);
    memcpy(content, &result, sizeof result);
    memcpy(content + sizeof result, words, sizeof words);
    return mach_msg(header, MACH_SEND_MSG, sizeof bytes, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

namespace {

using namespace harness;

struct Compiler {
    switch_seed_pack::StoreWriter store;
    std::mutex mutex;
    std::condition_variable answered;
    std::vector<raw::CompileRequest> requests;

    bool serve(const std::string &directory)
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0xc3;
        std::string why;
        if (!store.open(directory + "/store", identity, 128, 1u << 22, why))
            return false;
        kernel_model::model.compilerFlags = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected |
                                            TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            raw::CompileRequest request;
            if (!raw::decodeRequest(bytes, request) || request.kind != raw::RequestKind::Render ||
                request.stages.size() < 2)
                return int32_t(TegraGM20BCompileUnavailable);
            std::map<std::string, raw::AirHash> hashes;
            const std::string &library = request.libraries[request.stages[0].library].bytes;
            if (!raw::metallibAirHashes(reinterpret_cast<const uint8_t *>(library.data()), library.size(), hashes))
                return int32_t(TegraGM20BCompileUnavailable);
            bool own = false;
            const std::string key = raw::requestPackKey(request, own);
            const raw::CacheKey cache = raw::renderCacheKey(request.stack, key, hashes[request.stages[0].function],
                                                            hashes[request.stages[1].function], {}, own);
            raw::RuntimeRecord record;
            record.keyString = key;
            record.pipeline.keyHash = raw::Pack::fnv(key.data(), key.size());
            record.pipeline.vertexSlot = 0xff;
            record.pipeline.setBytes = 32;
            record.pipeline.memoryEntries = 3;
            raw::setEntryCount(record.pipeline, 2);
            record.pipeline.entry[0] = {0, raw::EntryTexture, 1, 0, raw::SamplerSlot, 0, 0};
            record.pipeline.entry[1] = {16, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0};
            if (request.fields.depthFormat) {
                raw::setEntryCount(record.pipeline, 3);
                record.pipeline.entry[2] = {32, raw::EntryFetch, 2, 0, raw::SamplerNone, 0, 0};
                record.pipeline.setBytes = 48;
                record.pipeline.memoryEntries = 7;
            }
            record.pipeline.flags = request.variant.twoPlane ? raw::PipelineTwoPlane : 0;
            for (auto &range : record.pipeline.uniform)
                range.entry = raw::kNoUniform;
            for (uint32_t stage = 0; stage < 2; stage++) {
                auto &program = record.programs[stage];
                program.image.assign(144, 0);
                program.image[80] = uint8_t(stage + 1);
                for (const auto &sampler : request.variant.cubic.samplers)
                    if (sampler.stage == stage + 1)
                        program.image[80] += 8;
                for (const auto &texture : request.variant.cubic.textures)
                    if (texture.stage == stage + 1)
                        program.image[81] = uint8_t(texture.format);
                program.image[82] = request.variant.fragmentTests;
                program.image[83] = request.variant.twoPlane;
                program.id = std::to_string(raw::Pack::fnv(program.image.data(), program.image.size()));
                program.gprs = 8;
                program.cbufTypes = {raw::CbufRoot, raw::CbufSet};
            }
            record.blob = {0x1a00, 0x1111};
            record.splitTried = true;
            const std::string entry = raw::encodeEntry(cache, &record, "");
            std::vector<raw::LibraryIdentity> ids;
            for (const auto &source : request.libraries)
                ids.push_back(raw::identityOf(source));
            {
                std::lock_guard<std::mutex> lock(mutex);
                store.put(raw::storeName(cache, request, ids).data(), reinterpret_cast<const uint8_t *>(entry.data()),
                          uint32_t(entry.size()));
                requests.push_back(std::move(request));
            }
            answered.notify_all();
            return int32_t(TegraGM20BCompileMade);
        };
        return true;
    }

    bool wait(size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return answered.wait_for(lock, std::chrono::seconds(2), [&] {
            return requests.size() >= count;
        });
    }

    std::vector<raw::CompileRequest> captured()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return requests;
    }
};

bool ready(raw::Variant<raw::Pipeline> &variant)
{
    auto signal = std::make_shared<std::promise<void>>();
    auto future = signal->get_future();
    const auto state = variant.observe([signal] {
        signal->set_value();
    });
    if (state == raw::Variant<raw::Pipeline>::State::Pending &&
        future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
        return false;
    return variant.state() == raw::Variant<raw::Pipeline>::State::Ready;
}

uint32_t program(const std::vector<uint32_t> &words, uint32_t stage)
{
    std::vector<switch_gm20b_method> methods(words.size());
    const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    EXPECT_GE(count, 0);
    uint32_t result = UINT32_MAX;
    for (long i = 0; i < count; i++)
        if (methods[i].subchannel == 0 && methods[i].method == NVB197_SET_PIPELINE_PROGRAM(stage == 1 ? 1 : 5))
            result = methods[i].value;
    return result;
}

std::vector<uint32_t> drawnPrograms(size_t firstJob, uint32_t stage)
{
    std::vector<uint32_t> result;
    uint32_t current = UINT32_MAX;
    const uint32_t first = NVB197_CALL_MME_MACRO(0), last = NVB197_CALL_MME_MACRO(127);
    for (size_t job = 0; job < kernel_model::model.jobs.size(); job++) {
        const auto &words = kernel_model::model.jobs[job].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        EXPECT_GE(count, 0);
        for (long i = 0; i < count; i++) {
            const auto &method = methods[i];
            if (method.subchannel != 0)
                continue;
            if (method.method == NVB197_SET_PIPELINE_PROGRAM(stage == 1 ? 1 : 5))
                current = method.value;
            const bool macro = method.method >= first && method.method <= last && (method.method - first) % 8 == 0 &&
                               raw::mme::countedAsDraw((method.method - first) / 8);
            if (job >= firstJob && (macro || method.method == NVB197_BEGIN))
                result.push_back(current);
        }
    }
    return result;
}

id<MTLTexture> image(id<MTLDevice> device, MTLTextureUsage usage, MTLPixelFormat format = MTLPixelFormatBGRA8Unorm)
{
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = usage;
    return [device newTextureWithDescriptor:descriptor];
}

void sampler(id<MTLRenderCommandEncoder> encoder, uint32_t stage, id<MTLSamplerState> state, NSUInteger index = 0)
{
    if (stage == 1)
        [encoder setVertexSamplerState:state atIndex:index];
    else
        [encoder setFragmentSamplerState:state atIndex:index];
}

void texture(id<MTLRenderCommandEncoder> encoder, uint32_t stage, id<MTLTexture> value)
{
    if (stage == 1)
        [encoder setVertexTexture:value atIndex:0];
    else
        [encoder setFragmentTexture:value atIndex:0];
}

void preparedCubic(uint32_t stage, bool pipelineFirst)
{
    const std::string directory = dir() + "/cubic-" + std::to_string(stage) +
                                  (pipelineFirst ? "-pipeline-" : "-bindings-") + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    Compiler compiler;
    ASSERT_TRUE(compiler.serve(directory));
    const std::vector<raw::PackEntry> entries = {{0, raw::EntryTexture, 1, 0, raw::SamplerSlot, 0, 0},
                                                 {16, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0}};
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, entries);
    NSString *why = nil;
    id<MTLDevice> device = raw_metal_device_open(pack.c_str(), 0, nil, &why);
    ASSERT_TRUE(device != nil) << (why ? why.UTF8String : "");
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    raw_metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil);
    id<MTLTexture> firstTexture = image(device, MTLTextureUsageShaderRead);
    id<MTLTexture> sameFormat = image(device, MTLTextureUsageShaderRead);
    ASSERT_TRUE(firstTexture != nil && sameFormat != nil);
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = static_cast<MTLSamplerMinMagFilter>(raw::kFilterCubic);
    id<MTLSamplerState> cubic = [device newSamplerStateWithDescriptor:sd];
    id<MTLSamplerState> sameSampler = [device newSamplerStateWithDescriptor:sd];
    sd.sAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> unusedSampler = [device newSamplerStateWithDescriptor:sd];
    ASSERT_TRUE(cubic != nil && sameSampler != nil && unusedSampler != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = nil;
    id<MTLRenderCommandEncoder> encoder = nil;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = image(device, MTLTextureUsageRenderTarget);
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    const auto begin = [&] {
        commands = [queue commandBuffer];
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (pipelineFirst)
            [encoder setRenderPipelineState:state];
        texture(encoder, stage, firstTexture);
        sampler(encoder, stage, cubic);
        if (!pipelineFirst)
            [encoder setRenderPipelineState:state];
    };
    begin();
    ASSERT_TRUE(compiler.wait(1)) << "cubic preparation must ask before the first draw";
    const auto requests = compiler.captured();
    ASSERT_EQ(requests.size(), 1u);
    const auto &binds = requests[0].variant.cubic;
    ASSERT_EQ(binds.samplers.size(), 1u);
    ASSERT_EQ(binds.textures.size(), 1u);
    EXPECT_EQ(binds.samplers[0].stage, stage);
    EXPECT_EQ(binds.textures[0].stage, stage);
    EXPECT_EQ(binds.textures[0].format, MTLPixelFormatBGRA8Unorm);
    __unsafe_unretained RMPipeline *pipeline = (RMPipeline *)state;
    const auto found = pipeline->_variants.find(requests[0].variant.name());
    ASSERT_NE(found, pipeline->_variants.end());
    ASSERT_TRUE(ready(*found->second));
    raw::Pipeline *selected = found->second->value();
    ASSERT_NE(selected, nullptr);
    const uint32_t expected = program(selected->methods, stage);
    ASSERT_NE(expected, UINT32_MAX);
    const auto draws = [&] {
        for (unsigned i = 0; i < 16; i++) {
            texture(encoder, stage, i & 1 ? sameFormat : firstTexture);
            sampler(encoder, stage, i & 2 ? sameSampler : cubic);
            sampler(encoder, stage, i & 1 ? unusedSampler : cubic, 1);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    };
    for (unsigned round = 0; round < 8; round++) {
        @autoreleasepool {
            if (round)
                begin();
            draws();
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            ASSERT_EQ(commands.status, MTLCommandBufferStatusCompleted);
            encoder = nil;
            commands = nil;
        }
    }
    const size_t firstJob = kernel_model::model.jobs.size();
    begin();
    const uint64_t placed = gRM->raw->stats.programsPlaced;
    record_count work = {};
    record_count_start();
    draws();
    record_count_stop(&work);
    EXPECT_EQ(work.locks, 0u);
    EXPECT_EQ(work.mallocs, 0u);
    EXPECT_EQ(work.dispatches, 0u);
    EXPECT_EQ(compiler.captured().size(), 1u);
    EXPECT_EQ(gRM->raw->stats.programsPlaced, placed);
    std::vector<uint32_t> expectedDraws(16, expected);
    sampler(encoder, stage, nil);
    texture(encoder, stage, nil);
    texture(encoder, stage, firstTexture);
    sampler(encoder, stage, cubic);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(expected);
    EXPECT_EQ(compiler.captured().size(), 1u);

    id<MTLTexture> otherFormat = image(device, MTLTextureUsageShaderRead, MTLPixelFormatRGBA16Float);
    ASSERT_TRUE(otherFormat != nil);
    texture(encoder, stage, otherFormat);
    ASSERT_TRUE(compiler.wait(2));
    const auto rebound = compiler.captured();
    ASSERT_EQ(rebound.size(), 2u);
    ASSERT_EQ(rebound.back().variant.cubic.textures.size(), 1u);
    EXPECT_EQ(rebound.back().variant.cubic.textures[0].stage, stage);
    EXPECT_EQ(rebound.back().variant.cubic.textures[0].format, MTLPixelFormatRGBA16Float);
    const auto changed = pipeline->_variants.find(rebound.back().variant.name());
    ASSERT_NE(changed, pipeline->_variants.end());
    ASSERT_TRUE(ready(*changed->second));
    const uint32_t changedProgram = program(changed->second->value()->methods, stage);
    ASSERT_NE(changedProgram, UINT32_MAX);
    EXPECT_NE(changedProgram, expected);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(changedProgram);
    texture(encoder, stage, firstTexture);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(expected);
    EXPECT_EQ(compiler.captured().size(), 2u);
    const uint32_t otherStage = stage == 1 ? 2 : 1;
    texture(encoder, otherStage, sameFormat);
    sampler(encoder, otherStage, sameSampler);
    ASSERT_TRUE(compiler.wait(3));
    const auto both = compiler.captured();
    ASSERT_EQ(both.size(), 3u);
    ASSERT_EQ(both.back().variant.cubic.samplers.size(), 2u);
    EXPECT_EQ(both.back().variant.cubic.samplers[0].stage, raw::kCubicFragment);
    EXPECT_EQ(both.back().variant.cubic.samplers[1].stage, raw::kCubicVertex);
    const auto joined = pipeline->_variants.find(both.back().variant.name());
    ASSERT_NE(joined, pipeline->_variants.end());
    ASSERT_TRUE(ready(*joined->second));
    const uint32_t otherProgram = program(joined->second->value()->methods, otherStage);
    ASSERT_NE(otherProgram, UINT32_MAX);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    expectedDraws.push_back(program(joined->second->value()->methods, stage));
    [encoder endEncoding];
    state = nil;
    library = nil;
    firstTexture = sameFormat = otherFormat = nil;
    cubic = sameSampler = unusedSampler = nil;
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    const auto programs = drawnPrograms(firstJob, stage);
    EXPECT_EQ(programs, expectedDraws);
    const auto otherPrograms = drawnPrograms(firstJob, otherStage);
    ASSERT_FALSE(otherPrograms.empty());
    EXPECT_EQ(otherPrograms.back(), otherProgram);
    EXPECT_EQ(compiler.captured().size(), 3u);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u);
}

void composedForms()
{
    constexpr MTLPixelFormat colour = MTLPixelFormat(550);
    const std::string directory = dir() + "/cubic-composed-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    Compiler compiler;
    ASSERT_TRUE(compiler.serve(directory));
    MTLRenderPipelineDescriptor *description = descriptor(nil, colour);
    description.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    const raw::StageKey vertex{"v", ""}, fragment{"f", ""};
    const std::string key = raw::pipelineKey(description, &vertex, &fragment, "", "QuartzCore");
    const std::vector<raw::PackEntry> entries = {{0, raw::EntryTexture, 1, 0, raw::SamplerSlot, 0, 0},
                                                 {16, raw::EntryTexture, 2, 0, raw::SamplerSlot, 0, 0},
                                                 {32, raw::EntryFetch, 2, 0, raw::SamplerNone, 0, 0}};
    const std::string pack = writePack(key, {}, {}, true, "", {}, entries);
    NSString *why = nil;
    id<MTLDevice> device = raw_metal_device_open(pack.c_str(), 0, nil, &why);
    ASSERT_TRUE(device != nil) << (why ? why.UTF8String : "");
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    raw_metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    description.vertexFunction = [library newFunctionWithName:@"v"];
    description.fragmentFunction = [library newFunctionWithName:@"f"];
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:description error:nil];
    ASSERT_TRUE(state != nil);
    NSMutableArray *planes = [NSMutableArray array];
    for (unsigned bytes : {4u, 1u})
        [planes addObject:@{
            (id)kIOSurfacePlaneWidth : @64,
            (id)kIOSurfacePlaneHeight : @64,
            (id)kIOSurfacePlaneBytesPerElement : @(bytes),
            (id)kIOSurfacePlaneBytesPerRow : @(64 * bytes),
            (id)kIOSurfacePlaneOffset : @(bytes == 4 ? 0 : 64 * 64 * 4),
            (id)kIOSurfacePlaneSize : @(64 * 64 * bytes)
        }];
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(64 * 64 * 5),
        (id)kIOSurfacePlaneInfo : planes
    });
    ASSERT_NE(surface, nullptr);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:colour width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td iosurface:surface plane:0];
    CFRelease(surface);
    ASSERT_TRUE(target != nil);
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = image(device, MTLTextureUsageRenderTarget, MTLPixelFormatDepth32Float);
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.clearDepth = 1;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state];
    texture(encoder, 1, image(device, MTLTextureUsageShaderRead));
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = static_cast<MTLSamplerMinMagFilter>(raw::kFilterCubic);
    sampler(encoder, 1, [device newSamplerStateWithDescriptor:sd]);
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    [encoder setDepthStencilState:[device newDepthStencilStateWithDescriptor:ds]];
    ASSERT_TRUE(compiler.wait(3));
    const auto requests = compiler.captured();
    ASSERT_EQ(requests.size(), 3u);
    const auto composed = std::find_if(requests.begin(), requests.end(), [](const auto &request) {
        return request.variant.fragmentTests == uint8_t(raw::FragmentTests::DepthWrite) &&
               !request.variant.cubic.empty() && request.variant.twoPlane;
    });
    ASSERT_NE(composed, requests.end());
    const auto &forms = composed->variant;
    ASSERT_EQ(forms.cubic.samplers.size(), 1u);
    EXPECT_EQ(forms.cubic.samplers[0].stage, raw::kCubicVertex);
    EXPECT_TRUE(forms.twoPlane);
    EXPECT_EQ(forms.fragmentTests, uint8_t(raw::FragmentTests::DepthWrite));
    __unsafe_unretained RMPipeline *pipeline = (RMPipeline *)state;
    for (const auto &request : requests) {
        const auto found = pipeline->_variants.find(request.variant.name());
        ASSERT_NE(found, pipeline->_variants.end());
        ASSERT_TRUE(ready(*found->second));
    }
    raw::Pipeline *selected = pipeline->_variants.at(forms.name())->value();
    ASSERT_NE(selected, nullptr);
    const uint32_t expected = program(selected->methods, 1);
    ASSERT_NE(expected, UINT32_MAX);
    const size_t firstJob = kernel_model::model.jobs.size();
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    state = nil;
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    EXPECT_EQ(drawnPrograms(firstJob, 1), (std::vector<uint32_t>{expected}));
    EXPECT_EQ(compiler.captured().size(), 3u);
    EXPECT_EQ(gOpens, 0);
}

}  // namespace

TEST(RawFacadeCubic, FragmentPreparationPrecedesDrawAndSameFormatRebindsReuseIt)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(2, true);
    });
}

TEST(RawFacadeCubic, VertexPreparationPrecedesDrawAndSameFormatRebindsReuseIt)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(1, true);
    });
}

TEST(RawFacadeCubic, VertexBindingsBeforePipelineSelectThePreparedProgram)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(1, false);
    });
}

TEST(RawFacadeCubic, FragmentBindingsBeforePipelineSelectThePreparedProgram)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        preparedCubic(2, false);
    });
}

TEST(RawFacadeCubic, VertexCubicComposesWithDepthTestsAndTwoPlaneTargetsAtSetup)
{
    switch_ios::test::ExpectPassesInFreshProcess(composedForms);
}
