// SPDX-License-Identifier: MIT
// directrun: the driver's direct compile (drivers/TegraGM20BMetal/TegraGM20BMetalDirect.h) of the states and kernels
// of exec.metal and the counter resolve kernel, as the runtime makes each at its creation, written as JSON for
// direct_exec_test.py to execute: each record's programs, set-0 entries, push words and the fields its draws or
// dispatches write.
// usage: directrun <exec.metallib> <out.json>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include "TegraGM20BMetalDirect.h"
#include "TegraGM20BMetalOffline.h"
#include "TegraGM20BMetalPipelineKey.h"
#include "compute_key.h"
#include "counter_kernel.h"
#include "emit.h"
#include "fragment_tests.h"
#include "stage_textures.h"

namespace {

NSString *hexOf(const std::vector<uint8_t> &bytes)
{
    NSMutableString *out = [NSMutableString stringWithCapacity:bytes.size() * 2];
    for (uint8_t b : bytes)
        [out appendFormat:@"%02x", b];
    return out;
}

NSArray *numbers(const uint8_t *values, size_t count)
{
    NSMutableArray *out = [NSMutableArray array];
    for (size_t i = 0; i < count; i++)
        [out addObject:@(values[i])];
    return out;
}

NSArray *words(const uint32_t *values, size_t count)
{
    NSMutableArray *out = [NSMutableArray array];
    for (size_t i = 0; i < count; i++)
        [out addObject:@(values[i])];
    return out;
}

// A program's uniform buffers past set-0 byte 496 (pack.h PackProgram::farUbos): [constant buffer slot, set-0 byte]
NSArray *farUbos(const gm20b::CapturedProgram &p)
{
    NSMutableArray *out = [NSMutableArray array];
    for (size_t c = 0; c < p.farUbos.size(); c++)
        if (p.farUbos[c])
            [out addObject:@[ @(c), @(uint32_t(p.farUbos[c]) * 16) ]];
    return out;
}

NSDictionary *program(const gm20b::CapturedProgram &p)
{
    return @{
        @"image" : hexOf(p.image),
        @"data" : hexOf(p.data),
        @"cbufs" : numbers(p.cbufTypes.data(), p.cbufTypes.size()),
        @"bound_textures" : @(p.boundTextures),
        @"ubo_offsets" : farUbos(p),
    };
}

// What a draw through argument buffers binds of the record: the root table's and ExecutionStatus's push words + 1 of each
// stage (vertex, fragment), the stages' root-table slots and set 1's heap arrays
NSDictionary *arguments(const gm20b::PackPipeline &p)
{
    NSMutableArray *heap = [NSMutableArray array];
    for (uint32_t k = 0; k < p.heapArrays; k++)
        [heap addObject:@[ @(p.heap[k].heapClass), @(p.heap[k].offset) ]];
    return @{
        @"root_table_word" : numbers(p.rootTableWord, 2),
        @"status_word" : numbers(p.statusWord, 2),
        @"root_slots" : words(p.rootSlots, 2),
        @"heap" : heap,
        @"heap_bytes" : @(p.heapBytes),
    };
}

NSDictionary *stateJson(const gm20b::RuntimeRecord &r)
{
    const gm20b::PackPipeline &p = r.pipeline;
    NSMutableArray *entries = [NSMutableArray array];
    const gm20b::SetEntries all = gm20b::entriesOf(r);
    for (uint32_t i = 0; i < all.size(); i++) {
        const gm20b::PackEntry &e = all[i];
        [entries
            addObject:@[ @(e.offset), @(e.kind), @(e.stage), @(e.slot), @(e.samplerKind), @(e.sampler), @(e.pad) ]];
    }
    NSMutableArray *pulled = [NSMutableArray array];
    for (const auto &s : p.pulledStride)
        [pulled addObject:@[ @(s[0]), @(s[1]) ]];
    NSMutableArray *extras = [NSMutableArray array];
    for (const auto &[m, v] : r.extras)
        [extras addObject:@[ @(m), @(v) ]];
    const bool tess = gm20b::tessellates(p);
    NSMutableDictionary *out = [@{
        @"programs" : @[ program(r.programs[0]), program(r.programs[1]) ],
        @"tess_programs" : tess ? @[ program(r.tessPrograms[0]), program(r.tessPrograms[1]) ] : @[],
        @"entries" : entries,
        @"entry_count" : @(gm20b::entryCount(p)),
        @"spill" : @[
            words(r.spill.data(), std::min<size_t>(p.spillWords[0], r.spill.size())),
            words(r.spill.data() + std::min<size_t>(p.spillWords[0], r.spill.size()),
                  r.spill.size() - std::min<size_t>(p.spillWords[0], r.spill.size()))
        ],
        @"push_mask" : @(p.pushMask),
        @"push_words" : words(p.pushWords, 32),
        @"set_bytes" : @(p.setBytes),
        @"vertex_slot" : @(p.vertexSlot),
        @"vertex_stride" : @(p.vertexStride),
        @"vertex_stride_dynamic" : @(p.vertexStrideDynamic),
        @"vertex_streams" : numbers(p.vertexStreams, sizeof p.vertexStreams),
        @"pulled_stride" : pulled,
        @"amplification" : @[ @(p.amplificationMax), @(p.amplificationIdWord), @(p.amplificationCountWord) ],
        @"amplification_offsets" : @[ @(p.amplificationLayerWord), @(p.amplificationViewportWord) ],
        @"pass_words" : @[ @(p.arrayLengthWord), @(p.viewportCountWord) ],
        @"storage" : @[ @(p.storageMask), @(p.storageWriteMask), @(p.storageTic[0]) ],
        @"control" : @[ @(p.controlWord), @(p.controlPoints), @(p.controlPointIndexBytes) ],
        @"shader_tests" : @[
            @(p.shaderTests.depthWord), @(p.shaderTests.stencilWord), @(p.shaderTests.flags), @(p.shaderTests.reserved),
            @(p.shaderTests.depthTic), @(p.shaderTests.stencilTic), @(p.storageTests)
        ],
        @"blob" : words(r.blob.data(), r.blob.size()),
        @"extras" : extras,
        @"arguments" : arguments(p),
    } mutableCopy];
    if (r.strideTwin)
        out[@"stride_twin"] = stateJson(*r.strideTwin);
    return out;
}

// A kernel record's sampler headers, word 0 first, as its sampler entries number them (CONSTEXPR | n)
NSArray *samplerHeaders(const gm20b::ComputeRecord &r)
{
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &h : r.samplers)
        [out addObject:words(h.data(), uint32_t(h.size()))];
    return out;
}

NSDictionary *kernelJson(const gm20b::ComputeRecord &r)
{
    const gm20b::PackKernel &k = r.kernel;
    NSMutableArray *entries = [NSMutableArray array];
    for (uint32_t i = 0; i < k.entries; i++)
        [entries addObject:@[ @(k.entry[i].offset), @(k.entry[i].kind), @(k.entry[i].slot), @(k.entry[i].window) ]];
    NSMutableArray *strides = [NSMutableArray array];
    for (const auto &s : k.stageInStride)
        [strides addObject:@[ @(s[0]), @(s[1]) ]];
    return @{
        @"code" : hexOf(r.code),
        @"data" : hexOf(r.data),
        @"cbufs" : numbers(r.cbufType, r.cbufs),
        @"entries" : entries,
        @"region_word" : @(k.regionWord),
        @"set_bytes" : @(k.setBytes),
        @"push_mask" : @(k.pushMask),
        @"push_words" : words(k.pushWords, 32),
        @"stage_in_region_word" : @(k.stageInRegionWord),
        @"stage_in_stride" : strides,
        @"qmd" : words(k.qmd, 64),
        @"samplers" : samplerHeaders(r),
        @"maps_stated" : @(k.mapsStated),
    };
}

// A field as [byte offset, bytes]
#define SWITCH_FIELD(type, field) @[ @(offsetof(type, field)), @(sizeof(type::field)) ]

// The counter resolve kernels' buffers as counter_kernel.h and TegraGM20BClockABI.h lay them out: its records, its
// parameters (the byte resolve's with its shift) and the kernel's clock anchor table
NSDictionary *counterLayout()
{
    using gm20b::counters::Record;
    using gm20b::counters::ResolveBytesParams;
    using gm20b::counters::ResolveParams;
    return @{
        @"record" : @{
            @"bytes" : @(sizeof(Record)),
            @"payload" : SWITCH_FIELD(Record, payload),
            @"units" : SWITCH_FIELD(Record, units)
        },
        @"sample_bytes" : @(gm20b::counters::kSampleBytes),
        @"params" : @{@"bytes" : @(sizeof(ResolveParams)), @"count" : SWITCH_FIELD(ResolveParams, count)},
        @"bytes_params" : @{
            @"bytes" : @(sizeof(ResolveBytesParams)),
            @"count" : SWITCH_FIELD(ResolveBytesParams, count),
            @"shift" : SWITCH_FIELD(ResolveBytesParams, shift)
        },
        @"anchors" : @{
            @"bytes" : @(sizeof(TegraGM20BAbiClockAnchors)),
            @"published" : SWITCH_FIELD(TegraGM20BAbiClockAnchors, published),
            @"slot" : @(offsetof(TegraGM20BAbiClockAnchors, slot)),
            @"slots" : @(TegraGM20BClockAnchorSlots),
        },
        @"anchor" : @{
            @"bytes" : @(sizeof(TegraGM20BAbiClockAnchor)),
            @"units" : SWITCH_FIELD(TegraGM20BAbiClockAnchor, units),
            @"ns" : SWITCH_FIELD(TegraGM20BAbiClockAnchor, ns)
        },
    };
}

class Runner {
public:
    Runner(id<MTLLibrary> library, gm20b::Direct &direct) : library_(library), direct_(direct)
    {}

    // `tests`: the test class the state is made for (its " | tests=" variant, fragment_tests.h), 0 for none
    void state(NSString *name, NSString *vertex, NSString *fragment, void (^setUp)(MTLRenderPipelineDescriptor *),
               uint8_t tests = 0, const gm20b::CubicBinds &cubic = {})
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library_ newFunctionWithName:vertex];
        d.fragmentFunction = [library_ newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        setUp(d);
        const gm20b::StageKey v = {vertex.UTF8String, ""}, f = {fragment.UTF8String, ""};
        const std::string key = gm20b::pipelineKey(d, &v, &f, "", "") + (cubic.empty() ? "" : cubic.key());
        gm20b::RuntimeRecord record;
        std::string why;
        gm20b::ProbeTextures probe;
        probe.fragmentTests = tests;
        probe.cubic = cubic;
        if (!d.vertexFunction || !d.fragmentFunction)
            why = "no function";
        else if (direct_.make(d, key, record, why, probe) == gm20b::CompileOutcome::Made) {
            NSMutableDictionary *made = [stateJson(record) mutableCopy];
            made[@"key"] = @(key.c_str());
            states_[name] = made;
            return;
        }
        refused_[name] = @(why.c_str());
    }

    // `textures` and `samplers`: what the dispatch binds, which its key names (compute_key.h ":am" with a map)
    void kernel(NSString *name, NSString *function, uint32_t threads, void (^setUp)(MTLComputePipelineDescriptor *),
                const std::map<uint32_t, uint32_t> &memory = {}, const std::vector<gm20b::BoundTexture> &textures = {},
                const std::vector<gm20b::BoundSampler> &samplers = {})
    {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [library_ newFunctionWithName:function];
        setUp(d);
        const uint32_t shape[3] = {threads, 1, 1};
        gm20b::KernelDispatch dispatch(shape, memory);
        for (const gm20b::BoundTexture &t : textures) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor new];
            td.pixelFormat = MTLPixelFormat(t.pixelFormat);
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            dispatch.texture(t.slot, td);
        }
        for (const gm20b::BoundSampler &s : samplers) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = MTLSamplerMinMagFilter(s.minFilter);
            sd.magFilter = MTLSamplerMinMagFilter(s.magFilter);
            sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = MTLSamplerAddressMode(s.sAddress);
            dispatch.sampler(s.slot, sd);
        }
        const std::string key = std::string("kernel ") + function.UTF8String +
                                " | tex=" + gm20b::computeTextures(textures.data(), textures.size()) + " | smp=";
        gm20b::ComputeRecord record;
        std::string why;
        if (!d.computeFunction)
            why = "no function";
        else if (direct_.kernel(d, dispatch.dispatch(), key, record, why) == gm20b::CompileOutcome::Made) {
            kernels_[name] = kernelJson(record);
            return;
        }
        refused_[name] = @(why.c_str());
    }

    id<MTLFunction> function(MTLFunctionDescriptor *descriptor) const
    {
        return [library_ newFunctionWithDescriptor:descriptor error:nil];
    }

    void answers(NSString *name, MTLComputePipelineDescriptor *descriptor)
    {
        uint32_t facts[3] = {};
        std::string reflection, why;
        if (!offline::kernelFacts(descriptor, facts, why) || !offline::computeReflection(descriptor, reflection, why)) {
            refused_[name] = @(why.c_str());
            return;
        }
        answers_[name] = @{
            @"facts" : words(facts, 3),
            @"reflection" : hexOf(std::vector<uint8_t>(reflection.begin(), reflection.end()))
        };
    }

    NSDictionary *results() const
    {
        return @{
            @"states" : states_,
            @"kernels" : kernels_,
            @"refused" : refused_,
            @"answers" : answers_,
            @"layouts" : @{@"counter" : counterLayout()},
            @"stage_textures" : @(gm20b::kStageTextures),
        };
    }

private:
    id<MTLLibrary> library_;
    gm20b::Direct &direct_;
    NSMutableDictionary *states_ = [NSMutableDictionary dictionary], *kernels_ = [NSMutableDictionary dictionary],
                        *refused_ = [NSMutableDictionary dictionary], *answers_ = [NSMutableDictionary dictionary];
};

void layout(MTLVertexDescriptor *v, uint32_t attribute, MTLVertexFormat format, uint32_t buffer, NSUInteger stride,
            MTLVertexStepFunction step = MTLVertexStepFunctionPerVertex)
{
    v.attributes[attribute].format = format;
    v.attributes[attribute].bufferIndex = buffer;
    v.layouts[buffer].stride = stride;
    v.layouts[buffer].stepFunction = step;
}

void make(Runner &run)
{
    run.state(@"cubic_vertex_linear", @"cubic_vertex", @"wide_fragment",
              ^(MTLRenderPipelineDescriptor *){
              });
    for (const auto &[name, format] : {std::pair{@"cubic_vertex_half", MTLPixelFormatRGBA16Float},
                                       std::pair{@"cubic_vertex_float", MTLPixelFormatRGBA32Float}}) {
        gm20b::CubicBinds cubic;
        cubic.samplers.push_back({0, 0, 0, 0, 0, true, gm20b::kCubicVertex});
        cubic.textures.push_back({0, uint32_t(format), gm20b::kCubicVertex});
        run.state(name, @"cubic_vertex", @"wide_fragment",
                  ^(MTLRenderPipelineDescriptor *){
                  },
                  0, cubic);
    }
    gm20b::CubicBinds fragmentOnly;
    fragmentOnly.samplers.push_back({0, 0, 0, 0, 0, true});
    fragmentOnly.textures.push_back({0, uint32_t(MTLPixelFormatRGBA16Float)});
    run.state(@"cubic_fragment_only", @"cubic_vertex", @"wide_fragment",
              ^(MTLRenderPipelineDescriptor *){
              },
              0, fragmentOnly);
    MTLFunctionDescriptor *binary = [MTLFunctionDescriptor functionDescriptor];
    binary.name = @"binary_affine_definition";
    binary.specializedName = @"binary_affine_alias";
    binary.options = MTLFunctionOptionCompileToBinary;
    id<MTLFunction> affine = run.function(binary);
    run.kernel(@"binary_affine", @"binary_affine_kernel", 32, ^(MTLComputePipelineDescriptor *descriptor) {
        MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
        linked.binaryFunctions = @[ affine ];
        descriptor.linkedFunctions = linked;
        run.answers(@"binary_affine", descriptor);
    });
    binary.specializedName = @"wrong_alias";
    id<MTLFunction> wrong = run.function(binary);
    run.kernel(@"binary_wrong_alias", @"binary_affine_kernel", 32, ^(MTLComputePipelineDescriptor *descriptor) {
        MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
        linked.binaryFunctions = @[ wrong ];
        descriptor.linkedFunctions = linked;
    });
    run.state(@"tessellation", @"tess_vertex", @"tess_fragment", ^(MTLRenderPipelineDescriptor *d) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        layout(v, 0, MTLVertexFormatFloat4, 0, 16, MTLVertexStepFunctionPerPatchControlPoint);
        d.vertexDescriptor = v;
        d.maxTessellationFactor = 16;
        d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
        d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        d.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
    });
    run.state(@"tessellation64", @"tess_vertex", @"tess_fragment", ^(MTLRenderPipelineDescriptor *d) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        layout(v, 0, MTLVertexFormatFloat4, 0, 16, MTLVertexStepFunctionPerPatchControlPoint);
        d.vertexDescriptor = v;
        d.maxTessellationFactor = 64;
        d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
        d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        d.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
    });
    run.state(@"amplification", @"amp_vertex", @"amp_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.maxVertexAmplificationCount = 2;
    });
    run.state(@"amplification8", @"amp_vertex", @"amp_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.maxVertexAmplificationCount = 8;
    });
    run.state(@"interpolants", @"interpolants_vertex", @"interpolants_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    });
    run.state(@"layered_fetch", @"layered_fetch_vertex", @"layered_fetch_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    });
    run.state(@"argument_textures", @"argument_vertex", @"argument_textures_fragment",
              ^(MTLRenderPipelineDescriptor *){
              });
    run.state(@"wide_static", @"wide_vertex", @"wide_fragment", ^(MTLRenderPipelineDescriptor *d) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        layout(v, 0, MTLVertexFormatFloat2, 1, 2052);
        layout(v, 1, MTLVertexFormatFloat4, 2, 4100);
        layout(v, 2, MTLVertexFormatFloat4, 3, 16);
        d.vertexDescriptor = v;
    });
    run.state(@"wide_dynamic", @"dynamic_vertex", @"wide_fragment", ^(MTLRenderPipelineDescriptor *d) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        layout(v, 0, MTLVertexFormatFloat2, 1, MTLBufferLayoutStrideDynamic);
        d.vertexDescriptor = v;
    });
    run.state(@"five_dynamic", @"five_dynamic_vertex", @"wide_fragment", ^(MTLRenderPipelineDescriptor *d) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        for (uint32_t k = 0; k < 5; k++)
            layout(v, k, MTLVertexFormatFloat2, k + 1, MTLBufferLayoutStrideDynamic);
        d.vertexDescriptor = v;
    });
    run.state(@"shadow_stencil", @"shadow_vertex", @"shadow_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    });
    run.state(@"shadow_unattached", @"shadow_vertex", @"shadow_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    });
    run.state(
        @"shadow_tested", @"shadow_vertex", @"shadow_fragment",
        ^(MTLRenderPipelineDescriptor *d) {
            d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        },
        uint8_t(gm20b::FragmentTests::StencilWrite));
    run.state(@"wide_set", @"wide_set_vertex", @"wide_set_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    });
    run.state(@"far_uniform", @"wide_set_vertex", @"far_uniform_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    });
    run.state(@"two_far", @"wide_set_vertex", @"two_far_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    });
    run.state(@"field_bound", @"bound_vertex", @"bound_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    });
    run.state(@"field_bound_wide", @"bound_wide_vertex", @"bound_wide_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    });
    run.kernel(@"stage_in", @"stage_in_kernel", 8, ^(MTLComputePipelineDescriptor *d) {
        MTLStageInputOutputDescriptor *s = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        s.attributes[0].format = MTLAttributeFormatFloat2;
        s.attributes[0].bufferIndex = 0;
        s.layouts[0].stride = 12;
        s.layouts[0].stepFunction = MTLStepFunctionThreadPositionInGridX;
        d.stageInputDescriptor = s;
    });
    run.kernel(@"wide_threadgroup", @"wide_threadgroup_kernel", 256,
               ^(MTLComputePipelineDescriptor *){
               });
    run.kernel(@"past_threadgroup", @"past_threadgroup_kernel", 256,
               ^(MTLComputePipelineDescriptor *){
               });
    run.kernel(@"dynamic_threadgroup", @"dynamic_threadgroup_kernel", 256,
               ^(MTLComputePipelineDescriptor *){
               },
               {{0, 48u << 10}});
    run.kernel(@"past_dynamic_threadgroup", @"dynamic_threadgroup_kernel", 256,
               ^(MTLComputePipelineDescriptor *){
               },
               {{0, (48u << 10) + 16}});
    run.kernel(@"counter_resolve", @(gm20b::counters::kResolveFunction), gm20b::counters::kResolveThreads,
               ^(MTLComputePipelineDescriptor *){
               });
    run.kernel(@"counter_resolve_bytes", @(gm20b::counters::kResolveBytesFunction), gm20b::counters::kResolveThreads,
               ^(MTLComputePipelineDescriptor *){
               });
    // a texture raw samples through an image it converts (NativeMetalKernelTextures.rs), its sampler repeating and
    // nearest (the translator emulates a converted texture's linear filter with fetches of its own)
    constexpr uint64_t kRepeat = MTLSamplerAddressModeRepeat;
    const std::vector<gm20b::BoundSampler> repeat = {{0, 0, 0, 0, kRepeat, kRepeat, kRepeat, 1, 0, 0, 0, 1000, true}};
    const auto convertedAt = [&](NSString *name, NSString *function, uint64_t format, uint8_t map) {
        gm20b::BoundTexture t = {0, format, MTLTextureType2D, 1, 1, {2, 3, 4, 5}};
        t.addressMap = map;
        run.kernel(name, function, 32,
                   ^(MTLComputePipelineDescriptor *){
                   },
                   {}, {t}, repeat);
    };
    convertedAt(@"converted_r10_packed", @"converted_sample_kernel", 588, 0);
    convertedAt(@"converted_rg10_packed", @"converted_sample_kernel", 589, 0);
    convertedAt(@"converted_420", @"converted_sample_kernel", 500, 1);
    convertedAt(@"converted_420_p3", @"converted_sample_kernel", 500, 3);
    convertedAt(@"converted_r10_packed_write", @"converted_write_kernel", 588, 0);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "usage: directrun <exec.metallib> <out.json>\n");
            return 2;
        }
        GM20BOfflineDevice *dev = [GM20BOfflineDevice new];
        NSError *error = nil;
        id<MTLLibrary> library = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        if (!library) {
            fprintf(stderr, "directrun: %s\n", dev ? error.localizedDescription.UTF8String : "no device");
            return 1;
        }
        gm20b::PackRules rules;
        rules.varying = gm20b::emittableMethods();
        gm20b::Direct direct(std::move(rules), false, offline::translator());
        Runner run(library, direct);
        make(run);
        NSData *json = [NSJSONSerialization dataWithJSONObject:run.results()
                                                       options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                         error:&error];
        if (!json || ![json writeToFile:@(argv[2]) atomically:YES]) {
            fprintf(stderr, "directrun: %s not written\n", argv[2]);
            return 1;
        }
        return 0;
    }
}
