// SPDX-License-Identifier: MIT
// directrun: the raw path's direct compile (drivers/TegraGM20BMetal/raw/runtime/RawDirect.h) of the states and kernels
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
#include "raw/runtime/RawDirect.h"
#include "raw/runtime/RawOffline.h"
#include "raw/runtime/RawPipelineKey.h"
#include "raw/runtime/raw_counter_kernel.h"
#include "raw/runtime/raw_emit.h"
#include "raw/runtime/raw_fragment_tests.h"
#include "raw/runtime/raw_stage_textures.h"

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

// A program's uniform buffers past set-0 byte 496 (raw_pack.h PackProgram::farUbos): [constant buffer slot, set-0 byte]
NSArray *farUbos(const raw::CapturedProgram &p)
{
    NSMutableArray *out = [NSMutableArray array];
    for (size_t c = 0; c < p.farUbos.size(); c++)
        if (p.farUbos[c])
            [out addObject:@[ @(c), @(uint32_t(p.farUbos[c]) * 16) ]];
    return out;
}

NSDictionary *program(const raw::CapturedProgram &p)
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
NSDictionary *arguments(const raw::PackPipeline &p)
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

NSDictionary *stateJson(const raw::RuntimeRecord &r)
{
    const raw::PackPipeline &p = r.pipeline;
    NSMutableArray *entries = [NSMutableArray array];
    const raw::SetEntries all = raw::entriesOf(r);
    for (uint32_t i = 0; i < all.size(); i++) {
        const raw::PackEntry &e = all[i];
        [entries
            addObject:@[ @(e.offset), @(e.kind), @(e.stage), @(e.slot), @(e.samplerKind), @(e.sampler), @(e.pad) ]];
    }
    NSMutableArray *pulled = [NSMutableArray array];
    for (const auto &s : p.pulledStride)
        [pulled addObject:@[ @(s[0]), @(s[1]) ]];
    NSMutableArray *extras = [NSMutableArray array];
    for (const auto &[m, v] : r.extras)
        [extras addObject:@[ @(m), @(v) ]];
    const bool tess = raw::tessellates(p);
    NSMutableDictionary *out = [@{
        @"programs" : @[ program(r.programs[0]), program(r.programs[1]) ],
        @"tess_programs" : tess ? @[ program(r.tessPrograms[0]), program(r.tessPrograms[1]) ] : @[],
        @"entries" : entries,
        @"entry_count" : @(raw::entryCount(p)),
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

NSDictionary *kernelJson(const raw::ComputeRecord &r)
{
    const raw::PackKernel &k = r.kernel;
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
    };
}

// A field as [byte offset, bytes]
#define SWITCH_FIELD(type, field) @[ @(offsetof(type, field)), @(sizeof(type::field)) ]

// The counter resolve kernel's buffers as raw_counter_kernel.h and TegraGM20BClockABI.h lay them out: its records, its
// parameters and the kernel's clock anchor table
NSDictionary *counterLayout()
{
    using raw::counters::Record;
    using raw::counters::ResolveParams;
    return @{
        @"record" : @{
            @"bytes" : @(sizeof(Record)),
            @"payload" : SWITCH_FIELD(Record, payload),
            @"units" : SWITCH_FIELD(Record, units)
        },
        @"sample_bytes" : @(raw::counters::kSampleBytes),
        @"params" : @{@"bytes" : @(sizeof(ResolveParams)), @"count" : SWITCH_FIELD(ResolveParams, count)},
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
    Runner(id<MTLLibrary> library, raw::Direct &direct) : library_(library), direct_(direct)
    {}

    // `tests`: the test class the state is made for (its " | tests=" variant, raw_fragment_tests.h), 0 for none
    void state(NSString *name, NSString *vertex, NSString *fragment, void (^setUp)(MTLRenderPipelineDescriptor *),
               uint8_t tests = 0, const raw::CubicBinds &cubic = {})
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library_ newFunctionWithName:vertex];
        d.fragmentFunction = [library_ newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        setUp(d);
        const raw::StageKey v = {vertex.UTF8String, ""}, f = {fragment.UTF8String, ""};
        const std::string key = raw::pipelineKey(d, &v, &f, "", "") + (cubic.empty() ? "" : cubic.key());
        raw::RuntimeRecord record;
        std::string why;
        raw::ProbeTextures probe;
        probe.fragmentTests = tests;
        probe.cubic = cubic;
        if (!d.vertexFunction || !d.fragmentFunction)
            why = "no function";
        else if (direct_.make(d, key, record, why, probe) == raw::CompileOutcome::Made) {
            NSMutableDictionary *made = [stateJson(record) mutableCopy];
            made[@"key"] = @(key.c_str());
            states_[name] = made;
            return;
        }
        refused_[name] = @(why.c_str());
    }

    void kernel(NSString *name, NSString *function, uint32_t threads, void (^setUp)(MTLComputePipelineDescriptor *),
                const std::map<uint32_t, uint32_t> &memory = {})
    {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [library_ newFunctionWithName:function];
        setUp(d);
        const uint32_t shape[3] = {threads, 1, 1};
        raw::KernelDispatch dispatch(shape, memory);
        raw::ComputeRecord record;
        std::string why;
        if (!d.computeFunction)
            why = "no function";
        else if (direct_.kernel(d, dispatch.dispatch(), std::string("kernel ") + function.UTF8String, record, why) ==
                 raw::CompileOutcome::Made) {
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
            @"stage_textures" : @(raw::kStageTextures),
        };
    }

private:
    id<MTLLibrary> library_;
    raw::Direct &direct_;
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
        raw::CubicBinds cubic;
        cubic.samplers.push_back({0, 0, 0, 0, 0, true, raw::kCubicVertex});
        cubic.textures.push_back({0, uint32_t(format), raw::kCubicVertex});
        run.state(name, @"cubic_vertex", @"wide_fragment",
                  ^(MTLRenderPipelineDescriptor *){
                  },
                  0, cubic);
    }
    raw::CubicBinds fragmentOnly;
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
        uint8_t(raw::FragmentTests::StencilWrite));
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
    run.kernel(@"counter_resolve", @(raw::counters::kResolveFunction), raw::counters::kResolveThreads,
               ^(MTLComputePipelineDescriptor *){
               });
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "usage: directrun <exec.metallib> <out.json>\n");
            return 2;
        }
        RawOfflineDevice *dev = [RawOfflineDevice new];
        NSError *error = nil;
        id<MTLLibrary> library = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        if (!library) {
            fprintf(stderr, "directrun: %s\n", dev ? error.localizedDescription.UTF8String : "no device");
            return 1;
        }
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(std::move(rules), false, offline::translator());
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
