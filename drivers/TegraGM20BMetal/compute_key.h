// SPDX-License-Identifier: MIT
// A compute state's pack key at one threadgroup shape, as the pack's generator (pack/kernel_shapes.h) and the facade
// (TegraGM20BMetalCompute.mm) both make it: what m2v compiles a kernel's program from. Its function by its AIR (metallib.h: the
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
#include <optional>
#include <string>
#include <vector>
#include "metallib.h"
#include "pack.h"

namespace gm20b {

// A texture bound at `slot`, as MTLTexture answers it; `planes`: an RGB10A8_2P_XR10 texture held in Metal's two planes
// (xr_pass.h), whose kernels read and write the planes apart; `addressMap`: the address map the driver samples a
// converted texture with (Texture::addressMap), which the kernel's samplers are compiled under
struct BoundTexture {
    uint32_t slot;
    uint64_t pixelFormat, textureType, levels, arrayLength;
    uint8_t swizzle[4];
    bool planes = false;
    uint8_t addressMap = 0;
};

constexpr char kTwoPlaneTexture[] = ":xr2p";
constexpr char kAddressMap[] = ":am";

// "<slot>:<pixelFormat>:<textureType>:<levels>:<arrayLength>:<swizzle, 4 hex bytes>[:xr2p][:am<map>],..." in slot order,
// ":xr2p" after a texture held in two planes, ":am<map>" after a converted one
inline std::string computeTextures(const BoundTexture *t, size_t count)
{
    std::string text;
    for (size_t i = 0; i < count; i++) {
        char one[128];
        snprintf(one, sizeof one, "%s%u:%llu:%llu:%llu:%llu:%02x%02x%02x%02x%s", text.empty() ? "" : ",", t[i].slot,
                 (unsigned long long)t[i].pixelFormat, (unsigned long long)t[i].textureType,
                 (unsigned long long)t[i].levels, (unsigned long long)t[i].arrayLength, t[i].swizzle[0],
                 t[i].swizzle[1], t[i].swizzle[2], t[i].swizzle[3], t[i].planes ? kTwoPlaneTexture : "");
        text += one;
        if (t[i].addressMap)
            text += kAddressMap + std::to_string(t[i].addressMap);
    }
    return text;
}

// One texture of computeTextures' text; false: not one
inline bool parseComputeTexture(const std::string &one, BoundTexture &t)
{
    unsigned slot, swizzle;
    unsigned long long format, type, levels, array;
    int end = 0;
    if (sscanf(one.c_str(), "%u:%llu:%llu:%llu:%llu:%x%n", &slot, &format, &type, &levels, &array, &swizzle, &end) != 6)
        return false;
    const bool planes = !one.compare(end, sizeof kTwoPlaneTexture - 1, kTwoPlaneTexture);
    size_t at = size_t(end) + (planes ? sizeof kTwoPlaneTexture - 1 : 0);
    uint8_t map = 0;
    if (!one.compare(at, sizeof kAddressMap - 1, kAddressMap)) {
        at += sizeof kAddressMap - 1;
        if (at + 1 != one.size() || one[at] < '1' || one[at] > '3')
            return false;
        map = uint8_t(one[at++] - '0');
    }
    if (at != one.size())
        return false;
    t = {slot,   format, type,
         levels, array,  {uint8_t(swizzle >> 24), uint8_t(swizzle >> 16), uint8_t(swizzle >> 8), uint8_t(swizzle)},
         planes, map};
    return true;
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

inline std::string airText(const AirHash &air)
{
    std::string text;
    char b[4];
    for (uint8_t x : air) {
        snprintf(b, sizeof b, "%02x", x);
        text += b;
    }
    return text;
}

// What a compute state is made from, as its creation names it: every dispatch shape's key starts with it
inline std::string computeStateKey(const AirHash &air, const std::string &constants, uint32_t maxThreads,
                                   bool multipleOfWidth)
{
    char b[48];
    snprintf(b, sizeof b, " | max=%u tgm=%d", maxThreads, multipleOfWidth ? 1 : 0);
    return airText(air) + " | " + constants + b;
}

// A compute state's linked function by its AIR, constants, list and exported name.
struct LinkedComputeFunction {
    bool hidden = false;
    AirHash air = {};
    std::string constants;
    bool binary = false;
    std::string symbol = {}, name = {};
    std::optional<std::string> group = std::nullopt;
};

// Lists retain descriptor order, groups by name. Names distinguish binaries, grouped functions and renamed imports;
// unchanged kl/kp entries keep their keys.
inline std::string computeLinkedText(const std::vector<LinkedComputeFunction> &linked)
{
    std::string text;
    for (const LinkedComputeFunction &f : linked) {
        text += (f.group    ? " | kg " + std::to_string(f.group->size()) + ":" + *f.group + " "
                 : f.binary ? " | kb "
                 : f.hidden ? " | kp "
                            : " | kl ") +
                airText(f.air) + (f.constants.empty() ? "" : " " + f.constants);
        if (f.group || f.binary || (!f.name.empty() && f.name != f.symbol))
            text += " name=" + std::to_string(f.symbol.size()) + ":" + f.symbol + std::to_string(f.name.size()) + ":" +
                    f.name;
    }
    return text;
}

// A compute state's constants as its keys carry them: its function's, its stage input's (stageInText), then its linked
// functions'. The client keys a state by it and the compiler remakes that key from a request's fields by it.
inline std::string computeStateConstants(const std::string &function, const std::string &stageIn,
                                         const std::vector<LinkedComputeFunction> &linked)
{
    return function + stageIn + computeLinkedText(linked);
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

}  // namespace gm20b
