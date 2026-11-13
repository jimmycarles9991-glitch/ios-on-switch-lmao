// SPDX-License-Identifier: MIT
#pragma once
#include "PrecompileInputs.h"

namespace precompile {
struct ComputeInput {
    MTLComputePipelineDescriptor *descriptor = nil;
    MTLPipelineOption options = MTLPipelineOptionNone;
    bool descriptorForm = false;
    Stage kernel;
    std::string file, label;
};

inline Outcome computeInput(const std::string &line, ComputeInput &input, NSError **why)
{
    const auto parts = split(line, " | ");
    std::map<std::string, std::string> fields;
    for (const auto &field : split(parts.empty() ? std::string() : parts[0])) {
        const auto equal = field.find('=');
        if (equal != std::string::npos)
            fields[field.substr(0, equal)] = field.substr(equal + 1);
    }
    input.label = fields.count("label") ? unescape(fields["label"]) : "-";
    std::vector<Stage> linked, hidden;
    for (size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].rfind("k ", 0) == 0)
            input.kernel = stage(parts[i].substr(2));
        if (parts[i].rfind("l ", 0) == 0)
            linked.push_back(stage(parts[i].substr(2)));
        if (parts[i].rfind("p ", 0) == 0)
            hidden.push_back(stage(parts[i].substr(2)));
    }
    input.file = input.kernel.file;
    if (input.kernel.name.empty())
        return Refused;
    Outcome outcome = Made;
    id<MTLFunction> kernel = function(input.kernel, outcome, why);
    if (outcome != Made)
        return outcome;
    auto *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = kernel;
    input.options = MTLPipelineOption(strtoull(fields["opt"].c_str(), nullptr, 10));
    input.descriptorForm = fields["form"] == "d";
    if (input.descriptorForm) {
        if (input.label != "-")
            descriptor.label = @(input.label.c_str());
        descriptor.maxTotalThreadsPerThreadgroup = strtoul(fields["max"].c_str(), nullptr, 10);
        descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = fields["tgm"] == "1";
        descriptor.supportIndirectCommandBuffers = fields["icb"] == "1";
        descriptor.maxCallStackDepth = strtoul(fields["depth"].c_str(), nullptr, 10);
        descriptor.supportAddingBinaryFunctions = fields["abf"] == "1";
        const auto &mutability = fields["mut"];
        for (size_t i = 0; i < 31 && i < mutability.size(); ++i)
            descriptor.buffers[i].mutability = MTLMutability(mutability[i] - '0');
        NSMutableArray *visible = [NSMutableArray new], *privateFunctions = [NSMutableArray new];
        for (auto &[stages, into] :
             std::vector<std::pair<std::vector<Stage> *, NSMutableArray *>>{{&linked, visible}, {&hidden, privateFunctions}})
            for (const auto &source : *stages) {
                id<MTLFunction> functionObject = function(source, outcome, why);
                if (outcome != Made)
                    return outcome;
                [into addObject:functionObject];
            }
        if (visible.count || privateFunctions.count) {
            auto *functions = [MTLLinkedFunctions linkedFunctions];
            functions.functions = visible;
            functions.privateFunctions = privateFunctions;
            descriptor.linkedFunctions = functions;
        }
    }
    input.descriptor = descriptor;
    return Made;
}
} // namespace precompile
