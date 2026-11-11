// SPDX-License-Identifier: MIT
// A corpus's compute dispatch shapes as the direct compile makes their kernels (runtime/raw_direct_compute.h), read as the
// kernel probe read them (kernel_shapes.h): the pack generator's directpack.mm and the host check test/kernelcheck.mm.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <memory>
#include "../runtime/RawDirect.h"
#include "kernel_shapes.h"

namespace probe {

// `s`'s dispatch at `threads` threads a threadgroup over its textures `bound` (textures()' of its own)
inline void dispatchOf(const Shape &s, const uint32_t threads[3], NSArray<id<MTLTexture>> *bound,
                       std::unique_ptr<raw::KernelDispatch> &out)
{
    std::map<uint32_t, uint32_t> memory;
    std::istringstream text(s.threadgroupMemory);
    std::string pair;
    while (std::getline(text, pair, ','))
        memory[uint32_t(strtoul(pair.c_str(), nullptr, 10))] =
            uint32_t(strtoul(pair.c_str() + pair.find(':') + 1, nullptr, 10));
    out = std::make_unique<raw::KernelDispatch>(threads, memory);
    for (size_t i = 0; i < s.textures.size(); i++) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = bound[i].pixelFormat;
        d.textureType = bound[i].textureType;
        d.usage = bound[i].usage;
        d.mipmapLevelCount = bound[i].mipmapLevelCount;
        d.arrayLength = bound[i].arrayLength;
        d.sampleCount = bound[i].sampleCount;
        out->texture(s.textures[i].slot, d);
    }
    for (const raw::BoundSampler &b : s.samplers)
        out->sampler(b.slot, samplerDescriptor(b));
}

struct Kernel {
    std::string line, key;
    Shape shape;
    MTLComputePipelineDescriptor *descriptor = nil;
    NSArray<id<MTLTexture>> *textures = nil;
};

// Each dispatch shape of `text`'s D lines the kernel probe made (its state made, its key once): `each(kernel)`, and
// `refused(line, why)` for one it would not make
template <typename Each, typename Refused>
void eachKernel(id<MTLDevice> dev, NSString *text, Each each, Refused refused)
{
    std::set<std::string> seen;
    for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
        if (![l hasPrefix:@"D ok=1 "])
            continue;
        Kernel k;
        k.line = l.UTF8String;
        std::string why;
        raw::AirHash air;
        id<MTLFunction> f = nil;
        if (!parse(k.line, k.shape, why) || !(f = function(dev, k.shape, air)) || !state(dev, k.shape, f, why) ||
            !(k.textures = textures(dev, k.shape.textures, why))) {
            refused(k.line, why.empty() ? "no function " + k.shape.function : why);
            continue;
        }
        k.key = key(k.shape, air);
        if (!seen.insert(k.key).second)
            continue;
        k.descriptor = descriptor(k.shape, f);
        each(k);
    }
}

}  // namespace probe
