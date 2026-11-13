// SPDX-License-Identifier: MIT
#pragma once
#include "NativeBridgeMetalFrontend.h"
#include "kernel_shapes.h"
#include "native_seed_key.h"
#include "native_seed_manifest.h"
#include "../runtime/RawDirect.h"

namespace rawseed {

inline NSMutableDictionary *computeRow(const std::string &line, uint64_t stack, raw::Direct &direct,
                                       raw::RecordCache &cache, const std::string &output,
                                       const probe::Shape *region = nullptr)
{
    NSMutableDictionary *row = [@{@"kind" : @"compute", @"status" : @"refused", @"reason" : @""} mutableCopy];
    std::string why;
    probe::Shape shape;
    if (!probe::parse(line, shape, why)) {
        row[@"reason"] = @(why.c_str());
        return row;
    }
    if (region)
        shape = *region;
    const size_t start = line.find(" | k ") + 5, end = line.find(" | ", start);
    const auto stage = precompile::stage(line.substr(start, end - start));
    row[@"kernelLibrary"] = @(stage.library.c_str());
    precompile::Outcome inputStatus;
    NSError *error = nil;
    id<MTLFunction> function = precompile::function(stage, inputStatus, &error);
    if (!function) {
        row[@"status"] = inputStatus == precompile::NoLibrary    ? @"missing-library"
                         : inputStatus == precompile::NoFunction ? @"missing-function"
                                                                 : @"refused";
        row[@"reason"] = error.localizedDescription ?: @"compute function unavailable";
        return row;
    }
    const void *bytes = nullptr;
    size_t length = 0;
    std::map<std::string, raw::AirHash> hashes;
    if (!native_bridge_metal_function_metallib(function, &bytes, &length) ||
        !raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes) || !hashes.count(stage.name)) {
        row[@"reason"] = @"native AIR identity unavailable";
        return row;
    }
    std::map<uint32_t, uint32_t> memory;
    std::istringstream allocations(shape.threadgroupMemory);
    std::string allocation;
    while (std::getline(allocations, allocation, ',')) {
        unsigned index, size;
        int used = 0;
        if (sscanf(allocation.c_str(), "%u:%u%n", &index, &size, &used) != 2 || size_t(used) != allocation.size() ||
            index >= 31 || !memory.emplace(index, size).second) {
            row[@"reason"] = @"invalid threadgroup memory binding";
            return row;
        }
    }
    raw::KernelDispatch dispatch(shape.threads, memory, shape.regionMode);
    for (const auto &texture : shape.textures) {
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];
        descriptor.pixelFormat = MTLPixelFormat(texture.pixelFormat);
        descriptor.textureType = MTLTextureType(texture.textureType);
        descriptor.mipmapLevelCount = texture.levels;
        descriptor.arrayLength = texture.arrayLength;
        descriptor.sampleCount = 1;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        dispatch.texture(texture.slot, descriptor);
    }
    for (const auto &sampler : shape.samplers)
        dispatch.sampler(sampler.slot, probe::samplerDescriptor(sampler));
    const std::string dispatchKey = probe::key(shape, hashes.at(stage.name));
    raw::CacheKey key;
    key.stack = stack;
    key.key = "kernel | " + dispatchKey;
    identify(row, key);
    raw::ComputeRecord record;
    const auto found = cache.loadKernel(key, record, why);
    const auto outcome =
        found == raw::RecordCache::Found::Record
            ? raw::CompileOutcome::Made
            : direct.kernel(probe::descriptor(shape, function), dispatch.dispatch(), dispatchKey, record, why);
    row[@"status"] = outcome == raw::CompileOutcome::Made      ? @"made"
                     : outcome == raw::CompileOutcome::Refused ? @"refused"
                                                               : @"failed";
    row[@"reason"] = @(why.c_str());
    if (outcome != raw::CompileOutcome::Made)
        return row;
    const std::string relative = "raw-" + hex(stack) + "/" + hex(key.name()) + ".rec";
    raw::ComputeRecord loaded;
    if (!cache.storeKernel(key, &record, "") || cache.loadKernel(key, loaded, why) != raw::RecordCache::Found::Record ||
        raw::encodeKernelEntry(key, &record, "") != raw::encodeKernelEntry(key, &loaded, "") ||
        chmod((output + "/" + relative).c_str(), 0644)) {
        row[@"status"] = @"failed";
        row[@"reason"] = @"native compute record write/readback failed";
        return row;
    }
    row[@"artifact"] = @(relative.c_str());
    return row;
}

inline NSArray<NSMutableDictionary *> *computeRows(const std::string &line, uint64_t stack, raw::Direct &direct,
                                                   raw::RecordCache &cache, const std::string &output)
{
    probe::Shape shape;
    std::string why;
    if (!probe::parse(line, shape, why))
        return @[ computeRow(line, stack, direct, cache, output) ];
    NSMutableArray *rows = [NSMutableArray new];
    probe::eachRegionShape(shape, [&](const probe::Shape &region, uint8_t mask) {
        auto row = computeRow(line, stack, direct, cache, output, &region);
        row[@"region"] = @(mask);
        row[@"local"] = @[ @(region.threads[0]), @(region.threads[1]), @(region.threads[2]) ];
        row[@"regionMode"] = @(region.regionMode);
        [rows addObject:row];
    });
    return rows;
}

}  // namespace rawseed
