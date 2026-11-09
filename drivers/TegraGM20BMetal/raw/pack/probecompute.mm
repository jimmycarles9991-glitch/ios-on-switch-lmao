// SPDX-License-Identifier: MIT
// raw GM20B path, pack generator: every corpus compute dispatch shape (the journal's D lines: a compute state and the
// threadgroup size it was dispatched with) made through the shipped frontend + m2v + NVK host builds over the mock GM20B, and
// dispatched once with tagged inputs, so mkpack.py can read off each kernel's record from nvdec's K lines
// (pack/nvdec_compute.cpp, NVDEC_KERNELS): NVK's QMD, the program, and where each Metal argument lands in set 0.
//
// Tags: every buffer slot is setBytes of 4096 bytes whose first word is 0xC0DE0000 | slot (the range m2v gives a slot below
// 4096 is the function's window on it); the line's tex= textures, each (16 + slot) x (8 + slot) in its format; its smp=
// samplers as they are (the key holds them: the record takes their headers as its own); threadgroup memory as its tgmem=
// field names it (<index>:<length>,...). Three command buffers per shape, their grids (3 + 3i + v)x5x2 threadgroups for the
// i-th line (its records name the dispatches by them); the second binds the same tags from buffers at another address, the
// third binds each of those buffers probe_bind.h's probeComputeCode bytes past its tag, which m2v's descriptor still starts
// at, so the root push word holding the slot's SlotBindOffset residual is the one set to that code. stdout: "KPROBE <i>
// tg=<x,y,z> grid=<x,y,z> key=<raw_compute_key.h key>" before each one is committed, "KREFUSED <i> <why>" for a shape not
// made.
// Libraries: lib=<path> under PROBE_LIB_ROOT (the crawl's root), lib=data:<SHA-256> as <PROBE_DATA_LIBS>/<SHA-256>.metallib.
// usage: probecompute <nvk dylib> <cache dir> <corpus.log>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../runtime/raw_compute_key.h"
#include "probe_bind.h"

extern "C" void pushcount_report(const char *what);

namespace {

constexpr uint32_t kSlots = 31, kTagBytes = 4096, kBufferOffset = 256;

std::vector<uint32_t> tag(uint32_t slot)
{
    std::vector<uint32_t> t(kTagBytes / 4);
    t[0] = 0xC0DE0000u | slot;
    for (uint32_t k = 1; k < t.size(); k++)
        t[k] = 0xA0000000u | slot << 16 | k;
    return t;
}

// The line's textures, each of its tag size: (16 + slot) x (8 + slot)
NSArray<id<MTLTexture>> *textures(id<MTLDevice> dev, const std::vector<raw::BoundTexture> &bound, std::string &why)
{
    NSMutableArray<id<MTLTexture>> *out = [NSMutableArray array];
    for (const raw::BoundTexture &b : bound) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(b.pixelFormat)
                                                                                     width:16 + b.slot
                                                                                    height:8 + b.slot
                                                                                 mipmapped:NO];
        d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzle(b.swizzle[0]), MTLTextureSwizzle(b.swizzle[1]),
                                                  MTLTextureSwizzle(b.swizzle[2]), MTLTextureSwizzle(b.swizzle[3]));
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d];
        if (!t) {
            d.usage = MTLTextureUsageShaderRead;
            t = [dev newTextureWithDescriptor:d];
        }
        if (!t) {
            why = "no texture of format " + std::to_string(b.pixelFormat);
            return nil;
        }
        [out addObject:t];
    }
    return out;
}

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

struct Shape {
    uint32_t threads[3] = {};
    std::string form;  // "f" or "d"
    uint64_t options = 0;
    uint32_t maxThreads = 0;
    bool multipleOfWidth = false;
    std::string function, library, constants;
    bool specialized = false;
    std::string threadgroupMemory;  // "<index>:<length>,..." (raw_compute_key.h)
    std::vector<raw::BoundTexture> textures;
    std::vector<raw::BoundSampler> samplers;
};

// smp=<slot>:<min>:<mag>:<mip>:<s>:<t>:<r>:<lodMin bits>:<lodMax bits>:<anisotropy>:<compare>:<border>:<normalized>:<average>,...
bool parseSamplers(const std::string &text, std::vector<raw::BoundSampler> &out, std::string &why)
{
    std::istringstream in(text);
    std::string one;
    while (std::getline(in, one, ',')) {
        raw::BoundSampler b = {};
        unsigned long long f[9];
        unsigned slot, lo, hi;
        int normalized, average;
        if (sscanf(one.c_str(), "%u:%llu:%llu:%llu:%llu:%llu:%llu:%x:%x:%llu:%llu:%llu:%d:%d", &slot, &f[0], &f[1],
                   &f[2], &f[3], &f[4], &f[5], &lo, &hi, &f[6], &f[7], &f[8], &normalized, &average) != 14) {
            why = "an smp= field it cannot read";
            return false;
        }
        b.slot = slot;
        b.minFilter = f[0];
        b.magFilter = f[1];
        b.mipFilter = f[2];
        b.sAddress = f[3];
        b.tAddress = f[4];
        b.rAddress = f[5];
        b.maxAnisotropy = f[6];
        b.compare = f[7];
        b.border = f[8];
        memcpy(&b.lodMin, &lo, 4);
        memcpy(&b.lodMax, &hi, 4);
        b.normalized = normalized;
        b.lodAverage = average;
        out.push_back(b);
    }
    return true;
}

id<MTLSamplerState> sampler(id<MTLDevice> dev, const raw::BoundSampler &b)
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = MTLSamplerMinMagFilter(b.minFilter);
    d.magFilter = MTLSamplerMinMagFilter(b.magFilter);
    d.mipFilter = MTLSamplerMipFilter(b.mipFilter);
    d.sAddressMode = MTLSamplerAddressMode(b.sAddress);
    d.tAddressMode = MTLSamplerAddressMode(b.tAddress);
    d.rAddressMode = MTLSamplerAddressMode(b.rAddress);
    d.lodMinClamp = b.lodMin;
    d.lodMaxClamp = b.lodMax;
    d.maxAnisotropy = b.maxAnisotropy;
    d.compareFunction = MTLCompareFunction(b.compare);
    d.borderColor = MTLSamplerBorderColor(b.border);
    d.normalizedCoordinates = b.normalized;
    d.lodAverage = b.lodAverage;
    return [dev newSamplerStateWithDescriptor:d];
}

// tex=<slot>:<pixelFormat>:<textureType>:<levels>:<arrayLength>:<swizzle>,... (raw_compute_key.h computeTextures)
bool parseTextures(const std::string &text, std::vector<raw::BoundTexture> &out, std::string &why)
{
    std::istringstream in(text);
    std::string one;
    while (std::getline(in, one, ',')) {
        raw::BoundTexture t = {};
        unsigned slot, swizzle;
        unsigned long long format, type, levels, array;
        if (sscanf(one.c_str(), "%u:%llu:%llu:%llu:%llu:%x", &slot, &format, &type, &levels, &array, &swizzle) != 6) {
            why = "a tex= field it cannot read";
            return false;
        }
        if (type != MTLTextureType2D || levels != 1 || array != 1) {
            why = "a texture of another type than a one-level 2D one";
            return false;
        }
        t = {slot,   format, type,
             levels, array,  {uint8_t(swizzle >> 24), uint8_t(swizzle >> 16), uint8_t(swizzle >> 8), uint8_t(swizzle)}};
        out.push_back(t);
    }
    return true;
}

// A D line: "D ok=1 tg=x,y,z edge=a,b,c threads=t | form=... | k <name> lib=<library> [spec] <constants> | ..."; false where
// the raw path dispatches no such shape (a grid with an edge, linked functions)
bool parse(const std::string &line, Shape &s, std::string &why)
{
    uint32_t edge[3], threadsForm;
    if (sscanf(line.c_str(), "D ok=1 tg=%u,%u,%u edge=%u,%u,%u threads=%u", &s.threads[0], &s.threads[1], &s.threads[2],
               &edge[0], &edge[1], &edge[2], &threadsForm) != 7) {
        why = "not a D line";
        return false;
    }
    if (edge[0] || edge[1] || edge[2]) {
        why = "a grid with an edge";
        return false;
    }
    if (const size_t at = line.find(" tgmem="); at != std::string::npos && at < line.find(" | "))
        s.threadgroupMemory = line.substr(at + 7, line.find_first_of(" |", at + 7) - at - 7);
    if (const size_t at = line.find(" tex=");
        at != std::string::npos && at < line.find(" | ") &&
        !parseTextures(line.substr(at + 5, line.find_first_of(" |", at + 5) - at - 5), s.textures, why))
        return false;
    if (const size_t at = line.find(" smp=");
        at != std::string::npos && at < line.find(" | ") &&
        !parseSamplers(line.substr(at + 5, line.find_first_of(" |", at + 5) - at - 5), s.samplers, why))
        return false;
    const size_t state = line.find(" | form=");
    const size_t k = line.find(" | k ", state);
    if (state == std::string::npos || k == std::string::npos) {
        why = "no state";
        return false;
    }
    if (line.find(" | l ", k) != std::string::npos || line.find(" | p ", k) != std::string::npos) {
        why = "linked functions";
        return false;
    }
    std::istringstream head(line.substr(state + 3, k - state - 3));
    std::string field;
    while (head >> field) {
        if (field.rfind("form=", 0) == 0)
            s.form = field.substr(5);
        else if (field.rfind("opt=", 0) == 0)
            s.options = strtoull(field.c_str() + 4, nullptr, 10);
        else if (field.rfind("max=", 0) == 0)
            s.maxThreads = uint32_t(strtoul(field.c_str() + 4, nullptr, 10));
        else if (field.rfind("tgm=", 0) == 0)
            s.multipleOfWidth = field[4] == '1';
    }
    const size_t end = line.find(" | ", k + 5);
    std::istringstream fn(line.substr(k + 5, (end == std::string::npos ? line.size() : end) - k - 5));
    fn >> s.function;
    while (fn >> field && field != "|") {
        if (field.rfind("lib=", 0) == 0)
            s.library = field.substr(4);
        else if (field == "spec")
            s.specialized = true;
        else
            s.constants += (s.constants.empty() ? "" : " ") + field;
    }
    return !s.function.empty() && !s.library.empty();
}

// "#<index>:<MTLDataType>:<hex bytes>" or "<name>:<MTLDataType>:<hex bytes>" (RawMetalLibrary.mm constantsText)
MTLFunctionConstantValues *constantValues(const std::string &text)
{
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    std::istringstream in(text);
    std::string c;
    while (in >> c) {
        const size_t a = c.find(':'), b = c.find(':', a + 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        const MTLDataType type = MTLDataType(strtoul(c.c_str() + a + 1, nullptr, 10));
        std::vector<uint8_t> bytes;
        for (size_t i = b + 1; i + 1 < c.size(); i += 2)
            bytes.push_back(uint8_t(strtoul(c.substr(i, 2).c_str(), nullptr, 16)));
        if (c[0] == '#')
            [values setConstantValue:bytes.data() type:type atIndex:strtoul(c.c_str() + 1, nullptr, 10)];
        else
            [values setConstantValue:bytes.data() type:type withName:@(c.substr(0, a).c_str())];
    }
    return values;
}

id<MTLLibrary> library(id<MTLDevice> dev, const std::string &name)
{
    static NSMutableDictionary<NSString *, id<MTLLibrary>> *loaded = [NSMutableDictionary dictionary];
    NSString *key = @(name.c_str());
    if (!loaded[key]) {
        std::string path;
        if (name.rfind("data:", 0) == 0) {
            const char *dir = getenv("PROBE_DATA_LIBS");
            path = std::string(dir ? dir : ".") + "/" + name.substr(5) + ".metallib";
        } else {
            const char *root = getenv("PROBE_LIB_ROOT");
            path = std::string(root ? root : "") + name;
        }
        NSError *e = nil;
        if (id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())] error:&e])
            loaded[key] = l;
    }
    return loaded[key];
}

id<MTLComputePipelineState> state(id<MTLDevice> dev, const Shape &s, id<MTLFunction> f, std::string &why)
{
    NSError *e = nil;
    id<MTLComputePipelineState> made = nil;
    if (s.form == "d") {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = f;
        d.maxTotalThreadsPerThreadgroup = s.maxThreads;
        d.threadGroupSizeIsMultipleOfThreadExecutionWidth = s.multipleOfWidth;
        made = [dev newComputePipelineStateWithDescriptor:d
                                                  options:MTLPipelineOption(s.options)
                                               reflection:nil
                                                    error:&e];
    } else {
        made = [dev newComputePipelineStateWithFunction:f options:MTLPipelineOption(s.options) reflection:nil error:&e];
    }
    if (!made)
        why = e.localizedDescription.UTF8String ?: "refused";
    return made;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: probecompute <nvk dylib> <cache dir> <corpus.log>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        // the second dispatch's buffers lie elsewhere: each slot's tagged bytes 256 bytes into a buffer of its own
        NSMutableArray<id<MTLBuffer>> *buffers = [NSMutableArray array];
        for (uint32_t slot = 0; slot < kSlots; slot++) {
            id<MTLBuffer> b = [dev newBufferWithLength:kBufferOffset + kTagBytes options:MTLResourceStorageModeShared];
            memcpy(static_cast<uint8_t *>(b.contents) + kBufferOffset, tag(slot).data(), kTagBytes);
            [buffers addObject:b];
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[3]) encoding:NSUTF8StringEncoding error:nil];
        std::map<std::string, bool> seen;
        unsigned index = 0, made = 0, refused = 0, failed = 0;
        for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
            if (![l hasPrefix:@"D ok=1 "])
                continue;
            const std::string line = l.UTF8String;
            Shape s;
            std::string why;
            const unsigned i = index++;
            if (!parse(line, s, why)) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            id<MTLLibrary> lib = library(dev, s.library);
            NSError *e = nil;
            id<MTLFunction> f = !lib ? nil
                                : s.specialized || !s.constants.empty()
                                    ? [lib newFunctionWithName:@(s.function.c_str())
                                                constantValues:constantValues(s.constants)
                                                         error:&e]
                                    : [lib newFunctionWithName:@(s.function.c_str())];
            const void *bytes = nullptr;
            size_t length = 0;
            std::map<std::string, raw::AirHash> air;
            if (!f || !native_bridge_metal_function_metallib(f, &bytes, &length) ||
                !raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, air) || !air.count(s.function)) {
                printf("KREFUSED %u no function %s in %s\n", i, s.function.c_str(), s.library.c_str());
                refused++;
                continue;
            }
            id<MTLComputePipelineState> ps = state(dev, s, f, why);
            if (!ps) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            const std::string key =
                raw::computeKey(air[s.function], s.constants, s.maxThreads, s.multipleOfWidth, s.threads,
                                s.threadgroupMemory, raw::computeTextures(s.textures.data(), s.textures.size()),
                                raw::computeSamplers(s.samplers.data(), s.samplers.size()));
            if (seen[key])
                continue;
            seen[key] = true;
            NSArray<id<MTLTexture>> *bound = textures(dev, s.textures, why);
            if (!bound) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            // the second with the buffers elsewhere, so kernels.py keeps only what their addresses do not move; the third
            // off 16 bytes, so it names their residual words
            for (uint32_t v = 0; v < 3; v++) {
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:ps];
                for (uint32_t slot = 0; slot < kSlots; slot++) {
                    if (v)
                        [enc setBuffer:buffers[slot]
                                offset:kBufferOffset + (v == 2 ? raw::probeComputeCode(slot) : 0)
                               atIndex:slot];
                    else
                        [enc setBytes:tag(slot).data() length:kTagBytes atIndex:slot];
                }
                for (NSUInteger t = 0; t < bound.count; t++)
                    [enc setTexture:bound[t] atIndex:s.textures[t].slot];
                for (const raw::BoundSampler &b : s.samplers)
                    [enc setSamplerState:sampler(dev, b) atIndex:b.slot];
                std::istringstream memory(s.threadgroupMemory);
                std::string pair;
                while (std::getline(memory, pair, ','))
                    [enc setThreadgroupMemoryLength:strtoul(pair.c_str() + pair.find(':') + 1, nullptr, 10)
                                            atIndex:strtoul(pair.c_str(), nullptr, 10)];
                const uint32_t grid[3] = {3 + 3 * i + v, 5, 2};
                [enc dispatchThreadgroups:MTLSizeMake(grid[0], grid[1], grid[2])
                    threadsPerThreadgroup:MTLSizeMake(s.threads[0], s.threads[1], s.threads[2])];
                [enc endEncoding];
                if (!v)
                    printf("KPROBE %u tg=%u,%u,%u grid=%u,%u,%u key=%s\n", i, s.threads[0], s.threads[1], s.threads[2],
                           grid[0], grid[1], grid[2], key.c_str());
                fflush(stdout);
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.error) {
                    failed++;
                    printf("KFAILED %u %s\n", i, cb.error.localizedDescription.UTF8String);
                } else if (v == 2) {
                    made++;
                }
            }
        }
        fprintf(stderr, "probecompute: %u shapes dispatched, %u refused, %u failed\n", made, refused, failed);
        pushcount_report("probecompute");
    }
    return 0;
}
