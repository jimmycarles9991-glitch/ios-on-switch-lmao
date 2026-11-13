// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../NativeBridgeMetalFrontend.h"
#include "../../TegraGM20B/TegraGM20BABI.h"
#include "../../TegraGM20BMetal/raw/runtime/RawDirect.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_cache.h"
#include "../../TegraGM20BMetal/raw/runtime/raw_emit.h"
#include <cstdio>

extern "C" unsigned long mock_iokit_calls(unsigned method);
unsigned computeStateChecks(id<MTLDevice> native, MTLComputePipelineDescriptor *descriptor);

static void missing(const char *cls, const char *selector)
{
    fprintf(stderr, "missing %s %s\n", cls, selector);
}

static NSUInteger journalLines(const char *cache, NSString *prefix)
{
    NSString *text = [NSString stringWithContentsOfFile:[@(cache) stringByAppendingPathComponent:@"pipelines.log"]
                                               encoding:NSUTF8StringEncoding
                                                  error:nil];
    NSUInteger count = 0;
    for (NSString *line in [text componentsSeparatedByString:@"\n"])
        count += [line hasPrefix:prefix];
    return count;
}

static NSDictionary *detail(id value, unsigned kind);

static NSArray *members(id value)
{
    NSMutableArray *out = [NSMutableArray new];
    for (id member in [value members])
        [out addObject:@{
            @"name" : [member name],
            @"offset" : @([member offset]),
            @"type" : @([member dataType]),
            @"argumentIndex" : @([member argumentIndex]),
            @"detail" : detail(member, 3)
        }];
    return out;
}

static NSDictionary *detail(id value, unsigned kind)
{
    id structure = nil, array = nil, pointer = nil;
    if (kind == 1) {
        structure = [value bufferStructType];
        pointer = [value bufferPointerType];
    } else if (kind == 3) {
        structure = [value structType];
        array = [value arrayType];
        pointer = [value pointerType];
    } else {
        structure = [value elementStructType];
        array = [value elementArrayType];
        if (kind == 4)
            pointer = [value elementPointerType];
    }
    NSMutableDictionary *out = [NSMutableDictionary new];
    if (structure)
        out[@"struct"] = members(structure);
    if (array)
        out[@"array"] = @{
            @"type" : @([array elementType]),
            @"length" : @([array arrayLength]),
            @"stride" : @([array stride]),
            @"argumentStride" : @([array argumentIndexStride]),
            @"detail" : detail(array, 4)
        };
    if (pointer)
        out[@"pointer"] = @{
            @"type" : @([pointer elementType]),
            @"access" : @([pointer access]),
            @"alignment" : @([pointer alignment]),
            @"size" : @([pointer dataSize]),
            @"argumentBuffer" : @([pointer elementIsArgumentBuffer]),
            @"detail" : detail(pointer, 5)
        };
    return out;
}

static NSArray *bindings(id reflection)
{
    NSMutableArray *out = [NSMutableArray new];
    for (id<MTLBinding> binding in [reflection bindings]) {
        NSMutableDictionary *row = [@{
            @"name" : binding.name,
            @"type" : @(binding.type),
            @"access" : @(binding.access),
            @"index" : @(binding.index),
            @"used" : @(binding.isUsed),
            @"argument" : @(binding.isArgument)
        } mutableCopy];
        id value = binding;
        if (binding.type == MTLBindingTypeBuffer) {
            row[@"size"] = @([value bufferDataSize]);
            row[@"alignment"] = @([value bufferAlignment]);
            row[@"dataType"] = @([value bufferDataType]);
            row[@"detail"] = detail(value, 1);
        } else if (binding.type == MTLBindingTypeThreadgroupMemory) {
            row[@"size"] = @([value threadgroupMemoryDataSize]);
            row[@"alignment"] = @([value threadgroupMemoryAlignment]);
        } else if (binding.type == MTLBindingTypeTexture) {
            row[@"textureType"] = @([value textureType]);
            row[@"dataType"] = @([value textureDataType]);
            row[@"depth"] = @([value isDepthTexture]);
            row[@"arrayLength"] = @([value arrayLength]);
        }
        [out addObject:row];
    }
    return out;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> native = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        id<MTLDevice> apple = MTLCreateSystemDefaultDevice();
        NSError *error = nil;
        NSURL *url = [NSURL fileURLWithPath:@(argv[3])];
        id<MTLLibrary> nativeLibrary = [native newLibraryWithURL:url error:&error];
        id<MTLLibrary> appleLibrary = [apple newLibraryWithURL:url error:&error];
        if (!nativeLibrary || !appleLibrary) {
            fprintf(stderr, "libraries: %s\n", error.description.UTF8String);
            return 1;
        }
        auto *descriptor = [MTLComputePipelineDescriptor new];
        descriptor.computeFunction = [nativeLibrary newFunctionWithName:@"artifact_probe"];
        descriptor.maxTotalThreadsPerThreadgroup = 128;
        MTLComputePipelineDescriptor *reference = [descriptor copy];
        reference.computeFunction = [appleLibrary newFunctionWithName:@"artifact_probe"];
        const MTLPipelineOption options = MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo;
        MTLComputePipelineReflection *appleReflection = nil;
        id<MTLComputePipelineState> appleState = [apple newComputePipelineStateWithDescriptor:reference
                                                                                      options:options
                                                                                   reflection:&appleReflection
                                                                                        error:&error];
        if (!appleState) {
            fprintf(stderr, "Apple pipeline: %s\n", error.description.UTF8String);
            return 1;
        }
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(rules);
        const uint32_t threads[3] = {32, 1, 1};
        raw::KernelDispatch dispatch(threads, {{0, 128}});
        auto *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                           width:8
                                                                          height:1
                                                                       mipmapped:NO];
        dispatch.texture(0, texture);
        raw::ComputeRecord before, after;
        std::string why;
        if (direct.kernel(descriptor, dispatch.dispatch(), "artifact test", before, why) != raw::CompileOutcome::Made) {
            fprintf(stderr, "baseline compile: %s\n", why.c_str());
            return 1;
        }
        const auto allocations = mock_iokit_calls(TegraGM20BMethodBoAlloc);
        const auto journaled = journalLines(argv[2], @"C ");
        id nativeReflection = nil;
        id<MTLComputePipelineState> state =
            native_bridge_metal_compute_prepare(descriptor, options, &nativeReflection, &error);
        unsigned failures = 0;
        const auto check = [&](bool good, const char *name) {
            printf("%s %s\n", good ? "PASS" : "FAIL", name);
            failures += !good;
        };
        check(state != nil, "native artifact creation");
        if (!state) {
            fprintf(stderr, "%s\n", error.description.UTF8String);
            return 1;
        }
        check(mock_iokit_calls(TegraGM20BMethodBoAlloc) == allocations, "metadata creates no GPU allocations");
        check(journalLines(argv[2], @"C ") == journaled + 1, "native artifact preserves debug seed observation");
        check(state.maxTotalThreadsPerThreadgroup == appleState.maxTotalThreadsPerThreadgroup, "thread limit");
        check(state.threadExecutionWidth == 32, "native warp width");
        check(state.staticThreadgroupMemoryLength == appleState.staticThreadgroupMemoryLength,
              "static threadgroup bytes");
        check([bindings(nativeReflection) isEqual:bindings(appleReflection)], "recursive reflection matches Apple");
        bool protocols = [nativeReflection isKindOfClass:[MTLComputePipelineReflection class]];
        for (id<MTLBinding> binding in [nativeReflection bindings]) {
            protocols &= [binding conformsToProtocol:@protocol(MTLBinding)];
            if (binding.type == MTLBindingTypeBuffer)
                protocols &= [binding conformsToProtocol:@protocol(MTLBufferBinding)];
            if (binding.type == MTLBindingTypeTexture)
                protocols &= [binding conformsToProtocol:@protocol(MTLTextureBinding)];
            if (binding.type == MTLBindingTypeThreadgroupMemory)
                protocols &= [binding conformsToProtocol:@protocol(MTLThreadgroupBinding)];
        }
        check(protocols, "reflection exposes its Metal types and protocols");
        if (![bindings(nativeReflection) isEqual:bindings(appleReflection)]) {
            fprintf(stderr, "native=%s\napple=%s\n", bindings(nativeReflection).description.UTF8String,
                    bindings(appleReflection).description.UTF8String);
        }
        const auto made = direct.kernel(descriptor, dispatch.dispatch(), "artifact test", after, why);
        raw::CacheKey key;
        const auto baseline = raw::encodeKernelEntry(key, &before, "");
        const auto prepared = raw::encodeKernelEntry(key, &after, "");
        check(made == raw::CompileOutcome::Made && baseline == prepared,
              "artifact dispatch exactly matches descriptor compile");
        check([[NSData dataWithBytes:baseline.data() length:baseline.size()]
                  writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"descriptor.kernel"]
                   atomically:YES] &&
                  [[NSData dataWithBytes:prepared.data() length:prepared.size()]
                      writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"artifact.kernel"]
                       atomically:YES],
              "exact native records preserved");
        failures += computeStateChecks(native, descriptor);
        descriptor = nil;
        state = nil;
        check([bindings(nativeReflection) isEqual:bindings(appleReflection)], "reflection outlives artifact");
        return failures ? 1 : 0;
    }
}
