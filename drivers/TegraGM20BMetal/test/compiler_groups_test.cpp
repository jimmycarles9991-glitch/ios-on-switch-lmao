// SPDX-License-Identifier: MIT
#include "../compiler/compiler_engine.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../TegraGM20BMetalOffline.h"
#include "../compute_facts_entry.h"
#include "../compute_key.h"
#include "../compute_record.h"
#include "../metallib.h"
#include "../reflection_blob.h"
#include "empty_pack.h"
#include "TegraGM20BCompilerABI.h"
#include <atomic>
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
    std::map<std::string, gm20b::AirHash> hashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), hashes));
    for (const char *name : {"grouped_pointer", "grouped_add", "grouped_mul"})
        ASSERT_EQ(hashes.count(name), 1u) << name;

    GM20BOfflineDevice *device = [GM20BOfflineDevice new];
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
    for (const auto kind : {gm20b::RequestKind::Facts, gm20b::RequestKind::Reflection, gm20b::RequestKind::Kernel}) {
        gm20b::ComputeRecord plainKernel;
        gm20b::PackComputeFacts plainFacts = {};
        std::string plainReflection;
        for (bool grouped : {false, true}) {
            gm20b::CompileRequest request;
            request.kind = kind;
            request.libraries.push_back({"", false, bytes});
            gm20b::RequestStage entry;
            entry.tag = "k";
            entry.function = entry.name = "grouped_pointer";
            request.stages.push_back(entry);
            std::vector<gm20b::LinkedComputeFunction> linked;
            for (unsigned list = 0; list < (grouped ? 2u : 1u); ++list)
                for (const char *name : {"grouped_add", "grouped_mul"}) {
                    gm20b::RequestStage stage;
                    stage.tag = list ? "kg" : "kl";
                    stage.function = stage.name = name;
                    gm20b::LinkedComputeFunction function;
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
                gm20b::computeStateKey(hashes.at("grouped_pointer"), gm20b::computeLinkedText(linked), 0, false);
            request.kernel.threads[0] = 32;
            request.kernel.threads[1] = request.kernel.threads[2] = 1;
            request.key = kind == gm20b::RequestKind::Facts ? gm20b::factsCacheKey(0, state).key
                          : kind == gm20b::RequestKind::Reflection
                              ? gm20b::computeReflectionKey(0, state).key
                              : "kernel | " + state + " | tg=32,1,1 | tgmem= | tex= | smp=";
            switch_compiler::Prepared prepared;
            ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(request), 0, system, prepared, why)) << why;
            std::string result;
            ASSERT_EQ(engine.compile(prepared, result, why), TegraGM20BCompileMade) << why;
            if (kind == gm20b::RequestKind::Facts) {
                gm20b::PackComputeFacts facts = {};
                ASSERT_EQ(gm20b::decodeFactsEntry(result, prepared.content, facts, why),
                          gm20b::RecordCache::Found::Record)
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
            } else if (kind == gm20b::RequestKind::Reflection) {
                bool made = false;
                std::string reflection;
                ASSERT_TRUE(gm20b::decodeReflectionEntry(result, prepared.content, made, reflection) && made);
                ASSERT_FALSE(reflection.empty());
                if (grouped)
                    EXPECT_EQ(reflection, plainReflection);
                else
                    plainReflection = std::move(reflection);
            } else {
                gm20b::ComputeRecord kernel;
                ASSERT_EQ(gm20b::decodeKernelEntry(result, prepared.content, kernel, why),
                          gm20b::RecordCache::Found::Record)
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

TEST(CompilerGroups, ClientLibraryHandleCompilesIdenticallyToInlineBytes)
{
    const char *path = getenv("SWITCH_GROUPED_METALLIB");
    ASSERT_NE(path, nullptr);
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());
    std::map<std::string, gm20b::AirHash> hashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), hashes));

    gm20b::CompileRequest request;
    request.kind = gm20b::RequestKind::Facts;
    gm20b::RequestLibrary handleLib;
    handleLib.libraryId = 42;
    request.libraries.push_back(handleLib);
    gm20b::RequestStage entry;
    entry.tag = "k";
    entry.function = entry.name = "grouped_pointer";
    request.stages.push_back(entry);
    std::vector<gm20b::LinkedComputeFunction> linked;
    for (const char *name : {"grouped_add", "grouped_mul"}) {
        gm20b::RequestStage stage;
        stage.tag = "kl";
        stage.function = stage.name = name;
        gm20b::LinkedComputeFunction function;
        function.air = hashes.at(name);
        function.symbol = function.name = name;
        request.stages.push_back(std::move(stage));
        linked.push_back(std::move(function));
    }
    const std::string state =
        gm20b::computeStateKey(hashes.at("grouped_pointer"), gm20b::computeLinkedText(linked), 0, false);
    request.key = gm20b::factsCacheKey(0, state).key;

    int mapCalls = 0;
    auto unmapCalls = std::make_shared<std::atomic<int>>(0);
    switch_compiler::ClientLibraries clientLibs(
        [&](uint64_t, uint64_t, const uint8_t *&ptr, size_t &sz, std::string &) {
            mapCalls++;
            ptr = reinterpret_cast<const uint8_t *>(bytes.data());
            sz = bytes.size();
            return true;
        },
        [unmapCalls](uint64_t, uint64_t) {
            unmapCalls->fetch_add(1, std::memory_order_relaxed);
        });
    switch_compiler::SystemLibraries system;
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {emptyPack(), "", {}}, why)) << why;

    std::string handleResult;
    {
        switch_compiler::Prepared prepared;
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(request), 0, system, prepared, why, 7, &clientLibs))
            << why;
        ASSERT_EQ(engine.compile(prepared, handleResult, why), TegraGM20BCompileMade) << why;
    }
    EXPECT_EQ(mapCalls, 1);

    request.libraries[0].libraryId = 0;
    request.libraries[0].bytes = bytes;
    std::string inlineResult;
    {
        switch_compiler::Prepared prepared;
        ASSERT_TRUE(switch_compiler::prepare(gm20b::encodeRequest(request), 0, system, prepared, why)) << why;
        ASSERT_EQ(engine.compile(prepared, inlineResult, why), TegraGM20BCompileMade) << why;
    }
    EXPECT_EQ(handleResult, inlineResult);
}

}  // namespace
