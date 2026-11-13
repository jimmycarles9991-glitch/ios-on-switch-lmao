// SPDX-License-Identifier: MIT
// The raw facade (RawMetal.h) on the host over the kernel model (raw_kernel_model.h, whose IOKit calls replace the
// framework's: include this in one unit of a test) and NVK's shared connection (fake_nvk_connection.c, loaded first). A
// pack of one state is written for each process: its two programs a page or more, so they run in place from the pack's
// pages, and a metallib at <dir>/QuartzCore.framework/default.metallib listing the state's functions, so the state is the
// framework's and looked up in the pack. The m2v device is a stand-in that answers zero: what a test reads is whether, and
// for what, the facade opened it. facade::device() is instead opened once per process over a pack of no states with no m2v
// device, the frontend and the bridge answered by raw_facade_standins.mm.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include "raw_kernel_model.h"
#include "raw_job_methods.h"
#include "made_metallib.h"
#include "empty_pack.h"
#include "raw_facade_standins.h"
#include "../raw/runtime/RawMetal.h"
#include "../raw/runtime/RawMetalInternal.h"
#include "../raw/runtime/RawPipelineKey.h"
#include "../raw/runtime/raw_compute_key.h"
#include "../raw/runtime/raw_metallib.h"
#include "../raw/runtime/raw_pack.h"
#include <algorithm>
#include <dlfcn.h>
#include <map>
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

// The m2v device: no object of its own, every Metal selector answered zero; harnessSelfRegistry is m2v's device class's
// method the raw device has none of, run on whichever device it was sent to (RawM2V.methods)
@interface HarnessM2V : NSObject
@end
@implementation HarnessM2V

- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    for (BOOL required : {YES, NO}) {
        const objc_method_description d = protocol_getMethodDescription(@protocol(MTLDevice), selector, required, YES);
        if (d.types)
            return [NSMethodSignature signatureWithObjCTypes:d.types];
    }
    return [super methodSignatureForSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
    uint64_t zero[4] = {};
    if (invocation.methodSignature.methodReturnLength)
        [invocation setReturnValue:zero];
}

- (uint64_t)harnessSelfRegistry
{
    return [(id<MTLDevice>)self registryID];
}

@end

@interface HarnessM2VMethods : NSObject
- (uint64_t)harnessSelfRegistry;
@end
@implementation HarnessM2VMethods

- (uint64_t)harnessSelfRegistry
{
    return 0;
}

@end

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

// The pack key the facade makes of descriptor(QuartzCore's library, format) (RawPipelineKey.h; QuartzCore is no suffix)
inline std::string packKey(MTLPixelFormat format)
{
    const raw::StageKey vertex{"v", ""}, fragment{"f", ""};
    return raw::pipelineKey(descriptor(nil, format), &vertex, &fragment, "", "QuartzCore");
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
// own after those (its `program` set here); `facts`: its compute states' answers
inline std::string writePack(const std::string &key, std::vector<raw::PackKernel> kernels = {},
                             std::vector<raw::PackComputeFacts> facts = {})
{
    constexpr uint64_t kPrograms = 0x180, kPipelines = 0x800, kComputeStates = 0xc00, kStrings = 0xe00,
                       kKernels = 0x2000;
    const uint32_t programs = 2 + uint32_t(kernels.size());
    std::vector<uint8_t> file(kHeap + programs * kSlot + 0x1000, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.programs = programs;
    h.pipelines = 1;
    const std::string fields = key.substr(0, key.find(" | v"));
    std::vector<char> strings(1, 0);
    strings.insert(strings.end(), fields.begin(), fields.end());
    strings.push_back(0);
    h.section[raw::PackPrograms] = {kPrograms, programs * sizeof(raw::PackProgram)};
    h.section[raw::PackPipelines] = {kPipelines, sizeof(raw::PackPipeline)};
    h.section[raw::PackStrings] = {kStrings, strings.size()};
    h.section[raw::PackKernels] = {kKernels, kernels.size() * sizeof(raw::PackKernel)};
    h.section[raw::PackHeap] = {kHeap, programs * kSlot + 0x1000};
    std::sort(facts.begin(), facts.end(), [](const auto &a, const auto &b) {
        return a.keyHash < b.keyHash;
    });
    h.section[raw::PackComputeStates] = {kComputeStates, facts.size() * sizeof(raw::PackComputeFacts)};
    memcpy(file.data() + kComputeStates, facts.data(), facts.size() * sizeof(raw::PackComputeFacts));
    memcpy(file.data(), &h, sizeof h);
    for (uint32_t k = 0; k < programs; k++) {
        raw::PackProgram p = {};
        p.heapOffset = uint32_t(k * kSlot);
        p.program = p.heapOffset + 0x30;
        p.bytes = uint32_t(kSlot);
        p.cbufs = 1;
        p.cbufType[0] = raw::CbufRoot;
        p.gprs = k < 2 ? 8 : 0;
        memcpy(file.data() + kPrograms + k * sizeof p, &p, sizeof p);
        const std::vector<uint8_t> bytes = slot(k);
        memcpy(file.data() + kHeap + k * kSlot, bytes.data(), bytes.size());
    }
    std::sort(kernels.begin(), kernels.end(), [](const raw::PackKernel &a, const raw::PackKernel &b) {
        return a.keyHash < b.keyHash;
    });
    for (size_t i = 0; i < kernels.size(); i++) {
        kernels[i].program = uint16_t(2 + i);
        memcpy(file.data() + kKernels + i * sizeof(raw::PackKernel), &kernels[i], sizeof(raw::PackKernel));
    }
    raw::PackPipeline pp = {};
    pp.keyHash = raw::Pack::fnv(key.data(), key.size());
    pp.keyString = 1;
    pp.vs = 0;
    pp.fs = 1;
    memcpy(file.data() + kPipelines, &pp, sizeof pp);
    memcpy(file.data() + kStrings, strings.data(), strings.size());
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
    std::map<std::string, raw::AirHash> hashes;
    if (!raw::metallibAirHashes(metallib, hashes) || !hashes.count(function))
        return 0;
    const std::string key = raw::computeStateKey(hashes[function], "", 0, false);
    return raw::Pack::fnv(key.data(), key.size());
}

// What the opener was asked: each process's m2v device opens once, at most
inline int gOpens = 0;

// A raw device over the pack of the BGRA8 state, NVK's connection the fake's, the m2v device the stand-in (`standIn`'s)
inline id<MTLDevice> open(const std::string &pack, Class methods = [HarnessM2VMethods class],
                          Class standIn = [HarnessM2V class])
{
    const char *nvk = getenv("SWITCH_FAKE_NVK");
    if (!nvk || !dlopen(nvk, RTLD_NOW | RTLD_LOCAL))
        return nil;
    RawM2V m2v;
    m2v.open = ^id<MTLDevice>
    {
        gOpens++;
        return (id<MTLDevice>)[standIn new];
    };
    m2v.methods = methods;
    NSString *why = nil;
    id<MTLDevice> device = raw_metal_device_open_lazy(pack.c_str(), 0, kRegistryID, m2v, &why);
    if (!device)
        fprintf(stderr, "open: %s\n", why.UTF8String ?: "?");
    return device;
}

inline NSDictionary *ask(id<MTLDevice> device, const char *selector)
{
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)(device, sel_registerName(selector));
}

// The device's record of its m2v device (RawMetalM2V.mm switchRawM2V)
inline NSDictionary *m2vRecord(id<MTLDevice> device)
{
    return ask(device, "switchRawM2V");
}

inline NSDictionary *memory(id<MTLDevice> device)
{
    return ask(device, "switchRawMemory");
}

inline NSDictionary *counters(id<MTLDevice> device)
{
    return ask(device, "switchRawCounters");
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
        id<MTLDevice> made = raw_metal_device_open(emptyPack().c_str(), 0, nil, &why);
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
