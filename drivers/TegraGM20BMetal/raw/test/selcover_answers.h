// SPDX-License-Identifier: MIT
// selcover's check of the raw device's capability and identity answers (RawMetalDeviceAnswers.mm) against m2v's device,
// whose class the raw device class still subclasses: every query the bridge installs (nmb_device_answer_selector), each
// argument query over formats 0-1023 and the GPU families, and the identity the frontend answers. The answers raw gives on
// purpose for its own layouts (constant buffer and linear texture alignments, RawMetalDevice.mm), those it takes from a
// chip-graded capability row (raw_device_answers.h chipFeature) and the features it has no form of where m2v's answers claim
// them (raw_device_answers.h) are not compared.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "bridge/NativeMetalObjects.h"

namespace answers {

inline std::vector<NSUInteger> arguments()
{
    std::vector<NSUInteger> out;
    for (NSUInteger a = 0; a < 1024; a++)
        out.push_back(a);
    for (NSUInteger first : {1001, 2001, 3001, 4001, 5001})
        for (NSUInteger a = first; a < first + 12; a++)
            out.push_back(a);
    return out;
}

inline uint64_t count(id device, SEL sel)
{
    return ((uint64_t (*)(id, SEL))objc_msgSend)(device, sel);
}

inline uint64_t flag(id device, SEL sel)
{
    return ((BOOL (*)(id, SEL))objc_msgSend)(device, sel);
}

inline uint64_t countArg(id device, SEL sel, NSUInteger a)
{
    return ((NSUInteger (*)(id, SEL, NSUInteger))objc_msgSend)(device, sel, a);
}

inline uint64_t flagArg(id device, SEL sel, NSUInteger a)
{
    return ((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(device, sel, a);
}

// How many answers differ (each printed); `compared` counts the comparisons
inline unsigned differ(id raw, id m2v, unsigned &compared)
{
    static const std::set<std::string> rawOwn = {"minConstantBufferAlignmentBytes",
                                                 "deviceLinearReadOnlyTextureAlignmentBytes",
                                                 "minimumLinearTextureAlignmentForPixelFormat:",
                                                 "supports32BitFloatFiltering",
                                                 "maxVertexAmplificationCount",
                                                 "maxVertexAmplificationFactor",
                                                 "supportsVertexAmplificationCount:",
                                                 "maxPredicatedNestingDepth",
                                                 "supportsDynamicAttributeStride",
                                                 "supportsTessellation",
                                                 "supportsIndirectTessellation",
                                                 "maxTessellationFactor"};
    unsigned bad = 0;
    auto check = [&](const char *name, NSUInteger a, uint64_t r, uint64_t m) {
        compared++;
        if (r != m && bad++ < 20)
            printf("DEVICE ANSWER DIFFERS %s (%lu): raw %llu, m2v %llu\n", name, (unsigned long)a,
                   (unsigned long long)r, (unsigned long long)m);
    };
    uint32_t kind = 0;
    const std::vector<NSUInteger> args = arguments();
    for (size_t i = 0; const char *name = nmb_device_answer_selector(i, &kind); i++) {
        if (rawOwn.count(name))
            continue;
        SEL sel = sel_registerName(name);
        if (kind == NMB_ANSWER_COUNT)
            check(name, 0, count(raw, sel), count(m2v, sel));
        else if (kind == NMB_ANSWER_FLAG)
            check(name, 0, flag(raw, sel), flag(m2v, sel));
        else
            for (NSUInteger a : args)
                check(name, a, kind == NMB_ANSWER_COUNT_ARG ? countArg(raw, sel, a) : flagArg(raw, sel, a),
                      kind == NMB_ANSWER_COUNT_ARG ? countArg(m2v, sel, a) : flagArg(m2v, sel, a));
    }
    for (const char *name : {"maxBufferLength", "sharedMemorySize", "dedicatedMemorySize",
                             "minBufferNoCopyAlignmentBytes", "maxComputeThreadgroupMemory"})
        check(name, 0, count(raw, sel_registerName(name)), count(m2v, sel_registerName(name)));
    for (const char *name : {"hasUnifiedMemory", "isLowPower", "isRemovable", "isHeadless"})
        check(name, 0, flag(raw, sel_registerName(name)), flag(m2v, sel_registerName(name)));
    for (NSUInteger a : args) {
        check("supportsFeatureSet:", a, flagArg(raw, @selector(supportsFeatureSet:), a),
              flagArg(m2v, @selector(supportsFeatureSet:), a));
        SEL buffer = sel_registerName("minimumTextureBufferAlignmentForPixelFormat:");
        check("minimumTextureBufferAlignmentForPixelFormat:", a, countArg(raw, buffer, a), countArg(m2v, buffer, a));
    }
    const MTLSize r = [(id<MTLDevice>)raw maxThreadsPerThreadgroup], m = [(id<MTLDevice>)m2v maxThreadsPerThreadgroup];
    check("maxThreadsPerThreadgroup", 0, (r.width << 32) | (r.height << 8) | r.depth,
          (m.width << 32) | (m.height << 8) | m.depth);
    for (const char *name : {"name", "productName", "vendorName"}) {
        id a = ((id (*)(id, SEL))objc_msgSend)(raw, sel_registerName(name)),
           b = ((id (*)(id, SEL))objc_msgSend)(m2v, sel_registerName(name));
        check(name, 0, ![a isEqual:b], 0);
    }
    return bad;
}

}  // namespace answers
