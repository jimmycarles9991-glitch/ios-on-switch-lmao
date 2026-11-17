// SPDX-License-Identifier: MIT
#include "../compiler/compiler_engine.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/RawOffline.h"
#include "../raw/runtime/raw_compute_facts_entry.h"
#include "../raw/runtime/raw_compute_key.h"
#include "../raw/runtime/raw_compute_record.h"
#include "../raw/runtime/raw_metallib.h"
#include "../raw/runtime/raw_reflection_blob.h"
#include "empty_pack.h"
#include "TegraGM20BCompilerABI.h"
#include <cstring>
#include <fstream>
#include <iterator>
#include <gtest/gtest.h>

namespace {

TEST(CompilerGroups, TheNativeAdaptersPreserveGroupedFunctionsAndTheUngroupedProgram)
{
    const char *path = getenv("SWITCH_GROUPED_METALLIB");
    ASSERT_NE(path, nullptr);
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());
    std::map<std::string, raw::AirHash> hashes;
    ASSERT_TRUE(raw::metallibAirHashes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), hashes));
    for (const char *name : {"grouped_pointer", "grouped_add", "grouped_mul"})
        ASSERT_EQ(hashes.count(name), 1u) << name;

    RawOfflineDevice *device = [RawOfflineDevice new];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
    ASSERT_TRUE(library != nil) << error.description.UTF8String;
    auto *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = [library newFunctionWithName:@"grouped_pointer"];
    id<MTLFunction> add = [library newFunctionWithName:@"grouped_add"],
                    mul = [library newFunctionWithName:@"grouped_mul"];
    ASSERT_TRUE(descriptor.computeFunction && add && mul);
    descriptor.linkedFunctions = [MTLLinkedFunctions linkedFunctions];
    descriptor.linkedFunctions.functions = @[ add, mul ];
    const std::string groupName("choice\0other", 12);
    NSString *name =
        [[NSString alloc] initWithBytes:groupName.data() length:groupName.size() encoding:NSUTF8StringEncoding];
    ASSERT_TRUE(name != nil);
    descriptor.linkedFunctions.groups = @{name : @[ add, mul ]};
    NmbOfflineComputePipelineDescriptor native = {};
    std::vector<NmbOfflineFunction *> handles;
    std::vector<switch_linked_group_name> groupNames;
    ASSERT_TRUE(offline::descriptorOf(descriptor, native, handles, groupNames));
    ASSERT_EQ(native.linked_count, 2u);
    EXPECT_EQ(native.private_count, 0u);
    EXPECT_EQ(native.binary_count, 0u);
    ASSERT_EQ(native.group_count, 2u);
    EXPECT_EQ(std::string(native.group_names[0].bytes, native.group_names[0].length), groupName);
    EXPECT_EQ(std::string(native.group_names[1].bytes, native.group_names[1].length), groupName);
    EXPECT_EQ(native.group_linked[0], native.linked[0]);
    EXPECT_EQ(native.group_linked[1], native.linked[1]);

    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {emptyPack(), "", {}}, why)) << why;
    switch_compiler::SystemLibraries system;
    for (const auto kind : {raw::RequestKind::Facts, raw::RequestKind::Reflection, raw::RequestKind::Kernel}) {
        raw::ComputeRecord plainKernel;
        raw::PackComputeFacts plainFacts = {};
        std::string plainReflection;
        for (bool grouped : {false, true}) {
            raw::CompileRequest request;
            request.kind = kind;
            request.libraries.push_back({"", false, bytes});
            raw::RequestStage entry;
            entry.tag = "k";
            entry.function = entry.name = "grouped_pointer";
            request.stages.push_back(entry);
            std::vector<raw::LinkedComputeFunction> linked;
            for (unsigned list = 0; list < (grouped ? 2u : 1u); ++list)
                for (const char *name : {"grouped_add", "grouped_mul"}) {
                    raw::RequestStage stage;
                    stage.tag = list ? "kg" : "kl";
                    stage.function = stage.name = name;
                    raw::LinkedComputeFunction function;
                    function.air = hashes.at(name);
                    function.symbol = function.name = name;
                    if (list) {
                        stage.group = groupName;
                        function.group = stage.group;
                    }
                    request.stages.push_back(std::move(stage));
                    linked.push_back(std::move(function));
                }
            const std::string state =
                raw::computeStateKey(hashes.at("grouped_pointer"), raw::computeLinkedText(linked), 0, false);
            request.kernel.threads[0] = 32;
            request.kernel.threads[1] = request.kernel.threads[2] = 1;
            request.key = kind == raw::RequestKind::Facts ? raw::factsCacheKey(0, state).key
                          : kind == raw::RequestKind::Reflection
                              ? raw::computeReflectionKey(0, state).key
                              : "kernel | " + state + " | tg=32,1,1 | tgmem= | tex= | smp=";
            switch_compiler::Prepared prepared;
            ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(request), 0, system, prepared, why)) << why;
            std::string result;
            ASSERT_EQ(engine.compile(prepared, result, why), TegraGM20BCompileMade) << why;
            if (kind == raw::RequestKind::Facts) {
                raw::PackComputeFacts facts = {};
                ASSERT_EQ(raw::decodeFactsEntry(result, prepared.key, facts, why), raw::RecordCache::Found::Record)
                    << why;
                EXPECT_GT(facts.maxThreads, 0u);
                EXPECT_GT(facts.executionWidth, 0u);
                if (grouped) {
                    EXPECT_EQ(facts.maxThreads, plainFacts.maxThreads);
                    EXPECT_EQ(facts.executionWidth, plainFacts.executionWidth);
                    EXPECT_EQ(facts.staticMemory, plainFacts.staticMemory);
                } else {
                    plainFacts = facts;
                }
            } else if (kind == raw::RequestKind::Reflection) {
                bool made = false;
                std::string reflection;
                ASSERT_TRUE(raw::decodeReflectionEntry(result, prepared.key, made, reflection) && made);
                ASSERT_FALSE(reflection.empty());
                if (grouped)
                    EXPECT_EQ(reflection, plainReflection);
                else
                    plainReflection = std::move(reflection);
            } else {
                raw::ComputeRecord kernel;
                ASSERT_EQ(raw::decodeKernelEntry(result, prepared.key, kernel, why), raw::RecordCache::Found::Record)
                    << why;
                ASSERT_FALSE(kernel.code.empty());
                EXPECT_TRUE(kernel.samplers.empty());
                if (grouped) {
                    EXPECT_EQ(kernel.code, plainKernel.code);
                    EXPECT_EQ(kernel.data, plainKernel.data);
                    EXPECT_EQ(kernel.cbufs, plainKernel.cbufs);
                    EXPECT_EQ(memcmp(kernel.cbufType, plainKernel.cbufType, sizeof kernel.cbufType), 0);
                    kernel.kernel.keyHash = plainKernel.kernel.keyHash;
                    EXPECT_EQ(memcmp(&kernel.kernel, &plainKernel.kernel, sizeof kernel.kernel), 0);
                } else {
                    plainKernel = std::move(kernel);
                }
            }
        }
    }
}

}  // namespace
