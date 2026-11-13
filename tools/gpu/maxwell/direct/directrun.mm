// SPDX-License-Identifier: MIT
// directrun: the raw path's direct compile (drivers/TegraGM20BMetal/raw/runtime/RawDirect.h) of the states and kernels
// of exec.metal and the counter resolve kernel, as the runtime makes each at its creation, written as JSON for
// direct_exec_test.py to execute: each record's programs, set-0 entries, push words and the fields its draws or
// dispatches write.
// usage: directrun <nvk dylib> <cache dir> <exec.metallib> <out.json>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstddef>
#include <cstdio>
#include <string>
#include "NativeBridgeMetalFrontend.h"
#include "raw/runtime/RawDirect.h"
#include "raw/runtime/RawPipelineKey.h"
#include "raw/runtime/raw_counter_kernel.h"
#include "raw/runtime/raw_emit.h"

namespace {

void missing(const char *, const char *)
{}

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

NSDictionary *program(const raw::CapturedProgram &p)
{
    return @{
        @"image" : hexOf(p.image),
        @"data" : hexOf(p.data),
        @"cbufs" : numbers(p.cbufTypes.data(), p.cbufTypes.size())
    };
}

NSDictionary *stateJson(const raw::RuntimeRecord &r)
{
    const raw::PackPipeline &p = r.pipeline;
    NSMutableArray *entries = [NSMutableArray array];
    for (uint32_t i = 0; i < p.entries; i++) {
        const raw::PackEntry &e = p.entry[i];
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
        @"push_mask" : @(p.pushMask),
        @"push_words" : words(p.pushWords, 32),
        @"set_bytes" : @(p.setBytes),
        @"vertex_slot" : @(p.vertexSlot),
        @"vertex_stride" : @(p.vertexStride),
        @"vertex_stride_dynamic" : @(p.vertexStrideDynamic),
        @"vertex_streams" : numbers(p.vertexStreams, sizeof p.vertexStreams),
        @"pulled_stride" : pulled,
        @"amplification" : @[ @(p.amplificationMax), @(p.amplificationIdWord), @(p.amplificationCountWord) ],
        @"amplification_offsets" : @[ @(r.amplificationLayerWord), @(r.amplificationViewportWord) ],
        @"pass_words" : @[ @(p.arrayLengthWord), @(p.viewportCountWord) ],
        @"storage" : @[ @(p.storageMask), @(p.storageWriteMask), @(p.storageTic[0]) ],
        @"control" : @[ @(p.controlWord), @(p.controlPoints), @(p.controlPointIndexBytes) ],
        @"blob" : words(r.blob.data(), r.blob.size()),
        @"extras" : extras,
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

    void state(NSString *name, NSString *vertex, NSString *fragment, void (^setUp)(MTLRenderPipelineDescriptor *))
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library_ newFunctionWithName:vertex];
        d.fragmentFunction = [library_ newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        setUp(d);
        const raw::StageKey v = {vertex.UTF8String, ""}, f = {fragment.UTF8String, ""};
        const std::string key = raw::pipelineKey(d, &v, &f, "", "");
        raw::RuntimeRecord record;
        std::string why;
        if (!d.vertexFunction || !d.fragmentFunction)
            why = "no function";
        else if (direct_.make(d, key, record, why) == raw::CompileOutcome::Made) {
            NSMutableDictionary *made = [stateJson(record) mutableCopy];
            made[@"key"] = @(key.c_str());
            states_[name] = made;
            return;
        }
        refused_[name] = @(why.c_str());
    }

    void kernel(NSString *name, NSString *function, uint32_t threads, void (^setUp)(MTLComputePipelineDescriptor *))
    {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [library_ newFunctionWithName:function];
        setUp(d);
        const uint32_t shape[3] = {threads, 1, 1};
        raw::KernelDispatch dispatch(shape, {});
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

    NSDictionary *results() const
    {
        return @{
            @"states" : states_,
            @"kernels" : kernels_,
            @"refused" : refused_,
            @"layouts" : @{@"counter" : counterLayout()}
        };
    }

private:
    id<MTLLibrary> library_;
    raw::Direct &direct_;
    NSMutableDictionary *states_ = [NSMutableDictionary dictionary], *kernels_ = [NSMutableDictionary dictionary],
                        *refused_ = [NSMutableDictionary dictionary];
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
    run.state(@"tessellation", @"tess_vertex", @"tess_fragment", ^(MTLRenderPipelineDescriptor *d) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        layout(v, 0, MTLVertexFormatFloat4, 0, 16, MTLVertexStepFunctionPerPatchControlPoint);
        d.vertexDescriptor = v;
        d.maxTessellationFactor = 16;
        d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
        d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        d.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
    });
    run.state(@"amplification", @"amp_vertex", @"amp_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.maxVertexAmplificationCount = 2;
    });
    run.state(@"layered_fetch", @"layered_fetch_vertex", @"layered_fetch_fragment", ^(MTLRenderPipelineDescriptor *d) {
        d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
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
    run.kernel(@"stage_in", @"stage_in_kernel", 8, ^(MTLComputePipelineDescriptor *d) {
        MTLStageInputOutputDescriptor *s = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        s.attributes[0].format = MTLAttributeFormatFloat2;
        s.attributes[0].bufferIndex = 0;
        s.layouts[0].stride = 12;
        s.layouts[0].stepFunction = MTLStepFunctionThreadPositionInGridX;
        d.stageInputDescriptor = s;
    });
    run.kernel(@"counter_resolve", @(raw::counters::kResolveFunction), raw::counters::kResolveThreads,
               ^(MTLComputePipelineDescriptor *){
               });
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 5) {
            fprintf(stderr, "usage: directrun <nvk dylib> <cache dir> <exec.metallib> <out.json>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        NSError *error = nil;
        id<MTLLibrary> library = dev ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error] : nil;
        if (!library) {
            fprintf(stderr, "directrun: %s\n", dev ? error.localizedDescription.UTF8String : "no device");
            return 1;
        }
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(std::move(rules));
        Runner run(library, direct);
        make(run);
        NSData *json = [NSJSONSerialization dataWithJSONObject:run.results()
                                                       options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                         error:&error];
        if (!json || ![json writeToFile:@(argv[4]) atomically:YES]) {
            fprintf(stderr, "directrun: %s not written\n", argv[4]);
            return 1;
        }
        return 0;
    }
}
