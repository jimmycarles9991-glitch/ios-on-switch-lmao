#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../NativeBridgeMetalFrontend.h"
#include "../bridge/NativeMetalDirect.h"
#include "device_compile.h"
#include "../../../third_party/mesa/src/nouveau/vulkan/nvk_switch_ir.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

enum : uint32_t {
    TypeVoid = 19,
    Load = 61,
    VectorInsertDynamic = 78,
    IsInf = 157,
    Select = 169
};

struct Compared {
    raw::nvk::DeviceNvk compiler;
    const char *name;
    const char *output;
    bool expected;
    bool passed = false;
};

struct Portable {
    const NmbDirectStage *stage;
    unsigned calls = 0;
};

static bool portable(const void *context, const uint32_t **words, size_t *bytes)
{
    auto &p = *const_cast<Portable *>(static_cast<const Portable *>(context));
    ++p.calls;
    return p.stage->portable && p.stage->portable(p.stage->portableContext, words, bytes);
}

static bool sameCode(const raw::NvkStage &a, const raw::NvkStage &b)
{
    return a.codeBytes == b.codeBytes && a.dataBytes == b.dataBytes && a.cbufCount == b.cbufCount &&
           !memcmp(a.cbufTypes, b.cbufTypes, a.cbufCount) && !memcmp(a.code, b.code, a.codeBytes) &&
           (!a.dataBytes || !memcmp(a.data, b.data, a.dataBytes));
}

static bool sameMetadata(const raw::NvkStage &a, const raw::NvkStage &b)
{
    return a.codeBytes == b.codeBytes && a.dataBytes == b.dataBytes && a.cbufCount == b.cbufCount &&
           a.numGprs == b.numGprs && !memcmp(a.header, b.header, sizeof(a.header)) &&
           !memcmp(a.cbufTypes, b.cbufTypes, a.cbufCount) && (!a.dataBytes || !memcmp(a.data, b.data, a.dataBytes));
}

static bool compare(const NmbDirectKernel *kernel, void *context)
{
    auto &c = *static_cast<Compared *>(context);
    raw::SubgroupSize subgroup;
    subgroup.size = kernel->stage.subgroupSize;
    Portable materialize = {&kernel->stage};
    raw::NvkStageInfo info = {kernel->stage.spirv,
                              kernel->stage.spirvBytes,
                              kernel->stage.entry,
                              kernel->stage.stage,
                              kernel->stage.specialization,
                              raw::handle(kernel->layout),
                              0,
                              nullptr,
                              subgroup.size ? &subgroup : nullptr,
                              false,
                              false,
                              0,
                              0,
                              {},
                              0,
                              kernel->stage.semantic,
                              &materialize,
                              portable};
    raw::NvkStage native = {}, portable = {};
    void *device = raw::handle(kernel->device);
    fprintf(stderr, "compile %s native\n", c.name);
    int first = raw::nvk::compileStage(c.compiler, device, info, native);
    bool lazy = kernel->stage.spirvBytes == 0 && materialize.calls == (native.semanticVersion ? 0 : 1);
    info.semantic = nullptr;
    fprintf(stderr, "compile %s portable\n", c.name);
    int second = raw::nvk::compileStage(c.compiler, device, info, portable);
    bool execution = !strcmp(c.name, "semantic_vector") || !strcmp(c.name, "semantic_private") ||
                     !strcmp(c.name, "semantic_extended") || !strcmp(c.name, "semantic_unpack_half") ||
                     !strcmp(c.name, "semantic_convert") || !strcmp(c.name, "semantic_texture");
    bool same = first == 0 && second == 0 && sameMetadata(native, portable) && lazy &&
                (execution || sameCode(native, portable));
    bool route = (native.semanticVersion == 1) == c.expected && portable.semanticVersion == 0;
    auto metrics = reinterpret_cast<bool (*)(const raw::NvkStage *, raw::NvkStageMetrics *, size_t)>(
        native_bridge_metal_nvk_symbol("switch_nvk_stage_metrics"));
    raw::NvkStageMetrics cost = {};
    bool costValid = metrics && metrics(&native, &cost, sizeof(cost)) && cost.version == 1 &&
                     cost.codeBytes == native.codeBytes && cost.gprs == native.numGprs && cost.instructions > 0 &&
                     cost.maxWarps > 0 && !metrics(&native, &cost, sizeof(cost) - 1) &&
                     !metrics(nullptr, &cost, sizeof(cost));
    raw::nvk::DeviceNvk rankedCompiler = c.compiler;
    rankedCompiler.compileStage = native_bridge_metal_nvk_symbol("switch_nvk_compile_ranked_stage");
    raw::NvkStage ranked = {};
    raw::NvkStageInfo rankedInfo = info;
    rankedInfo.semantic = kernel->stage.semantic;
    int rankedResult =
        rankedCompiler.compileStage ? raw::nvk::compileStage(rankedCompiler, device, rankedInfo, ranked) : -1;
    const raw::NvkStage &selected = ranked.semanticVersion ? native : portable;
    bool rankValid = rankedResult == 0 && sameCode(ranked, selected) && sameMetadata(ranked, selected) &&
                     (!sameCode(native, portable) || ranked.semanticVersion == native.semanticVersion);
    printf("%s %s metrics=%d ranked=%u instructions=%u cycles=%llu spills=%u/%u\n",
           costValid && rankValid ? "PASS" : "FAIL", c.name, costValid, ranked.semanticVersion, cost.instructions,
           static_cast<unsigned long long>(cost.staticCycles), cost.memorySpills, cost.memoryFills);
    same &= costValid && rankValid;
    raw::nvk::stageFinish(c.compiler, device, ranked);
    printf("%s %s native=%u nativeBytes=%u portableBytes=%u byteExact=%d lazy=%d status=%d/%d\n",
           same && route ? (execution ? "PENDING_EXECUTION" : "PASS") : "FAIL", c.name, native.semanticVersion,
           native.codeBytes, portable.codeBytes, sameCode(native, portable), lazy, first, second);
    for (auto [stage, suffix] : {std::pair{&native, "native"}, std::pair{&portable, "portable"}}) {
        std::string path = std::string(c.output) + "/" + c.name + "." + suffix + ".sass";
        if (stage->code && stage->codeBytes)
            [[NSData dataWithBytes:stage->code length:stage->codeBytes] writeToFile:@(path.c_str()) atomically:YES];
        path = std::string(c.output) + "/" + c.name + "." + suffix + ".data";
        [[NSData dataWithBytes:stage->data length:stage->dataBytes] writeToFile:@(path.c_str()) atomically:YES];
    }
    const uint32_t *words = kernel->stage.spirv;
    size_t wordBytes = kernel->stage.spirvBytes;
    if ((words && wordBytes) ||
        (kernel->stage.portable && kernel->stage.portable(kernel->stage.portableContext, &words, &wordBytes))) {
        std::string path = std::string(c.output) + "/" + c.name + ".spv";
        [[NSData dataWithBytes:words length:wordBytes] writeToFile:@(path.c_str()) atomically:YES];
    }
    if (kernel->stage.semantic) {
        const auto &program = *static_cast<const switch_nvk_ir *>(kernel->stage.semantic);
        std::string path = std::string(c.output) + "/" + c.name + ".typed";
        uint32_t header[] = {program.version, program.types,        program.values,       program.blocks,
                             program.entry,   program.record_count, program.operand_count};
        NSMutableData *bytes = [NSMutableData dataWithBytes:header length:sizeof(header)];
        [bytes appendBytes:program.records length:program.record_count * sizeof(*program.records)];
        [bytes appendBytes:program.operands length:program.operand_count * sizeof(*program.operands)];
        [bytes writeToFile:@(path.c_str()) atomically:YES];
        auto refused = [&](const switch_nvk_ir &rejected, const char *mutation) {
            info.semantic = &rejected;
            raw::NvkStage fallback = {};
            unsigned before = materialize.calls;
            int result = raw::nvk::compileStage(c.compiler, device, info, fallback);
            bool safe = result == 0 && fallback.semanticVersion == 0 && sameCode(fallback, portable) &&
                        sameMetadata(fallback, portable) && materialize.calls == before + 1;
            printf("%s %s rejected-contract=%s\n", safe ? "PASS" : "FAIL", c.name, mutation);
            same &= safe;
            raw::nvk::stageFinish(c.compiler, device, fallback);
        };
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
            auto rejected = program;
            if (mutation == 0)
                rejected.version = 99;
            if (mutation == 1)
                rejected.entry = rejected.blocks;
            if (mutation == 2)
                rejected.operand_count = 0;
            refused(rejected, std::to_string(mutation).c_str());
        }
        if (!strcmp(c.name, "semantic_table")) {
            for (unsigned mutation = 0; mutation < 2; ++mutation) {
                std::vector<switch_nvk_ir_record> records(program.records, program.records + program.record_count);
                std::vector<uint32_t> operands(program.operands, program.operands + program.operand_count);
                bool changed = false;
                for (auto &record : records) {
                    if (record.kind != 1 || record.opcode != 44 || record.count != 256)
                        continue;
                    if (mutation == 0)
                        --record.count;
                    else
                        operands[record.first + 255] = program.values;
                    changed = true;
                    break;
                }
                same &= changed;
                auto rejected = program;
                rejected.records = records.data();
                rejected.operands = operands.data();
                refused(rejected, mutation ? "table-element" : "table-length");
            }
        }
        for (auto [opcode, count] : {std::pair{Select, 2u}, std::pair{IsInf, 0u}, std::pair{VectorInsertDynamic, 0u}}) {
            std::vector<switch_nvk_ir_record> records(program.records, program.records + program.record_count);
            for (auto &record : records) {
                if (record.kind != 6 || record.result >= program.values)
                    continue;
                record.opcode = opcode;
                record.count = count;
                record.first = 0;
                auto rejected = program;
                rejected.records = records.data();
                refused(rejected, ("alu-arity-" + std::to_string(opcode)).c_str());
                break;
            }
        }
        uint32_t voidType = UINT32_MAX;
        for (const auto &record :
             std::vector<switch_nvk_ir_record>(program.records, program.records + program.record_count)) {
            if (record.kind == 0 && record.opcode == TypeVoid)
                voidType = record.result;
        }
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            std::vector<switch_nvk_ir_record> records(program.records, program.records + program.record_count);
            for (auto &record : records) {
                if (record.kind != 6 || record.opcode != Load)
                    continue;
                if (mutation == 0)
                    record.auxiliary = 3;
                else if (mutation == 1)
                    record.type = voidType;
                else if (mutation == 2)
                    record.flags |= 32;
                else
                    record.flags |= 16;
                auto rejected = program;
                rejected.records = records.data();
                const char *reason[] = {"load-alignment", "load-type", "load-unknown-access", "load-availability"};
                refused(rejected, reason[mutation]);
                break;
            }
        }
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
            std::vector<switch_nvk_ir_record> records(program.records, program.records + program.record_count);
            std::vector<uint32_t> operands(program.operands, program.operands + program.operand_count);
            for (auto &record : records) {
                if (record.kind != 6 || record.opcode != 224)
                    continue;
                if (mutation == 0)
                    record.count = 2;
                else if (mutation == 1)
                    operands[record.first + 1] = UINT32_MAX;
                else
                    operands[record.first + 2] |= 1u << 31;
                auto rejected = program;
                rejected.records = records.data();
                rejected.operands = operands.data();
                const char *reason[] = {"barrier-arity", "barrier-scope", "barrier-semantics"};
                refused(rejected, reason[mutation]);
                break;
            }
        }
    }
    raw::nvk::stageFinish(c.compiler, device, native);
    raw::nvk::stageFinish(c.compiler, device, portable);
    c.passed = same && route;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, nullptr);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!library) {
            fprintf(stderr, "library: %s\n", error.description.UTF8String);
            return 1;
        }
        unsigned failures = 0;
        for (const char *name :
             {"semantic_integer", "semantic_branch", "semantic_loop", "semantic_float", "semantic_float_round",
              "semantic_atan2", "semantic_vector", "semantic_half", "semantic_float_compare", "semantic_convert",
              "semantic_private", "semantic_texture", "semantic_atomic", "semantic_barrier", "semantic_extended",
              "semantic_group_min", "semantic_unpack_half", "semantic_subgroup", "semantic_table"}) {
            if (const char *only = getenv("SWITCH_IR_CASE"); only && strcmp(only, name))
                continue;
            auto *descriptor = [MTLComputePipelineDescriptor new];
            descriptor.computeFunction = [library newFunctionWithName:@(name)];
            if (!strcmp(name, "semantic_subgroup"))
                descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = YES;
            NmbDirectBoundTexture texture = {
                0, MTLPixelFormatRGBA32Float, MTLTextureType2D, MTLTextureUsageShaderRead, 1, 1, 1};
            NmbDirectDispatch dispatch = {};
            dispatch.threads[0] = 32;
            dispatch.threads[1] = dispatch.threads[2] = 1;
            if (!strcmp(name, "semantic_subgroup"))
                dispatch.memory[0] = 128;
            bool expected = strcmp(name, "semantic_atomic");
            if (!strcmp(name, "semantic_texture")) {
                dispatch.textures = &texture;
                dispatch.textureCount = 1;
            }
            Compared compared = {raw::nvk::DeviceNvk::find(native_bridge_metal_nvk_symbol), name, argv[4], expected};
            const char *why = nullptr;
            bool made = native_bridge_metal_compute_direct(descriptor, &dispatch, compare, &compared, &why);
            if (!made)
                fprintf(stderr, "%s: %s\n", name, why ? why : "no compile");
            failures += !made || !compared.passed;
        }
        return failures ? 1 : 0;
    }
}
