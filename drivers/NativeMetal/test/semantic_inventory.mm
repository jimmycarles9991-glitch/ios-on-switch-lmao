#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../NativeBridgeMetalFrontend.h"
#include "../bridge/NativeMetalDirect.h"
#include "../../TegraGM20BMetal/precompile/PrecompileInputs.h"
#include "../../TegraGM20BMetal/raw/pack/kernel_shapes.h"
#include "device_compile.h"
#include "../../../third_party/mesa/src/nouveau/vulkan/nvk_switch_ir.h"
#include <fstream>

static bool compileCandidates;
static unsigned artifactIndex;

static NSDictionary *metrics(const raw::NvkStage &stage)
{
    auto read = reinterpret_cast<bool (*)(const raw::NvkStage *, raw::NvkStageMetrics *, size_t)>(
        native_bridge_metal_nvk_symbol("switch_nvk_stage_metrics"));
    raw::NvkStageMetrics m = {};
    if (!read || !read(&stage, &m, sizeof(m)) || m.version != 1)
        return @{@"error" : @"stage metrics unavailable"};
    return @{
        @"codeBytes" : @(m.codeBytes),
        @"instructions" : @(m.instructions),
        @"gprs" : @(m.gprs),
        @"maxWarps" : @(m.maxWarps),
        @"scratchBytes" : @(m.scratchBytes),
        @"memorySpills" : @(m.memorySpills),
        @"memoryFills" : @(m.memoryFills),
        @"registerSpills" : @(m.registerSpills),
        @"registerFills" : @(m.registerFills),
        @"staticCycles" : @(m.staticCycles)
    };
}

static NSDictionary *stage(const NmbDirectStage &stage, uint64_t device, uint64_t layout)
{
    const uint32_t *words = stage.spirv;
    size_t bytes = stage.spirvBytes;
    if ((!words || !bytes) && (!stage.portable || !stage.portable(stage.portableContext, &words, &bytes)))
        return @{@"error" : @"portable materialization failed"};
    if (bytes < 20 || bytes % 4 || words[0] != 0x07230203)
        return @{@"error" : @"invalid portable module"};
    NSMutableDictionary *operations = [NSMutableDictionary dictionary];
    NSMutableDictionary *extended = [NSMutableDictionary dictionary];
    uint32_t functions = 0;
    for (size_t at = 5; at < bytes / 4;) {
        uint32_t count = words[at] >> 16, operation = words[at] & 0xffff;
        if (!count || count > bytes / 4 - at)
            return @{@"error" : @"invalid operation length"};
        NSString *key = [@(operation) stringValue];
        operations[key] = @([operations[key] unsignedIntegerValue] + 1);
        if (operation == 12 && count >= 5) {
            NSString *instruction = [@(words[at + 4]) stringValue];
            extended[instruction] = @([extended[instruction] unsignedIntegerValue] + 1);
        }
        functions += operation == 54;
        at += count;
    }
    NSMutableDictionary *out = [@{
        @"stage" : @(stage.stage),
        @"typedCandidate" : @(stage.semantic != nullptr),
        @"functions" : @(functions),
        @"portableBytes" : @(bytes),
        @"operations" : operations,
        @"extendedOperations" : extended
    } mutableCopy];
    NSString *artifact = nil;
    if (const char *directory = getenv("SWITCH_IR_DUMP")) {
        [[NSFileManager defaultManager] createDirectoryAtPath:@(directory)
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        artifact =
            [@(directory) stringByAppendingPathComponent:[NSString stringWithFormat:@"stage-%04u", artifactIndex++]];
        [[NSData dataWithBytes:words length:bytes] writeToFile:[artifact stringByAppendingString:@".spv"]
                                                    atomically:YES];
        out[@"artifact"] = artifact;
        if (stage.semantic) {
            const auto &p = *static_cast<const switch_nvk_ir *>(stage.semantic);
            uint32_t header[] = {p.version, p.types, p.values, p.blocks, p.entry, p.record_count, p.operand_count};
            NSMutableData *data = [NSMutableData dataWithBytes:header length:sizeof header];
            [data appendBytes:p.records length:p.record_count * sizeof(*p.records)];
            [data appendBytes:p.operands length:p.operand_count * sizeof(*p.operands)];
            [data writeToFile:[artifact stringByAppendingString:@".typed"] atomically:YES];
        }
    }
    if (compileCandidates && stage.semantic) {
        raw::nvk::DeviceNvk compiler = raw::nvk::DeviceNvk::find(native_bridge_metal_nvk_symbol);
        raw::SubgroupSize subgroup;
        subgroup.size = stage.subgroupSize;
        raw::NvkStageInfo info = {words,
                                  bytes,
                                  stage.entry,
                                  stage.stage,
                                  stage.specialization,
                                  raw::handle(layout),
                                  0,
                                  nullptr,
                                  subgroup.size ? &subgroup : nullptr,
                                  false,
                                  false,
                                  0,
                                  0,
                                  {},
                                  0,
                                  stage.semantic,
                                  nullptr,
                                  nullptr};
        raw::NvkStage native = {}, portable = {};
        int n = raw::nvk::compileStage(compiler, raw::handle(device), info, native);
        info.semantic = nullptr;
        int p = raw::nvk::compileStage(compiler, raw::handle(device), info, portable);
        out[@"nativeStatus"] = @(n);
        out[@"portableStatus"] = @(p);
        out[@"nativeUsed"] = @(native.semanticVersion == 1);
        out[@"nativeCodeBytes"] = @(native.codeBytes);
        out[@"portableCodeBytes"] = @(portable.codeBytes);
        out[@"nativeGprs"] = @(native.numGprs);
        out[@"portableGprs"] = @(portable.numGprs);
        out[@"codeExact"] = @(n == 0 && p == 0 && native.codeBytes == portable.codeBytes &&
                              !memcmp(native.code, portable.code, native.codeBytes));
        out[@"nativeMetrics"] = metrics(native);
        out[@"portableMetrics"] = metrics(portable);
        raw::nvk::DeviceNvk rankedCompiler = compiler;
        rankedCompiler.compileStage = native_bridge_metal_nvk_symbol("switch_nvk_compile_ranked_stage");
        if (rankedCompiler.compileStage) {
            info.semantic = stage.semantic;
            raw::NvkStage ranked = {};
            int result = raw::nvk::compileStage(rankedCompiler, raw::handle(device), info, ranked);
            out[@"rankedStatus"] = @(result);
            out[@"rankedNative"] = @(ranked.semanticVersion == 1);
            out[@"rankedMetrics"] = metrics(ranked);
            raw::nvk::stageFinish(compiler, raw::handle(device), ranked);
        }
        if (artifact) {
            [[NSData dataWithBytes:native.code
                            length:native.codeBytes] writeToFile:[artifact stringByAppendingString:@".native.sass"]
                                                      atomically:YES];
            [[NSData dataWithBytes:portable.code
                            length:portable.codeBytes] writeToFile:[artifact stringByAppendingString:@".portable.sass"]
                                                        atomically:YES];
        }
        raw::nvk::stageFinish(compiler, raw::handle(device), native);
        raw::nvk::stageFinish(compiler, raw::handle(device), portable);
    }
    return out;
}

static bool compute(const NmbDirectKernel *kernel, void *context)
{
    NSMutableDictionary *out = [stage(kernel->stage, kernel->device, kernel->layout) mutableCopy];
    NSMutableArray *writes = [NSMutableArray new];
    for (unsigned i = 0; i < kernel->writeCount; ++i) {
        const auto &kw = kernel->writes[i];
        const auto &w = kw.write;
        [writes addObject:@{
            @"set" : @(w.set),
            @"binding" : @(w.binding),
            @"element" : @(w.element),
            @"type" : @(w.type),
            @"source" : @(w.source),
            @"slot" : @(w.slot),
            @"viewFormat" : @(kw.viewFormat),
            @"viewType" : @(w.viewType),
            @"sampler" : @(w.sampler)
        }];
    }
    out[@"descriptorWrites"] = writes;
    [(__bridge NSMutableArray *)context addObject:out];
    return true;
}

static bool render(const NmbDirectState *state, void *context)
{
    NSMutableArray *out = (__bridge NSMutableArray *)context;
    for (unsigned i = 0; i < state->stageCount; ++i)
        [out addObject:stage(state->stages[i], state->device, state->layout)];
    return true;
}

static bool dispatch(const std::string &line, NSMutableArray *stages, std::string &why)
{
    probe::Shape shape;
    if (!probe::parse(line, shape, why))
        return false;
    const size_t start = line.find(" | k ") + 5, end = line.find(" | ", start);
    auto source = precompile::stage(line.substr(start, end - start));
    precompile::Outcome status;
    NSError *error = nil;
    id<MTLFunction> function = precompile::function(source, status, &error);
    if (!function) {
        why = error.localizedDescription.UTF8String ?: "missing library/function";
        return false;
    }
    NmbDirectDispatch dispatch = {};
    memcpy(dispatch.threads, shape.threads, sizeof dispatch.threads);
    std::istringstream allocations(shape.threadgroupMemory);
    std::string allocation;
    while (std::getline(allocations, allocation, ',')) {
        unsigned index, size;
        if (sscanf(allocation.c_str(), "%u:%u", &index, &size) != 2 || index >= 31) {
            why = "invalid threadgroup memory";
            return false;
        }
        dispatch.memory[index] = size;
    }
    std::vector<NmbDirectBoundTexture> textures;
    for (const auto &t : shape.textures)
        textures.push_back({t.slot, t.pixelFormat, t.textureType,
                            MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, t.levels, t.arrayLength, 1});
    std::vector<NmbDirectBoundSampler> samplers;
    for (const auto &s : shape.samplers)
        samplers.push_back({s.slot,
                            {s.minFilter, s.magFilter, s.mipFilter, s.sAddress, s.tAddress, s.rAddress, s.border,
                             s.normalized, s.maxAnisotropy, s.compare, s.lodAverage, s.lodMin, s.lodMax}});
    dispatch.textures = textures.data();
    dispatch.textureCount = textures.size();
    dispatch.samplers = samplers.data();
    dispatch.samplerCount = samplers.size();
    bool ok = true;
    probe::eachRegionShape(shape, [&](const probe::Shape &region, unsigned mask) {
        if (!ok)
            return;
        memcpy(dispatch.threads, region.threads, sizeof dispatch.threads);
        dispatch.regionMode = region.regionMode;
        const char *refused = nullptr;
        NSUInteger start = stages.count;
        ok = native_bridge_metal_compute_direct(probe::descriptor(region, function), &dispatch, compute,
                                                (__bridge void *)stages, &refused);
        if (!ok)
            why = refused ?: "compute translation failed";
        for (NSUInteger i = start; i < stages.count; ++i) {
            NSMutableDictionary *stage = [stages[i] mutableCopy];
            stage[@"regionMask"] = @(mask);
            stage[@"regionMode"] = @(region.regionMode);
            stage[@"localSize"] = @[ @(region.threads[0]), @(region.threads[1]), @(region.threads[2]) ];
            stage[@"nominalLocalSize"] = @[ @(shape.threads[0]), @(shape.threads[1]), @(shape.threads[2]) ];
            stages[i] = stage;
        }
    });
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 8)
        return 2;
    compileCandidates = strchr(argv[7], '!') != nullptr;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        precompile::gStore = argv[3];
        precompile::gQuartzCore = argv[4];
        precompile::gDevice = native_bridge_metal_open(argv[1], argv[2], 0, nullptr);
        if (!precompile::gDevice)
            return 1;
        std::ifstream input(argv[5]);
        std::ofstream output(argv[6]);
        std::string line;
        while (std::getline(input, line)) {
            if (line.rfind("# derived", 0) == 0)
                break;
            if (line.size() < 2 || line[1] != ' ' || !strchr(argv[7], line[0]))
                continue;
            @autoreleasepool {
                NSMutableArray *stages = [NSMutableArray array];
                bool ok = false;
                std::string why;
                if (line[0] == 'D') {
                    ok = dispatch(line, stages, why);
                } else if (line[0] == 'R') {
                    precompile::RenderInput descriptor;
                    NSError *error = nil;
                    if (precompile::renderInput(line, descriptor, &error) == precompile::Made) {
                        const char *refused = nullptr;
                        ok = native_bridge_metal_render_direct(descriptor.descriptor, false, 0, 1, nullptr, render,
                                                               (__bridge void *)stages, &refused);
                        why = refused ?: "";
                    } else {
                        why = error.localizedDescription.UTF8String ?: "missing library/function";
                    }
                }
                NSDictionary *row =
                    @{@"line" : @(line.c_str()),
                      @"made" : @(ok),
                      @"reason" : @(why.c_str()),
                      @"stages" : stages};
                NSData *json = [NSJSONSerialization dataWithJSONObject:row options:NSJSONWritingSortedKeys error:nil];
                output.write(static_cast<const char *>(json.bytes), json.length);
                output << '\n';
                output.flush();
            }
        }
        return input.bad() || output.bad();
    }
}
