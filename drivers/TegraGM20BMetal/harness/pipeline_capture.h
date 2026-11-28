// SPDX-License-Identifier: MIT
#pragma once
#include "../TegraGM20BMetalOffline.h"

namespace pipeline_capture {

struct SpecializationEntry {
    uint32_t id, offset;
    size_t size;
};

struct Specialization {
    uint32_t count;
    const SpecializationEntry *entries;
    size_t size;
    const void *data;
};

struct Render {
    NSString *folder;
    MTLRenderPipelineDescriptor *descriptor;
    bool (*each)(const NmbDirectState *, void *);
    void *ctx;
};

inline void writeData(NSData *data, NSString *file)
{
    NSError *error = nil;
    if (![data writeToFile:file options:NSDataWritingAtomic error:&error]) {
        fprintf(stderr, "directcheck pipeline capture: %s\n", error.localizedDescription.UTF8String);
        exit(1);
    }
}

inline bool writeState(const NmbDirectState *state, void *context)
{
    const auto &capture = *static_cast<Render *>(context);
    NSMutableArray *stages = [NSMutableArray array];
    NSMutableArray *sets = [NSMutableArray array];
    NSMutableArray *push = [NSMutableArray array];
    for (uint32_t i = 0; i < state->stageCount; ++i) {
        const auto &stage = state->stages[i];
        NSString *file = [NSString stringWithFormat:@"stage%u.spv", i];
        writeData([NSData dataWithBytes:stage.spirv length:stage.spirvBytes],
                  [capture.folder stringByAppendingPathComponent:file]);
        NSMutableArray *map = [NSMutableArray array];
        NSString *bytes = @"";
        if (const auto *spec = static_cast<const Specialization *>(stage.specialization)) {
            for (uint32_t j = 0; j < spec->count; ++j) {
                const auto &e = spec->entries[j];
                [map addObject:@[ @(e.id), @(e.offset), @(e.size) ]];
            }
            bytes = [[NSData dataWithBytes:spec->data length:spec->size] base64EncodedStringWithOptions:0];
        }
        [stages addObject:@{
            @"file" : file,
            @"stage" : @(stage.stage),
            @"entry" : @(stage.entry),
            @"flags" : @(stage.flags),
            @"subgroup" : @(stage.subgroupSize),
            @"map" : map,
            @"data" : bytes
        }];
    }
    const auto &layout = *state->layoutData;
    for (unsigned i = 0; i < layout.setCount; ++i) {
        const auto &set = layout.sets[i];
        NSMutableArray *bindings = [NSMutableArray array];
        NSMutableDictionary *samplers = [NSMutableDictionary dictionary];
        for (unsigned j = 0; j < set.bindingCount; ++j) {
            const auto &b = set.bindings[j];
            if (b.immutable) {
                const auto &s = b.sampler;
                samplers[[@(b.binding) stringValue]] = @[
                    @(s.magFilter), @(s.minFilter), @(s.mipmapMode), @(s.address[0]), @(s.address[1]), @(s.address[2]),
                    @(s.mipLodBias), @(s.anisotropy), @(s.maxAnisotropy), @(s.compare), @(s.compareOp), @(s.minLod),
                    @(s.maxLod), @(s.borderColor), @(s.unnormalized), @(s.reduction)
                ];
            }
            [bindings addObject:@[ @(b.binding), @(b.type), @(b.count), @(b.flags), @(b.immutable) ]];
        }
        [sets addObject:@{@"flags" : @(set.flags), @"bindings" : bindings, @"samplers" : samplers}];
    }
    for (unsigned i = 0; i < layout.pushCount; ++i) {
        const auto &r = layout.push[i];
        [push addObject:@[ @(r.stages), @(r.offset), @(r.size) ]];
    }
    MTLRenderPipelineDescriptor *desc = capture.descriptor;
    NSMutableArray *colors = [NSMutableArray array];
    for (unsigned i = 0; i < 8; ++i)
        [colors addObject:@(desc.colorAttachments[i].pixelFormat)];
    NSDictionary *info = @{
        @"stages" : stages,
        @"sets" : sets,
        @"push" : push,
        @"samples" : @(desc.rasterSampleCount),
        @"sample_shading" : @(state->sampleShading),
        @"storage" : @(state->storageForm),
        @"storage_mask" : @(state->storage.mask),
        @"color_formats" : colors,
        @"depth_format" : @(desc.depthAttachmentPixelFormat),
        @"stencil_format" : @(desc.stencilAttachmentPixelFormat)
    };
    writeData([NSJSONSerialization dataWithJSONObject:info options:NSJSONWritingPrettyPrinted error:nil],
              [capture.folder stringByAppendingPathComponent:@"pipeline.json"]);
    const char *stageRoot = getenv("DIRECTCHECK_STAGE_INPUTS");
    if (!stageRoot || !*stageRoot)
        return capture.each(state, capture.ctx);
    NmbDirectState replay = *state;
    __attribute__((objc_precise_lifetime)) NSMutableArray<NSData *> *held = [NSMutableArray array];
    for (uint32_t i = 0; i < state->stageCount; ++i) {
        NSString *base = [@(stageRoot) stringByAppendingPathComponent:[NSString stringWithFormat:@"stage%u", i]];
        NSData *original = [NSData dataWithContentsOfFile:[base stringByAppendingString:@".spv"]];
        NSData *replacement = [NSData dataWithContentsOfFile:[base stringByAppendingString:@".r32.spv"]];
        auto &stage = replay.stages[i];
        if (original.length != stage.spirvBytes || memcmp(original.bytes, stage.spirv, stage.spirvBytes) ||
            replacement.length < 20 || replacement.length % sizeof(uint32_t)) {
            fprintf(stderr, "directcheck pipeline capture: stage %u does not match %s\n", i, stageRoot);
            exit(1);
        }
        NSString *name = [NSString stringWithFormat:@"stage%u.used.spv", i];
        writeData(replacement, [capture.folder stringByAppendingPathComponent:name]);
        [held addObject:replacement];
        stage.spirv = static_cast<const uint32_t *>(replacement.bytes);
        stage.spirvBytes = replacement.length;
    }
    return capture.each(&replay, capture.ctx);
}

inline gm20b::DirectTranslator translator()
{
    auto translator = offline::translator();
    const char *dest = getenv("DIRECTCHECK_PIPELINES");
    if (!dest || !*dest)
        return translator;
    translator.render = [](id desc, bool barrier, NSUInteger format, NSUInteger samples, const NmbDirectBinds *binds,
                           bool (*each)(const NmbDirectState *, void *), void *ctx, const char **why) {
        static unsigned sequence = 0;
        NSString *root = @(getenv("DIRECTCHECK_PIPELINES"));
        NSString *folder = [root stringByAppendingPathComponent:[NSString stringWithFormat:@"%04u", sequence++]];
        NSError *error = nil;
        if (![[NSFileManager defaultManager] createDirectoryAtPath:folder
                                       withIntermediateDirectories:YES
                                                        attributes:nil
                                                             error:&error]) {
            fprintf(stderr, "directcheck pipeline capture: %s\n", error.localizedDescription.UTF8String);
            exit(1);
        }
        Render capture{folder, desc, each, ctx};
        return offline::translator().render(desc, barrier, format, samples, binds, writeState, &capture, why);
    };
    return translator;
}
}
