// SPDX-License-Identifier: MIT
#pragma once
#include "../../precompile/PrecompileCompute.h"
#include "NativeBridgeMetalFrontend.h"
#include <sys/stat.h>

namespace rawseed {
inline NSMutableDictionary *analysisRow(const std::string &line, const std::string &output)
{
    NSMutableDictionary *row = [@{@"kind": @"analysis", @"status": @"refused", @"reason": @""} mutableCopy];
    precompile::ComputeInput input;
    NSError *error = nil;
    const auto outcome = precompile::computeInput(line, input, &error);
    row[@"kernelLibrary"] = @(input.kernel.library.c_str());
    if (outcome != precompile::Made) {
        row[@"status"] = outcome == precompile::NoLibrary ? @"missing-library" :
                         outcome == precompile::NoFunction ? @"missing-function" : @"refused";
        row[@"reason"] = error.localizedDescription ?: @"compute input unavailable";
        return row;
    }
    NSMutableArray *artifacts = [NSMutableArray new];
    const std::string directory = output + "/native-analysis";
    for (bool reflected : {false, true}) {
        id reflection = nil;
        const auto options = input.options | (reflected ? MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo : 0);
        id artifact = native_bridge_metal_compute_prepare(input.descriptor, options, reflected ? &reflection : nullptr, &error);
        if (!artifact) {
            row[@"reason"] = error.localizedDescription ?: @"native compute analysis unavailable";
            return row;
        }
        char name[69] = {};
        if (!native_bridge_metal_compute_save(artifact, directory.c_str(), name, sizeof name) ||
            chmod(directory.c_str(), 0755) || chmod((directory + "/" + name).c_str(), 0644)) {
            row[@"status"] = @"failed";
            row[@"reason"] = @"native analysis write failed";
            return row;
        }
        [artifacts addObject:@{@"name": @(name), @"path": @(("native-analysis/" + std::string(name)).c_str()),
                               @"reflection": @(reflected)}];
    }
    row[@"status"] = @"made";
    row[@"analysis"] = artifacts;
    return row;
}
} // namespace rawseed
