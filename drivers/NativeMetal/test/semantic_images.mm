#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../NativeBridgeMetalFrontend.h"
#include "../bridge/NativeMetalDirect.h"
#include "device_compile.h"
#include "../../../third_party/mesa/src/nouveau/vulkan/nvk_switch_ir.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

struct Case {
    const char *name;
    MTLPixelFormat format;
    MTLTextureType type;
    unsigned samples;
    bool admitted;
    NSString *directory;
    bool called = false;
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

static bool sameArtifact(const raw::NvkStage &a, const raw::NvkStage &b)
{
    return a.codeBytes == b.codeBytes && a.codeBytes && !memcmp(a.code, b.code, a.codeBytes) &&
           !memcmp(a.header, b.header, sizeof(a.header)) && a.dataBytes == b.dataBytes &&
           (!a.dataBytes || !memcmp(a.data, b.data, a.dataBytes)) && a.cbufCount == b.cbufCount &&
           !memcmp(a.cbufTypes, b.cbufTypes, sizeof(a.cbufTypes)) && a.numGprs == b.numGprs &&
           a.earlyFragmentTests == b.earlyFragmentTests && a.writesDepth == b.writesDepth &&
           a.postDepthCoverage == b.postDepthCoverage && !a.split && !b.split &&
           a.interlockControl == b.interlockControl && a.usesSampleShading == b.usesSampleShading;
}

static bool rejectMalformed(const NmbDirectKernel *kernel, const raw::nvk::DeviceNvk &compiler, raw::NvkStageInfo info,
                            const raw::NvkStage &reference)
{
    const auto &original = *static_cast<const switch_nvk_ir *>(kernel->stage.semantic);
    const auto *image = std::find_if(original.records, original.records + original.record_count, [](const auto &r) {
        return r.kind == 0 && r.opcode == 25;
    });
    const auto *operation = std::find_if(original.records, original.records + original.record_count, [](const auto &r) {
        return r.kind == 6 && r.opcode >= 86 && r.opcode <= 107;
    });
    if (image == original.records + original.record_count || operation == original.records + original.record_count)
        return false;
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        std::vector<switch_nvk_ir_record> records(original.records, original.records + original.record_count);
        std::vector<uint32_t> operands(original.operands, original.operands + original.operand_count);
        auto &type = records[image - original.records];
        auto &op = records[operation - original.records];
        switch (mutation) {
        case 0:
            --type.count;
            break;
        case 1:
            operands[type.first + 1] = UINT32_MAX;
            break;
        case 2:
            operands[type.first + 6] = UINT32_MAX;
            break;
        case 3:
            --op.count;
            break;
        case 4:
            operands[op.first] = UINT32_MAX;
            break;
        }
        switch_nvk_ir malformed = original;
        malformed.records = records.data();
        malformed.operands = operands.data();
        info.semantic = &malformed;
        raw::NvkStage result = {};
        int status = raw::nvk::compileStage(compiler, raw::handle(kernel->device), info, result);
        bool okay = !status && !result.semanticVersion && sameArtifact(result, reference);
        raw::nvk::stageFinish(compiler, raw::handle(kernel->device), result);
        if (!okay)
            return false;
    }
    return true;
}

static bool compile(const NmbDirectKernel *kernel, void *context)
{
    auto &test = *static_cast<Case *>(context);
    test.called = true;
    if (kernel->stage.semantic) {
        const auto &p = *static_cast<const switch_nvk_ir *>(kernel->stage.semantic);
        uint32_t header[] = {p.version, p.types, p.values, p.blocks, p.entry, p.record_count, p.operand_count};
        NSMutableData *data = [NSMutableData dataWithBytes:header length:sizeof header];
        [data appendBytes:p.records length:p.record_count * sizeof(*p.records)];
        [data appendBytes:p.operands length:p.operand_count * sizeof(*p.operands)];
        [data writeToFile:[test.directory
                              stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.typed", test.name]]
               atomically:YES];
    }
    auto compiler = raw::nvk::DeviceNvk::find(native_bridge_metal_nvk_symbol);
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
    raw::NvkStage stages[2] = {};
    bool okay = true;
    for (unsigned route = 0; route < 2; ++route) {
        if (route)
            info.semantic = nullptr;
        fprintf(stderr, "compile %s %s\n", test.name, route ? "portable" : "native");
        const int result = raw::nvk::compileStage(compiler, raw::handle(kernel->device), info, stages[route]);
        okay &=
            result == 0 && stages[route].codeBytes && (route || bool(stages[route].semanticVersion) == test.admitted);
        if (!route)
            okay &= kernel->stage.spirvBytes == 0 && materialize.calls == (test.admitted ? 0 : 1);
        NSString *name = [NSString stringWithFormat:@"%s.%s.sass", test.name, route ? "portable" : "native"];
        [[NSData dataWithBytes:stages[route].code
                        length:stages[route].codeBytes] writeToFile:[test.directory stringByAppendingPathComponent:name]
                                                         atomically:YES];
    }
    bool exact = sameArtifact(stages[0], stages[1]);
    okay &= exact;
    if (test.admitted && okay)
        okay &= rejectMalformed(kernel, compiler, info, stages[1]);
    printf("%s %s typed=%u admitted=%u native=%u portable=%u exact=%u\n", okay ? "PASS" : "FAIL", test.name,
           kernel->stage.semantic != nullptr, stages[0].semanticVersion, stages[0].codeBytes, stages[1].codeBytes,
           exact);
    for (auto &stage : stages)
        raw::nvk::stageFinish(compiler, raw::handle(kernel->device), stage);
    return okay;
}

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, nullptr);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!library) {
            fprintf(stderr, "library: %s\n", error.description.UTF8String);
            return 1;
        }
        constexpr auto u = MTLPixelFormatRGBA32Uint, f = MTLPixelFormatRGBA32Float, d = MTLPixelFormatDepth32Float;
        Case cases[] = {
            {"semantic_image_read", u, MTLTextureType2D, 1, true, @(argv[4])},
            {"semantic_image_store", u, MTLTextureType2D, 1, true, @(argv[4])},
            {"semantic_image_rw", u, MTLTextureType2D, 1, true, @(argv[4])},
            {"semantic_image_sample", f, MTLTextureType2D, 1, true, @(argv[4])},
            {"semantic_image_compare", d, MTLTextureType2D, 1, true, @(argv[4])},
            {"semantic_image_ms", f, MTLTextureType2DMultisample, 4, true, @(argv[4])},
            {"semantic_image_array", u, MTLTextureType2DArray, 1, true, @(argv[4])},
            {"semantic_image_3d", u, MTLTextureType3D, 1, true, @(argv[4])},
            {"semantic_image_gradient", f, MTLTextureType2D, 1, true, @(argv[4])},
            {"semantic_image_gather", f, MTLTextureType2D, 1, false, @(argv[4])},
        };
        unsigned failures = 0;
        for (auto &test : cases) {
            auto *descriptor = [MTLComputePipelineDescriptor new];
            descriptor.computeFunction = [library newFunctionWithName:@(test.name)];
            NmbDirectBoundTexture texture = {0,
                                             test.format,
                                             test.type,
                                             MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
                                             test.samples == 1 ? 2u : 1u,
                                             test.type == MTLTextureType2DArray ? 2u : 1u,
                                             test.samples};
            NmbDirectDispatch dispatch = {};
            dispatch.threads[0] = 32;
            dispatch.threads[1] = dispatch.threads[2] = 1;
            dispatch.textures = &texture;
            dispatch.textureCount = 1;
            const char *why = nullptr;
            if (!native_bridge_metal_compute_direct(descriptor, &dispatch, compile, &test, &why) || !test.called) {
                fprintf(stderr, "%s: %s\n", test.name, why ?: "compile rejected");
                ++failures;
            }
        }
        return failures ? 1 : 0;
    }
}
