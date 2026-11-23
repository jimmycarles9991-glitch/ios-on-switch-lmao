// SPDX-License-Identifier: MIT
// The facade (TegraGM20BMetal.h) on the host over the kernel model (kernel_model.h, whose IOKit calls replace the
// framework's: include this in one unit of a test). A pack of one state is written for each process: its two programs a
// page or more, so they run in place from the pack's pages, and a metallib at <dir>/QuartzCore.framework/default.metallib
// listing the state's functions, so the state is the framework's and looked up in the pack. facade::device() is instead
// opened once per process over a pack of no states, the translation answered by translate_absent.cpp.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include "kernel_model.h"
#include "job_methods.h"
#include "made_metallib.h"
#include "empty_pack.h"
#include "../TegraGM20BMetal.h"
#include "../TegraGM20BMetalInternal.h"
#include "../TegraGM20BMetalPipelineKey.h"
#include "../compute_key.h"
#include "../metallib.h"
#include "../pack.h"
#include "../set_entries.h"
#include <algorithm>
#include <atomic>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <libproc.h>
#include <mach/mach.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace harness {

constexpr uint64_t kHeap = 0x10000, kSlot = 0x4000, kRegistryID = 0xABC;

inline std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

// A render state of `format` over the library's functions "v" and "f", one color attachment
inline MTLRenderPipelineDescriptor *descriptor(id<MTLLibrary> library, MTLPixelFormat format)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = library ? [library newFunctionWithName:@"v"] : nil;
    d.fragmentFunction = library ? [library newFunctionWithName:@"f"] : nil;
    d.colorAttachments[0].pixelFormat = format;
    return d;
}

// The pack key the facade makes of descriptor(QuartzCore's library, format) (TegraGM20BMetalPipelineKey.h; QuartzCore is no suffix)
inline std::string packKey(MTLPixelFormat format)
{
    const gm20b::StageKey vertex{"v", ""}, fragment{"f", ""};
    return gm20b::pipelineKey(descriptor(nil, format), &vertex, &fragment, "", "QuartzCore");
}

// Program slot `k`'s bytes: its shader header (zeros: no local memory, no outputs) at 0x30, then code
inline std::vector<uint8_t> slot(uint32_t k)
{
    std::vector<uint8_t> bytes(kSlot, 0);
    for (size_t i = 0x80; i < bytes.size(); i++)
        bytes[i] = uint8_t(i * 7 + 13 * k + 1);
    return bytes;
}

// A pack of this build's version holding the state of `key`: vertex program slot 0, fragment slot 1, each kSlot bytes in
// the heap section at kHeap, so a page-multiple slot of the pack's own pages; `kernels`, each with a program slot of its
// own after those (its `program` set here); `facts`: its compute states' answers. Without `pixelStage` the state has no
// fragment program (pack.h kNoProgram). `reflections`: its PackReflections section (reflection_blob_writer.h).
// `named`: further states the device looks up by name (Device::ycbcrPipeline's), each over the same two programs, its
// fields the key's with its own attachment 0 format; `entries`: the handles and buffers the key's state reads
inline std::string writePack(const std::string &key, std::vector<gm20b::PackKernel> kernels = {},
                             std::vector<gm20b::PackComputeFacts> facts = {}, bool pixelStage = true,
                             const std::string &reflections = "",
                             const std::vector<std::pair<std::string, MTLPixelFormat>> &named = {},
                             const std::vector<gm20b::PackEntry> &entries = {})
{
    constexpr uint64_t kPrograms = 0x180, kComputeStates = 0xc00, kStrings = 0xe00, kKernels = 0x2000;
    // one state's record fits below the compute states; more go between the kernels and the heap
    const uint64_t kPipelines = named.empty() ? 0x800 : 0x8000;
    const uint32_t programs = 2 + uint32_t(kernels.size());
    std::vector<uint8_t> file(kHeap + programs * kSlot + 0x1000, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.programs = programs;
    h.pipelines = uint32_t(1 + named.size());
    const std::string fields = key.substr(0, key.find(" | v"));
    std::vector<char> strings(1, 0);
    std::map<uint64_t, uint32_t> fieldsAt;
    auto addState = [&](const std::string &name, const std::string &stateFields) {
        fieldsAt[gm20b::Pack::fnv(name.data(), name.size())] = uint32_t(strings.size());
        strings.insert(strings.end(), stateFields.begin(), stateFields.end());
        strings.push_back(0);
    };
    addState(key, fields);
    for (const auto &[name, format] : named) {
        std::string own = fields;
        if (const size_t at = own.find(" c0="); at != std::string::npos)
            own.replace(at + 4, own.find(':', at) - at - 4, std::to_string(format));
        addState(name, own);
    }
    h.section[gm20b::PackPrograms] = {kPrograms, programs * sizeof(gm20b::PackProgram)};
    h.section[gm20b::PackPipelines] = {kPipelines, h.pipelines * sizeof(gm20b::PackPipeline)};
    h.section[gm20b::PackStrings] = {kStrings, strings.size()};
    h.section[gm20b::PackKernels] = {kKernels, kernels.size() * sizeof(gm20b::PackKernel)};
    h.section[gm20b::PackHeap] = {kHeap, programs * kSlot + 0x1000};
    std::sort(facts.begin(), facts.end(), [](const auto &a, const auto &b) {
        return a.keyHash < b.keyHash;
    });
    h.section[gm20b::PackComputeStates] = {kComputeStates, facts.size() * sizeof(gm20b::PackComputeFacts)};
    memcpy(file.data() + kComputeStates, facts.data(), facts.size() * sizeof(gm20b::PackComputeFacts));
    memcpy(file.data(), &h, sizeof h);
    for (uint32_t k = 0; k < programs; k++) {
        gm20b::PackProgram p = {};
        p.heapOffset = uint32_t(k * kSlot);
        p.program = p.heapOffset + 0x30;
        p.bytes = uint32_t(kSlot);
        p.cbufs = 1;
        p.cbufType[0] = gm20b::CbufRoot;
        p.gprs = k < 2 ? 8 : 0;
        memcpy(file.data() + kPrograms + k * sizeof p, &p, sizeof p);
        const std::vector<uint8_t> bytes = slot(k);
        memcpy(file.data() + kHeap + k * kSlot, bytes.data(), bytes.size());
    }
    std::sort(kernels.begin(), kernels.end(), [](const gm20b::PackKernel &a, const gm20b::PackKernel &b) {
        return a.keyHash < b.keyHash;
    });
    for (size_t i = 0; i < kernels.size(); i++) {
        kernels[i].program = uint16_t(2 + i);
        memcpy(file.data() + kKernels + i * sizeof(gm20b::PackKernel), &kernels[i], sizeof(gm20b::PackKernel));
    }
    const uint64_t keyHash = gm20b::Pack::fnv(key.data(), key.size());
    size_t record = 0;
    for (const auto &[hash, at] : fieldsAt) {
        gm20b::PackPipeline pp = {};
        pp.keyHash = hash;
        pp.keyString = at;
        pp.vs = 0;
        pp.fs = hash != keyHash || pixelStage ? 1 : gm20b::kNoProgram;
        if (hash == keyHash) {
            const uint32_t count = uint32_t(std::min<size_t>(entries.size(), gm20b::kPackEntries));
            gm20b::setEntryCount(pp, count);
            std::copy_n(entries.begin(), count, pp.entry);
        }
        memcpy(file.data() + kPipelines + record++ * sizeof pp, &pp, sizeof pp);
    }
    memcpy(file.data() + kStrings, strings.data(), strings.size());
    const uint64_t reflectionsAt = file.size();
    file.insert(file.end(), reflections.begin(), reflections.end());
    h.section[gm20b::PackReflections] = {reflectionsAt, reflections.size()};
    memcpy(file.data(), &h, sizeof h);
    const std::string path = dir() + "/pack.bin";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    return path;
}

// QuartzCore's default library as the facade reads it: "v" a vertex function, "f" a fragment one, "k" a kernel
inline std::string writeMetallib()
{
    const std::string framework = dir() + "/QuartzCore.framework";
    mkdir(framework.c_str(), 0755);
    const std::string path = framework + "/default.metallib";
    const std::string bytes = made_metallib::universal({"v", "f", "k"}, 0xA0, 0xEE, {0, 1, 2});
    std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    return path;
}

// The pack's hash of the compute state the library at `metallib` makes of `function` with no descriptor fields
inline uint64_t computeStateHash(const std::string &metallib, const std::string &function)
{
    std::map<std::string, gm20b::AirHash> hashes;
    if (!gm20b::metallibAirHashes(metallib, hashes) || !hashes.count(function))
        return 0;
    const std::string key = gm20b::computeStateKey(hashes[function], "", 0, false);
    return gm20b::Pack::fnv(key.data(), key.size());
}

// A raw device over the pack of the BGRA8 state; nil and in `why` the open's reason when it did not open
inline id<MTLDevice> open(const std::string &pack, NSString **why)
{
    return metal_device_open(pack.c_str(), 0, kRegistryID, why);
}

inline id<MTLDevice> open(const std::string &pack)
{
    NSString *why = nil;
    id<MTLDevice> device = open(pack, &why);
    if (!device)
        fprintf(stderr, "open: %s\n", why.UTF8String ?: "?");
    return device;
}

// One draw of `state` sampling fragment texture 0 through sampler 0 into a 64x64 BGRA8 target, committed and waited for
inline id<MTLCommandBuffer> drawSampling(id<MTLDevice> device, id<MTLRenderPipelineState> state, id<MTLTexture> texture,
                                         id<MTLSamplerState> sampler)
{
    MTLTextureDescriptor *rt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    rt.storageMode = MTLStorageModePrivate;
    rt.usage = MTLTextureUsageRenderTarget;
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = [device newTextureWithDescriptor:rt];
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    [e setFragmentTexture:texture atIndex:0];
    [e setFragmentSamplerState:sampler atIndex:0];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

inline NSDictionary *ask(id<MTLDevice> device, const char *selector)
{
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)(device, sel_registerName(selector));
}

inline NSDictionary *memory(id<MTLDevice> device)
{
    return ask(device, "switchMetalMemory");
}

// Blocks until every compressed memory the device was asked for so far is made or refused: the event
// switchMetalProbeCompressionSettled:'s block reports (debug builds), on the compression thread or at once on this one when
// nothing is asked. The block's calls: one.
inline int compressionSettled(id<MTLDevice> device)
{
    const auto calls = std::make_shared<std::atomic<int>>(0);
    dispatch_semaphore_t ran = dispatch_semaphore_create(0);
    NSString *refused = ((NSString * (*)(id, SEL, id))
                             objc_msgSend)((id)device, sel_registerName("switchMetalProbeCompressionSettled:"), ^{
        ++*calls;
        dispatch_semaphore_signal(ran);
    });
    EXPECT_EQ(refused, nil) << refused.UTF8String;
    if (refused)
        return 0;
    dispatch_semaphore_wait(ran, DISPATCH_TIME_FOREVER);
    return calls->load();
}

inline NSDictionary *counters(id<MTLDevice> device)
{
    return ask(device, "switchMetalCounters");
}

// The counters of `family` (switchMetalCounters failed and failed.<predicate>, or skipped and skipped.<predicate>) that
// moved from `before` to `after`. Most skips are counted at the call, as it is recorded: `before` is read before the
// recording of the work judged begins.
inline std::map<std::string, long long> countersMoved(NSDictionary *before, NSDictionary *after, NSString *family)
{
    std::map<std::string, long long> moved;
    NSString *keys = [family stringByAppendingString:@"."];
    for (NSString *key in after)
        if ([key isEqualToString:family] || [key hasPrefix:keys])
            if (const long long n = [after[key] longLongValue] - [before[key] longLongValue])
                moved[key.UTF8String] = n;
    return moved;
}

inline std::map<std::string, long long> failedMoved(NSDictionary *before, NSDictionary *after)
{
    return countersMoved(before, after, @"failed");
}

inline std::map<std::string, long long> skippedMoved(NSDictionary *before, NSDictionary *after)
{
    return countersMoved(before, after, @"skipped");
}

// What one command buffer whose failure named `pred` ("none": no predicate) moves
inline std::map<std::string, long long> failedOnce(const std::string &pred)
{
    return {{"failed", 1}, {"failed." + pred, 1}};
}

// What one call skipped as `pred` moves
inline std::map<std::string, long long> skippedOnce(const std::string &pred)
{
    return {{"skipped", 1}, {"skipped." + pred, 1}};
}

inline std::string gm20bPath(id<MTLCommandBuffer> cb)
{
    return ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath")).UTF8String ?: "";
}

inline uint64_t footprint()
{
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count);
    return vm.phys_footprint;
}

// An import the model holds bound in the GPU's VM whose pages are a file's: the process's mapping of it, at `offset` of the
// file
struct FileImport {
    uint64_t address, bytes, offset;
};

// The bound imports of the file at `path`
inline std::vector<FileImport> boundFileImports(const std::string &path)
{
    char want[PATH_MAX];
    if (!realpath(path.c_str(), want))
        return {};
    std::vector<FileImport> found;
    for (const auto &[va, bound] : kernel_model::model.binds) {
        (void)va;
        const auto import = kernel_model::model.imports.find(bound.first);
        if (import == kernel_model::model.imports.end())
            continue;
        proc_regionwithpathinfo info = {};
        char real[PATH_MAX];
        if (proc_pidinfo(getpid(), PROC_PIDREGIONPATHINFO, import->second.first, &info, sizeof info) != sizeof info ||
            !realpath(info.prp_vip.vip_path, real) || strcmp(real, want))
            continue;
        const uint64_t into = import->second.first - info.prp_prinfo.pri_address;
        found.push_back({import->second.first, import->second.second, info.prp_prinfo.pri_offset + into});
    }
    return found;
}

// Bytes [offset, offset + bytes) of the file at `path`, read from it, not from any mapping
inline std::vector<uint8_t> fileBytes(const std::string &path, uint64_t offset, uint64_t bytes)
{
    std::vector<uint8_t> out(bytes);
    std::ifstream f(path, std::ios::binary);
    f.seekg(std::streamoff(offset));
    f.read(reinterpret_cast<char *>(out.data()), std::streamsize(bytes));
    return out;
}

// One pass into a 64x64 target of `format` drawing a triangle with `state`, committed and waited for
inline id<MTLCommandBuffer> draw(id<MTLDevice> device, id<MTLRenderPipelineState> state, MTLPixelFormat format)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

}  // namespace harness

namespace facade {

inline id<MTLDevice> device()
{
    static id<MTLDevice> opened = [] {
        NSString *why = nil;
        id<MTLDevice> made = metal_device_open(emptyPack().c_str(), 0, 0, &why);
        EXPECT_NE(made, nil) << (why ? why.UTF8String : "");
        return made;
    }();
    return opened;
}

inline uint64_t word(MTLResourceID id)
{
    uint64_t w;
    memcpy(&w, &id, sizeof w);
    return w;
}

inline id<MTLBuffer> shared(NSUInteger length)
{
    return [device() newBufferWithLength:length options:MTLResourceStorageModeShared];
}

// An argument buffer of `fill` bytes, none of which an encoder's word or a test's constant is
inline id<MTLBuffer> filled(NSUInteger length, uint8_t fill = 0xA5)
{
    id<MTLBuffer> b = shared(length);
    memset(b.contents, fill, length);
    return b;
}

inline id<MTLTexture> texture(MTLStorageMode mode = MTLStorageModeShared, id<MTLHeap> heap = nil)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:16
                                                                                height:16
                                                                             mipmapped:NO];
    d.storageMode = mode;
    d.usage = MTLTextureUsageShaderRead;
    return heap ? [heap newTextureWithDescriptor:d] : [device() newTextureWithDescriptor:d];
}

inline id<MTLSamplerState> sampler(bool normalized = true)
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.supportArgumentBuffers = YES;
    d.normalizedCoordinates = normalized;
    return [device() newSamplerStateWithDescriptor:d];
}

inline id<MTLHeap> heap(MTLStorageMode mode, NSUInteger bytes)
{
    MTLHeapDescriptor *d = [MTLHeapDescriptor new];
    d.size = bytes;
    d.storageMode = mode;
    d.type = MTLHeapTypeAutomatic;
    return [device() newHeapWithDescriptor:d];
}

inline std::vector<uint8_t> bytesOf(id<MTLBuffer> b)
{
    const auto *at = static_cast<const uint8_t *>(b.contents);
    return {at, at + b.length};
}

inline uint64_t at(id<MTLBuffer> b, uint64_t offset)
{
    uint64_t w;
    memcpy(&w, static_cast<const uint8_t *>(b.contents) + offset, sizeof w);
    return w;
}

}  // namespace facade
