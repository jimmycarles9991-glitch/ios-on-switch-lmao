// SPDX-License-Identifier: MIT
// computeframe: one Metal kernel dispatched through the shipped frontend + m2v + NVK and through the raw path's compute encoder
// (runtime/raw_compute.h), over the mock GPU; test/computeeq.sh compares the two decodes (pack/eqcheck.py's DISPATCH records).
//   computeframe nvk <nvk dylib> <cache dir> <metallib> <kernel> <threads x,y,z> <groups x,y,z>
//     with NVDEC_KERNELS set, its log's K lines are the kernel's record (pack/nvdec_compute.cpp)
//   computeframe raw <pack.bin> <nvk log> <programs.bin> <threads x,y,z> <groups x,y,z>
//   computeframe pack <pack.bin> - <key> <threads x,y,z> <groups x,y,z>: the pack's kernel of that raw_compute_key.h key
// Both bind every buffer slot 0..kSlots-1 to the same tagged bytes (pack/probe.mm's scheme).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../runtime/raw_compute.h"
#include "../runtime/raw_compute_key.h"
#ifdef RAW_FACADE  // computefacade: the nvk mode's dispatch through the raw path's Metal facade (runtime/RawMetal.mm)
#include "../runtime/RawMetal.h"
#endif

extern "C" void pushcount_report(const char *what);

namespace {

constexpr uint32_t kSlots = 8, kTextures = 8, kTagBytes = 4096;
constexpr uint32_t kRgba8Unorm = 70;

std::vector<uint32_t> tag(uint32_t slot)
{
    std::vector<uint32_t> t(kTagBytes / 4);
    t[0] = 0xC0DE0000u | slot;
    for (uint32_t k = 1; k < t.size(); k++)
        t[k] = 0xA0000000u | slot << 16 | k;
    return t;
}

// Texture slot t: (16 + t) x (8 + t) RGBA8 texels of a pattern; sampler slot s: max LOD clamp 1 + s / 2
uint32_t texel(uint32_t t, uint32_t x, uint32_t y)
{
    return 0x01000000u * t ^ x * 0x00010307u ^ y * 0x00070103u;
}

std::vector<uint32_t> texels(uint32_t t)
{
    std::vector<uint32_t> v((16 + t) * (8 + t));
    for (uint32_t y = 0; y < 8 + t; y++)
        for (uint32_t x = 0; x < 16 + t; x++)
            v[y * (16 + t) + x] = texel(t, x, y);
    return v;
}

// FRAME_BIG_BUFFER=<bytes>: slot kBigSlot bound to a buffer of that many bytes instead of its tag (a storage range past
// a constant buffer's 64 KiB)
constexpr uint32_t kBigSlot = 1;

// FRAME_DISPATCHES=<n>: the dispatch n times in one serial encoder (the order between them)
uint32_t dispatches()
{
    const char *n = getenv("FRAME_DISPATCHES");
    return n ? std::max(1u, uint32_t(atoi(n))) : 1;
}

// FRAME_BLIT_FIRST: a fill before the compute encoder, in the same command buffer (over the facade, a refused compute
// encoder sends only its own segment to m2v: the rates line's sp=)
void blitFirst(id<MTLDevice> dev, id<MTLCommandBuffer> cb)
{
    id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:b range:NSMakeRange(0, 4096) value:0x5a];
    [blit endEncoding];
}

// FRAME_OFF16=<slot>:<offset>: that slot's tagged bytes from a zeroed buffer at an offset off 16 bytes (Core Animation's
// luma sums), so the descriptor starts below the tag and the residual is the slot's SlotBindOffset push word
bool off16(uint32_t &slot, uint32_t &offset)
{
    const char *text = getenv("FRAME_OFF16");
    return text && sscanf(text, "%u:%u", &slot, &offset) == 2 && slot < kSlots;
}

// FRAME_TEXTURES=<n>: textures (and samplers, unless FRAME_NO_SAMPLERS) bound at slots 0..n-1 only, as a kernel's caller binds
// them (the facade looks its kernel up by what is bound)
uint32_t boundTextures()
{
    const char *n = getenv("FRAME_TEXTURES");
    return n ? std::min(uint32_t(atoi(n)), kTextures) : kTextures;
}

uint32_t bigBuffer()
{
    const char *bytes = getenv("FRAME_BIG_BUFFER");
    return bytes ? uint32_t(strtoul(bytes, nullptr, 0)) : 0;
}

void fillBig(uint8_t *p, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++)
        p[i] = uint8_t(i * 7 + (i >> 9));
}

bool size3(const char *text, uint32_t v[3])
{
    return sscanf(text, "%u,%u,%u", &v[0], &v[1], &v[2]) == 3;
}

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

int runNvk(char **argv)
{
    uint32_t threads[3], groups[3];
    if (!size3(argv[6], threads) || !size3(argv[7], groups))
        return 2;
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    id<MTLDevice> dev = native_bridge_metal_open(argv[2], argv[3], 0, missing);
    if (!dev) {
        fprintf(stderr, "NO DEVICE %s\n", nmb_last_error());
        return 1;
    }
#ifdef RAW_FACADE
    // the raw facade over the m2v device, as the render server opens it, with the pack FRAME_PACK names
    NSString *why = nil;
    dev = raw_metal_device_open(getenv("FRAME_PACK"), 0, dev, &why);
    if (!dev) {
        fprintf(stderr, "NO FACADE %s\n", why.UTF8String);
        return 1;
    }
    // FRAME_COMPLETE=<dir>|memory: a kernel the pack lacks is made at its first dispatch (RawComplete.h), kept in <dir>
    if (const char *complete = getenv("FRAME_COMPLETE"))
        raw_metal_complete_states(strcmp(complete, "memory") ? complete : nullptr);
#endif
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4])] error:&e];
    id<MTLFunction> f = [lib newFunctionWithName:@(argv[5])];
    id<MTLComputePipelineState> ps = f ? [dev newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) {
        fprintf(stderr, "NO STATE %s\n", e.localizedDescription.UTF8String ?: "no function");
        return 1;
    }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (getenv("FRAME_BLIT_FIRST"))
        blitFirst(dev, cb);
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    for (uint32_t s = 0; s < kSlots; s++)
        [enc setBytes:tag(s).data() length:kTagBytes atIndex:s];
    if (const uint32_t big = bigBuffer()) {
        id<MTLBuffer> b = [dev newBufferWithLength:big options:MTLResourceStorageModeShared];
        fillBig(static_cast<uint8_t *>(b.contents), big);
        [enc setBuffer:b offset:0 atIndex:kBigSlot];
    }
    if (uint32_t slot, offset; off16(slot, offset)) {
        id<MTLBuffer> b = [dev newBufferWithLength:offset + kTagBytes options:MTLResourceStorageModeShared];
        memset(b.contents, 0, offset);
        memcpy(static_cast<uint8_t *>(b.contents) + offset, tag(slot).data(), kTagBytes);
        [enc setBuffer:b offset:offset atIndex:slot];
    }
    if (const char *length = getenv("FRAME_THREADGROUP_MEMORY"))
        [enc setThreadgroupMemoryLength:strtoul(length, nullptr, 0) atIndex:0];
    [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    for (uint32_t t = 0; t < boundTextures(); t++) {
        const char *format = getenv("FRAME_TEXTURE_FORMAT");
        MTLTextureDescriptor *d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:format ? MTLPixelFormat(atoi(format)) : MTLPixelFormatRGBA8Unorm
                                         width:16 + t
                                        height:8 + t
                                     mipmapped:NO];
        d.usage = getenv("FRAME_TEXTURE_READ_ONLY") ? MTLTextureUsageShaderRead
                                                    : MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> x = [dev newTextureWithDescriptor:d];
        [x replaceRegion:MTLRegionMake2D(0, 0, 16 + t, 8 + t)
             mipmapLevel:0
               withBytes:texels(t).data()
             bytesPerRow:4 * (16 + t)];
        [enc setTexture:x atIndex:t];
        if (getenv("FRAME_NO_SAMPLERS"))
            continue;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.lodMaxClamp = 1 + t / 2.0f;
        [enc setSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:t];
    }
    for (uint32_t n = 0; n < dispatches(); n++)
        [enc dispatchThreadgroups:MTLSizeMake(groups[0], groups[1], groups[2])
            threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    pushcount_report("computeframe nvk");
#ifdef RAW_FACADE
    char rates[2048];
    raw_metal_rates(rates, sizeof rates);
    fprintf(stderr, "facade rates: %s\n", rates);
#endif
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
}

uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// The program named `id` in programs.bin (pack/mkprograms.py: 'NVPG', code, cbuf count, data, cbuf types, 80-byte header,
// code, data; nvdec names it by the FNV-1a of header + code, then data)
bool program(const char *path, const std::string &id, raw::ComputeRecord &r)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t at = 0; at + 16 <= b.size();) {
        uint32_t code, count, data;
        memcpy(&code, &b[at + 4], 4);
        memcpy(&count, &b[at + 8], 4);
        memcpy(&data, &b[at + 12], 4);
        const uint8_t *types = &b[at + 16], *image = types + count, *bytes = image + 80 + code;
        char name[24];
        snprintf(name, sizeof name, "%llx", (unsigned long long)fnv(bytes, data, fnv(image, 80 + code)));
        if (id == name) {
            r.code.assign(image + 80, image + 80 + code);
            r.data.assign(bytes, bytes + data);
            for (uint32_t c = 0; c < count && c < 8; c++)
                r.cbufType[c] = types[c];
            return true;
        }
        at += 16 + count + 80 + code + data;
    }
    return false;
}

// The first dispatch's K lines of an NVDEC_KERNELS log (pack/nvdec_compute.cpp), read as pack/mkpack.py reads them
bool record(const char *log, const char *programs, raw::ComputeRecord &r)
{
    std::ifstream in(log);
    std::string line, prog;
    bool qmd = false;
    std::map<int, uint32_t> push;
    raw::PackKernel &k = r.kernel;
    k.regionWord = -1;
    auto entry = [&](uint32_t offset, uint8_t kind, uint32_t slot, uint32_t window) {
        if (k.entries < 16)
            k.entry[k.entries++] = {uint16_t(offset), kind, uint8_t(slot), window};
    };
    while (std::getline(in, line)) {
        unsigned offset, slot, bytes, w, h, t[8];
        if (!qmd && line.rfind("K qmd=", 0) == 0) {
            std::istringstream words(line.substr(6, line.find(' ', 6) - 6));
            std::string word;
            for (uint32_t i = 0; i < raw::kQmdWords && std::getline(words, word, ','); i++)
                k.qmd[i] = uint32_t(strtoul(word.c_str(), nullptr, 16));
            prog = line.substr(line.find(" prog=") + 6);
            qmd = true;
        } else if (qmd && line.rfind("K push", 0) == 0) {
            std::istringstream words(line.substr(6));
            std::string word;
            while (words >> word)
                push[atoi(word.c_str())] = uint32_t(strtoul(word.c_str() + word.find('=') + 1, nullptr, 16));
        } else if (qmd && sscanf(line.c_str(), "K region %u", &slot) == 1) {
            k.regionWord = int8_t(slot);
        } else if (qmd && sscanf(line.c_str(), "K set %x", &bytes) == 1) {
            k.setBytes = uint16_t(bytes);
        } else if (qmd && sscanf(line.c_str(), "K entry %u buffer %u %x", &offset, &slot, &bytes) == 3) {
            entry(offset, raw::KernelBuffer, slot, bytes < kTagBytes ? bytes : 0);
        } else if (qmd && sscanf(line.c_str(), "K entry %u texture %u %u", &offset, &w, &h) == 3) {
            if (w < 16 || h != w - 8)
                return false;
            entry(offset, raw::KernelTexture, w - 16, 0);
        } else if (qmd && sscanf(line.c_str(), "K entry %u sampler %x %x %x %x %x %x %x %x", &offset, &t[0], &t[1],
                                 &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 9) {
            const uint32_t lod = (t[2] >> 12) & 0xfff;  // max LOD clamp, 4.8 fixed: the probe's tag 1 + slot / 2
            if (lod < 256 || lod % 128)
                return false;
            entry(offset, raw::KernelSampler, (lod - 256) / 128, 0);
        } else if (qmd && line.rfind("DISPATCH", 0) == 0) {
            break;
        }
    }
    for (const auto &[word, value] : push)
        if (k.regionWord < 0 || word < k.regionWord || word >= k.regionWord + 8) {
            k.pushMask |= 1u << word;
            k.pushWords[word] = value;
        }
    return qmd && program(programs, prog, r);
}

int runRaw(char **argv)
{
    uint32_t threads[3], groups[3];
    if (!size3(argv[5], threads) || !size3(argv[6], groups))
        return 2;
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[2], 0, err)) {
        fprintf(stderr, "NO DEVICE %s\n", err.c_str());
        return 1;
    }
    // the pack's kernel of key argv[4] (pack mode), or the record of an NVK run's log
    raw::ComputeRecord rec;
    const bool packed = !strcmp(argv[1], "pack");
    if (!packed && !record(argv[3], argv[4], rec)) {
        fprintf(stderr, "no kernel record in %s\n", argv[3]);
        return 1;
    }
    const raw::ComputeState *k =
        packed ? raw::computeState(dev, raw::computeKeyHash(argv[4])) : raw::placeCompute(dev, rec);
    raw::CommandBuffer *cb = dev.begin();
    if (!k || !cb) {
        fprintf(stderr, "no kernel or command buffer\n");
        return 1;
    }
    raw::ComputeEncoder enc(*cb, dev);
    enc.setState(k);
    for (uint32_t s = 0; s < kSlots; s++)
        enc.setBytes(s, tag(s).data(), kTagBytes);
    raw::Buffer big;
    if (const uint32_t bytes = bigBuffer()) {
        if (!dev.createBuffer(bytes, big))
            return 1;
        fillBig(big.cpu, bytes);
        enc.setBuffer(kBigSlot, &big, 0);
    }
    raw::Buffer shifted;
    if (uint32_t slot, offset; off16(slot, offset)) {
        if (!dev.createBuffer(offset + kTagBytes, shifted))
            return 1;
        memset(shifted.cpu, 0, offset);
        memcpy(shifted.cpu + offset, tag(slot).data(), kTagBytes);
        enc.setBuffer(slot, &shifted, offset);
    }
    std::vector<raw::Texture> textures(kTextures);
    std::vector<raw::Sampler> samplers(kTextures);
    for (uint32_t t = 0; t < kTextures; t++) {
        const char *format = getenv("FRAME_TEXTURE_FORMAT");
        if (!dev.createTexture(16 + t, 8 + t, format ? uint32_t(atoi(format)) : kRgba8Unorm, textures[t], false,
                               getenv("FRAME_TILED") != nullptr))
            return 1;
        if (textures[t].cpu && !textures[t].blockLinear) {
            const std::vector<uint32_t> v = texels(t);
            for (uint32_t y = 0; y < 8 + t; y++)
                memcpy(textures[t].cpu + y * textures[t].pitch, &v[y * (16 + t)], 4 * (16 + t));
            cb->clean(textures[t].cpu, uint64_t(textures[t].pitch) * (8 + t));
        }
        raw::SamplerDesc sd;
        sd.lodMax = 1 + t / 2.0f;
        if (!dev.createSampler(sd, samplers[t]))
            return 1;
        enc.setTexture(t, &textures[t]);
        enc.setSampler(t, &samplers[t]);
    }
    const char *why = nullptr;
    enc.setSerial(true);
    for (uint32_t n = 0; n < dispatches(); n++)
        if (!enc.dispatch(groups, threads, &why)) {
            fprintf(stderr, "refused: %s\n", why);
            return 1;
        }
    enc.end();
    uint32_t seq = 0;
    if (!cb->commit(seq) || !dev.wait(seq))
        return 1;
    pushcount_report("computeframe raw");
    fprintf(stderr, "raw: %llu dispatches, %llu programs placed\n", (unsigned long long)dev.stats.dispatches.load(),
            (unsigned long long)dev.stats.programsPlaced.load());
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc >= 8 && (!strcmp(argv[1], "nvk") || !strcmp(argv[1], "facade")))
            return runNvk(argv);
        if (argc >= 7 && (!strcmp(argv[1], "raw") || !strcmp(argv[1], "pack")))
            return runRaw(argv);
        fprintf(stderr, "usage: computeframe nvk <nvk> <cache> <metallib> <kernel> <threads x,y,z> <groups x,y,z>\n"
                        "       computeframe raw <pack.bin> <nvk log> <programs.bin> <threads x,y,z> <groups x,y,z>\n"
                        "       computeframe pack <pack.bin> - <key> <threads x,y,z> <groups x,y,z>\n");
        return 2;
    }
}
