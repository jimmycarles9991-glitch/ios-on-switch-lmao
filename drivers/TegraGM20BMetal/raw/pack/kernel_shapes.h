// SPDX-License-Identifier: MIT
// A corpus D line (the frontend journal's compute dispatch: a compute state and the threadgroup shape, memory, textures and
// samplers it was dispatched with) as the pack generator reads it: its function made from its library, its state's descriptor,
// its textures, each (16 + slot) x (8 + slot), and its raw_compute_key.h key. The generator's direct records (directpack.mm) and
// the host check (raw/test/kernelcheck.mm) read lines alike.
// Libraries: lib=<path> under PROBE_LIB_ROOT (the crawl's root), lib=data:<SHA-256> as <PROBE_DATA_LIBS>/<SHA-256>.metallib.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../runtime/raw_compute_key.h"
#import "../runtime/RawPipelineKey.h"
#import "../runtime/RawRenderKey.h"

namespace probe {

struct Shape {
    uint32_t threads[3] = {};
    uint32_t edge[3] = {};
    bool regionMode = false;
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

template <typename Each> void eachRegionShape(const Shape &original, Each each)
{
    for (uint8_t mask = 0; mask < 8; ++mask) {
        Shape shape = original;
        bool present = true;
        for (uint8_t axis = 0; axis < 3; ++axis)
            if (mask & (1u << axis)) {
                present &= original.edge[axis] != 0;
                shape.threads[axis] = original.edge[axis];
            }
        if (present)
            each(shape, mask);
    }
}

// smp=<slot>:<min>:<mag>:<mip>:<s>:<t>:<r>:<lodMin bits>:<lodMax bits>:<anisotropy>:<compare>:<border>:<normalized>:<average>,...
inline bool parseSamplers(const std::string &text, std::vector<raw::BoundSampler> &out, std::string &why)
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

inline MTLSamplerDescriptor *samplerDescriptor(const raw::BoundSampler &b)
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
    return d;
}

inline id<MTLSamplerState> sampler(id<MTLDevice> dev, const raw::BoundSampler &b)
{
    return [dev newSamplerStateWithDescriptor:samplerDescriptor(b)];
}

// tex=<slot>:<pixelFormat>:<textureType>:<levels>:<arrayLength>:<swizzle>,... (raw_compute_key.h computeTextures)
inline bool parseTextures(const std::string &text, std::vector<raw::BoundTexture> &out, std::string &why)
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

// The field `name`= of a D line's head, before its first " | "
inline bool field(const std::string &line, const char *name, std::string &out)
{
    const std::string tag = std::string(" ") + name + "=";
    const size_t at = line.find(tag);
    if (at == std::string::npos || at >= line.find(" | "))
        return false;
    const size_t from = at + tag.size();
    out = line.substr(from, line.find_first_of(" |", from) - from);
    return true;
}

// A D line: "D ok=1 tg=x,y,z edge=a,b,c threads=t | form=... | k <name> lib=<library> [spec] <constants> | ..."; false where
// the raw path dispatches no such shape (linked functions or an invalid local size)
inline bool parse(const std::string &line, Shape &s, std::string &why)
{
    uint32_t threadsForm;
    if (sscanf(line.c_str(), "D ok=1 tg=%u,%u,%u edge=%u,%u,%u threads=%u", &s.threads[0], &s.threads[1], &s.threads[2],
               &s.edge[0], &s.edge[1], &s.edge[2], &threadsForm) != 7) {
        why = "not a D line";
        return false;
    }
    if (threadsForm > 1) {
        why = "an invalid dispatch form";
        return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
        if (!s.threads[i] || s.threads[i] > 1024 || s.edge[i] >= s.threads[i] || (!threadsForm && s.edge[i])) {
            why = "an invalid local size or edge";
            return false;
        }
        s.regionMode |= s.edge[i] != 0;
    }
    std::string text;
    field(line, "tgmem", s.threadgroupMemory);
    if ((field(line, "tex", text) && !parseTextures(text, s.textures, why)) ||
        (field(line, "smp", text) && !parseSamplers(text, s.samplers, why)))
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
    std::string item;
    while (head >> item) {
        if (item.rfind("form=", 0) == 0)
            s.form = item.substr(5);
        else if (item.rfind("opt=", 0) == 0)
            s.options = strtoull(item.c_str() + 4, nullptr, 10);
        else if (item.rfind("max=", 0) == 0)
            s.maxThreads = uint32_t(strtoul(item.c_str() + 4, nullptr, 10));
        else if (item.rfind("tgm=", 0) == 0)
            s.multipleOfWidth = item[4] == '1';
    }
    const size_t end = line.find(" | ", k + 5);
    std::istringstream fn(line.substr(k + 5, (end == std::string::npos ? line.size() : end) - k - 5));
    fn >> s.function;
    while (fn >> item && item != "|") {
        if (item.rfind("lib=", 0) == 0)
            s.library = item.substr(4);
        else if (item == "spec")
            s.specialized = true;
        else
            s.constants += (s.constants.empty() ? "" : " ") + item;
    }
    return !s.function.empty() && !s.library.empty();
}

// "#<index>:<MTLDataType>:<hex bytes>" or "<name>:<MTLDataType>:<hex bytes>" (RawMetalLibrary.mm constantsText)
inline MTLFunctionConstantValues *constantValues(const std::string &text)
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

inline id<MTLLibrary> library(id<MTLDevice> dev, const std::string &name)
{
    static NSMutableDictionary<NSString *, id<MTLLibrary>> *loaded = [NSMutableDictionary dictionary];
    NSString *key = [NSString stringWithFormat:@"%p %s", (void *)dev, name.c_str()];
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

// The shape's function made from its library, and its AIR's hash; nil where the library holds no such function
inline id<MTLFunction> function(id<MTLDevice> dev, const Shape &s, raw::AirHash &air)
{
    id<MTLLibrary> lib = library(dev, s.library);
    NSError *e = nil;
    id<MTLFunction> f = !lib                                    ? nil
                        : s.specialized || !s.constants.empty() ? [lib newFunctionWithName:@(s.function.c_str())
                                                                            constantValues:constantValues(s.constants)
                                                                                     error:&e]
                                                                : [lib newFunctionWithName:@(s.function.c_str())];
    const void *bytes = nullptr;
    size_t length = 0;
    std::string entry;
    std::map<std::string, raw::AirHash> hashes;
    if (!f || !raw::functionLibrary(f, bytes, length, entry) ||
        !raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes) || !hashes.count(s.function))
        return nil;
    air = hashes[s.function];
    return f;
}

// The state's descriptor as its form makes it: "f" newComputePipelineStateWithFunction:'s defaults
inline MTLComputePipelineDescriptor *descriptor(const Shape &s, id<MTLFunction> f)
{
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = f;
    if (s.form == "d") {
        d.maxTotalThreadsPerThreadgroup = s.maxThreads;
        d.threadGroupSizeIsMultipleOfThreadExecutionWidth = s.multipleOfWidth;
    }
    return d;
}

inline id<MTLComputePipelineState> state(id<MTLDevice> dev, const Shape &s, id<MTLFunction> f, std::string &why)
{
    NSError *e = nil;
    id<MTLComputePipelineState> made = s.form == "d"
                                           ? [dev newComputePipelineStateWithDescriptor:descriptor(s, f)
                                                                                options:MTLPipelineOption(s.options)
                                                                             reflection:nil
                                                                                  error:&e]
                                           : [dev newComputePipelineStateWithFunction:f
                                                                              options:MTLPipelineOption(s.options)
                                                                           reflection:nil
                                                                                error:&e];
    if (!made)
        why = e.localizedDescription.UTF8String ?: "refused";
    return made;
}

// The shape's textures, each of its tag size, (16 + slot) x (8 + slot): read and written, but read only where swizzled
// (Metal makes no swizzled texture shaders write)
inline NSArray<MTLTextureDescriptor *> *textures(const std::vector<raw::BoundTexture> &bound)
{
    NSMutableArray<MTLTextureDescriptor *> *out = [NSMutableArray array];
    for (const raw::BoundTexture &b : bound) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(b.pixelFormat)
                                                                                     width:16 + b.slot
                                                                                    height:8 + b.slot
                                                                                 mipmapped:NO];
        d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzle(b.swizzle[0]), MTLTextureSwizzle(b.swizzle[1]),
                                                  MTLTextureSwizzle(b.swizzle[2]), MTLTextureSwizzle(b.swizzle[3]));
        const bool swizzled = b.swizzle[0] != MTLTextureSwizzleRed || b.swizzle[1] != MTLTextureSwizzleGreen ||
                              b.swizzle[2] != MTLTextureSwizzleBlue || b.swizzle[3] != MTLTextureSwizzleAlpha;
        d.usage = MTLTextureUsageShaderRead | (swizzled ? 0 : MTLTextureUsageShaderWrite);
        [out addObject:d];
    }
    return out;
}

inline std::string key(const Shape &s, const raw::AirHash &air)
{
    return raw::computeKey(air, raw::constantsText(constantValues(s.constants)), s.maxThreads, s.multipleOfWidth,
                           s.threads, s.threadgroupMemory, raw::computeTextures(s.textures.data(), s.textures.size()),
                           raw::computeSamplers(s.samplers.data(), s.samplers.size()), s.regionMode);
}

}  // namespace probe
