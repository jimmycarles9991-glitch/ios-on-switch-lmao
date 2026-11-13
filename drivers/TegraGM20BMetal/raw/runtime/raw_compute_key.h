// SPDX-License-Identifier: MIT
// A compute state's pack key at one threadgroup shape, as the pack's generator (pack/kernel_shapes.h) and the facade
// (RawMetalCompute.mm) both make it: what m2v compiles a kernel's program from. Its function by its AIR (raw_metallib.h: the
// same bitcode whatever library or path it came by), the function constants as the journal writes them, the descriptor's
// thread limits, the threads per threadgroup (m2v's local size), the threadgroup memory lengths (m2v compiles each into
// the program, as NAK sizes shared memory by it), and the textures and samplers bound: m2v picks a kernel's program and
// push words by each texture's format class with its sampling (metal2vulkan texel_format, BoundTexelFormat), so a record
// holds for the textures and samplers it was made with (the frontend journal's tex= and smp= fields,
// NativeBridgeMetalFrontend.mm texturesText, samplersText), and holds each sampler's header itself.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "raw_metallib.h"
#include "raw_pack.h"

namespace raw {

// A texture bound at `slot`, as MTLTexture answers it
struct BoundTexture {
    uint32_t slot;
    uint64_t pixelFormat, textureType, levels, arrayLength;
    uint8_t swizzle[4];
};

// "<slot>:<pixelFormat>:<textureType>:<levels>:<arrayLength>:<swizzle, 4 hex bytes>,..." in slot order
inline std::string computeTextures(const BoundTexture *t, size_t count)
{
    std::string text;
    for (size_t i = 0; i < count; i++) {
        char one[128];
        snprintf(one, sizeof one, "%s%u:%llu:%llu:%llu:%llu:%02x%02x%02x%02x", text.empty() ? "" : ",", t[i].slot,
                 (unsigned long long)t[i].pixelFormat, (unsigned long long)t[i].textureType,
                 (unsigned long long)t[i].levels, (unsigned long long)t[i].arrayLength, t[i].swizzle[0],
                 t[i].swizzle[1], t[i].swizzle[2], t[i].swizzle[3]);
        text += one;
    }
    return text;
}

// A sampler bound at `slot`: its MTLSamplerDescriptor's fields
struct BoundSampler {
    uint32_t slot;
    uint64_t minFilter, magFilter, mipFilter, sAddress, tAddress, rAddress, maxAnisotropy, compare, border;
    float lodMin, lodMax;
    bool normalized, lodAverage;
};

// "<slot>:<min>:<mag>:<mip>:<s>:<t>:<r>:<lodMin bits>:<lodMax bits>:<anisotropy>:<compare>:<border>:<normalized>:<average>,..."
// in slot order, the floats' bits in hex
inline std::string computeSamplers(const BoundSampler *s, size_t count)
{
    std::string text;
    for (size_t i = 0; i < count; i++) {
        uint32_t lodMin, lodMax;
        memcpy(&lodMin, &s[i].lodMin, 4);
        memcpy(&lodMax, &s[i].lodMax, 4);
        char one[192];
        snprintf(one, sizeof one, "%s%u:%llu:%llu:%llu:%llu:%llu:%llu:%x:%x:%llu:%llu:%llu:%d:%d",
                 text.empty() ? "" : ",", s[i].slot, (unsigned long long)s[i].minFilter,
                 (unsigned long long)s[i].magFilter, (unsigned long long)s[i].mipFilter,
                 (unsigned long long)s[i].sAddress, (unsigned long long)s[i].tAddress,
                 (unsigned long long)s[i].rAddress, lodMin, lodMax, (unsigned long long)s[i].maxAnisotropy,
                 (unsigned long long)s[i].compare, (unsigned long long)s[i].border, s[i].normalized ? 1 : 0,
                 s[i].lodAverage ? 1 : 0);
        text += one;
    }
    return text;
}

// What a compute state is made from, as its creation names it: every dispatch shape's key starts with it
inline std::string computeStateKey(const AirHash &air, const std::string &constants, uint32_t maxThreads,
                                   bool multipleOfWidth)
{
    std::string key;
    char b[48];
    for (uint8_t x : air) {
        snprintf(b, sizeof b, "%02x", x);
        key += b;
    }
    snprintf(b, sizeof b, " | max=%u tgm=%d", maxThreads, multipleOfWidth ? 1 : 0);
    return key + " | " + constants + b;
}

// A compute state as an archive inventory names it (pack/archives.py): its library function, not its AIR
inline std::string computeArchiveKey(const std::string &function, const std::string &constants, uint32_t maxThreads,
                                     bool multipleOfWidth)
{
    char b[48];
    snprintf(b, sizeof b, " | max=%u tgm=%d", maxThreads, multipleOfWidth ? 1 : 0);
    return "k " + function + (constants.empty() ? "" : " " + constants) + b;
}

inline std::string computeKey(const AirHash &air, const std::string &constants, uint32_t maxThreads,
                              bool multipleOfWidth, const uint32_t threads[3], const std::string &threadgroupMemory,
                              const std::string &textures, const std::string &samplers, bool regionMode = false)
{
    char b[64];
    snprintf(b, sizeof b, " | tg=%u,%u,%u | tgmem=", threads[0], threads[1], threads[2]);
    return computeStateKey(air, constants, maxThreads, multipleOfWidth) + b + threadgroupMemory + " | tex=" + textures +
           " | smp=" + samplers + (regionMode ? " | region=1" : "");
}

inline uint64_t computeKeyHash(const std::string &key)
{
    return Pack::fnv(key.data(), key.size());
}

}  // namespace raw
