// SPDX-License-Identifier: MIT
// The facade's reflection objects (TegraGM20BMetalReflectionObjects.mm) of a blob answer as this Mac's Metal
// answers the same state, laid out as the translator lays out a reflection (metal_reflection_blob.h): every binding
// type (buffers of a vector, a struct, a union of two anonymous structs, a scalar; textures alone and in arrays,
// depth, written; samplers alone and in arrays; threadgroup memory; a tile function's imageblock) and type class,
// compute, render and tile, selector by selector and in which selectors each responds to. A binding asked another
// binding type's selector fails as Metal's does: the same failure through Metal's report, then the same answer, and
// with Metal's default error mode the process aborts. A blob that is not one whole gives no reflection.
#include "../TegraGM20BMetalReflection.h"
#include "metal_reflection_blob.h"
#include "reflection_blob_writer.h"
#include "reflection_compare.h"

#import <objc/message.h>
#import <objc/runtime.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <csignal>
#include <iterator>
#include <set>
#include <string>
#include <vector>

// Metal's own, declared by no SDK header: the block its failure reports go to instead of the error mode's action
using ReportFailure = void (^)(uint64_t type, const char *function, int line, NSString *message);
extern "C" void MTLSetReportFailureBlock(ReportFailure block);
extern "C" ReportFailure MTLGetReportFailureBlock(void);

namespace {

constexpr MTLPipelineOption kReflected = MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;

constexpr char kSource[] = R"(
#include <metal_stdlib>
using namespace metal;
struct S { float4 v; float a[4]; };
struct Args { texture2d<float> t; sampler s; device float *p; array<texture2d<float>, 2> ts; };
struct Union { union { struct { float a; float b; }; struct { int c; }; }; };
kernel void k(device float4 *b4 [[buffer(0)]], device S *bs [[buffer(1)]], constant float &cf [[buffer(2)]],
              device uint *bu [[buffer(3)]], device half *unused [[buffer(4)]], device Args &ab [[buffer(5)]],
              texture2d<float> t [[texture(0)]], array<texture2d<float>, 4> ta [[texture(1)]],
              texture2d_array<float> tarr [[texture(5)]], depth2d<float> d [[texture(6)]],
              array<depth2d<float>, 3> dc [[texture(7)]], texture2d<half, access::write> w [[texture(10)]],
              sampler s [[sampler(0)]], array<sampler, 3> sa [[sampler(1)]], array<sampler, 2> sc [[sampler(4)]],
              threadgroup float *tg [[threadgroup(0)]], threadgroup float4 *tg4 [[threadgroup(1)]],
              constant Union &un [[buffer(6)]], uint id [[thread_position_in_grid]]) {
    float x = b4[id].x + bs[id].a[1] + cf + float(bu[id]) + t.sample(s, float2(0)).x + ta[3].sample(sa[2], float2(0)).x
            + tarr.sample(s, float2(0), 1).x + d.sample(sc[1], float2(0)) + dc[2].sample(sc[0], float2(0))
            + ab.p[id] + ab.t.sample(ab.s, float2(0)).x + ab.ts[1].read(uint2(0)).x + un.a + float(un.c);
    tg[id] = x;
    tg4[id] = float4(x);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    w.write(half4(tg4[0]), uint2(id, 0));
    b4[id] = float4(tg[0]);
}
struct V { float4 p [[position]]; float2 uv; };
vertex V vs(device const float4 *pos [[buffer(0)]], constant S &u [[buffer(1)]], texture2d<float> vt [[texture(0)]],
            uint vid [[vertex_id]]) {
    V o; o.p = pos[vid] + u.v + vt.read(uint2(0)); o.uv = float2(0); return o;
}
fragment float4 fs(V in [[stage_in]], texture2d<float> ft [[texture(0)]], array<texture2d<float>, 2> fa [[texture(1)]],
                   sampler fsm [[sampler(0)]], array<sampler, 2> fsa [[sampler(1)]],
                   constant float4 &tint [[buffer(0)]]) {
    return ft.sample(fsm, in.uv) + fa[1].sample(fsa[1], in.uv) * tint;
}
struct TileOut { half4 c [[color(0)]]; };
kernel void tk(imageblock<TileOut, imageblock_layout_implicit> ib, device float4 *tb [[buffer(0)]],
               texture2d<float> tt [[texture(0)]], threadgroup float *ttg [[threadgroup(0)]],
               ushort2 lid [[thread_position_in_threadgroup]]) {
    ttg[0] = tt.read(uint2(0)).x + tb[0].x;
    TileOut o = ib.read(lid); o.c += half4(ttg[0]); ib.write(o, lid);
}
)";

id<MTLLibrary> library(id<MTLDevice> device)
{
    NSError *error = nil;
    id<MTLLibrary> made = [device newLibraryWithSource:@(kSource) options:nil error:&error];
    EXPECT_TRUE(made != nil) << (error ? error.localizedDescription.UTF8String : "");
    return made;
}

id gm20bOf(const std::string &bytes, bool compute)
{
    return gm20bReflectionOf(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), compute);
}

// `metal`'s stages as a render state's blob: vertex, fragment, tile, and no object or mesh stage
std::string renderBlob(MTLRenderPipelineReflection *metal)
{
    metal_reflection::Table table;
    NSArray *stages[] = {metal.vertexBindings, metal.fragmentBindings, metal.tileBindings, @[], @[]};
    for (NSArray *stage : stages)
        table.stage(stage);
    return table.blob();
}

// One ask of a binding with a report-failure block set: each failure reported during it (its type and message, and
// the reporting method where it is not the selector asked), then the answer
struct Asked {
    std::string reports, answer;
};

Asked ask(id binding, const char *selector)
{
    __block std::string reports;
    const std::string asked = std::string(" ") + selector + "]";
    ReportFailure record = ^(uint64_t type, const char *function, int, NSString *message) {
        const std::string by = function ?: "";
        const bool same = by.size() >= asked.size() && !by.compare(by.size() - asked.size(), asked.size(), asked);
        reports += std::to_string(type) + " " + (message.UTF8String ?: "") + (same ? "" : " by " + by) + "; ";
    };
    const ReportFailure before = MTLGetReportFailureBlock();
    MTLSetReportFailureBlock(record);
    const SEL sel = sel_registerName(selector);
    const char returns = method_getTypeEncoding(class_getInstanceMethod([binding class], sel))[0];
    std::string answer;
    if (returns == '@')
        answer = ((id (*)(id, SEL))objc_msgSend)(binding, sel) ? "an object" : "nil";
    else if (returns == 'B' || returns == 'c')
        answer = ((BOOL (*)(id, SEL))objc_msgSend)(binding, sel) ? "YES" : "NO";
    else
        answer = std::to_string((unsigned long long)((NSUInteger (*)(id, SEL))objc_msgSend)(binding, sel));
    MTLSetReportFailureBlock(before);
    return {reports, answer};
}

// Each binding of `metal` and the same of `raw` asked every reflection selector: where Metal responds and where it
// reports a failure, raw does the same and then answers what Metal does. Adds each binding's type to `types` and each
// failure Metal reported to `failures`; returns the asks raw answers otherwise
std::vector<std::string> askedAsMetal(NSArray *metal, NSArray *raw, std::set<NSUInteger> &types, unsigned &failures)
{
    std::vector<std::string> off;
    if (metal.count != raw.count)
        return {"bindings: metal " + std::to_string(metal.count) + ", raw " + std::to_string(raw.count)};
    for (NSUInteger i = 0; i < metal.count; i++) {
        types.insert(((NSUInteger (*)(id, SEL))objc_msgSend)(metal[i], @selector(type)));
        for (const std::string &s : reflection_compare::everySelector()) {
            const std::string at = std::string([[metal[i] name] UTF8String]) + "." + s;
            const BOOL responds = [metal[i] respondsToSelector:sel_registerName(s.c_str())];
            if (responds != [raw[i] respondsToSelector:sel_registerName(s.c_str())]) {
                off.push_back(at + ": responds metal " + (responds ? "YES" : "NO"));
                continue;
            }
            if (!responds)
                continue;
            const Asked m = ask(metal[i], s.c_str()), r = ask(raw[i], s.c_str());
            failures += !m.reports.empty();
            if (m.reports != r.reports)
                off.push_back(at + ": reported metal {" + m.reports + "} raw {" + r.reports + "}");
            else if (!m.reports.empty() && m.answer != r.answer)
                off.push_back(at + ": answered after the report metal " + m.answer + ", raw " + r.answer);
        }
    }
    return off;
}

// m2v's table of a buffer binding whose struct holds a float4 and an array of depth textures, through a pointer too; a
// texture binding; a threadgroup binding (reflection_blob_writer.h of)
struct Table {
    std::vector<NmbReflectionNode> nodes;
    std::vector<uint32_t> edges = {3, 4, 7, 8, 9};

    Table()
    {
        const auto node = [](uint32_t kind, const char *name, uint32_t flags, std::initializer_list<uint64_t> values,
                             std::initializer_list<uint32_t> children, uint32_t first = 0, uint32_t count = 0) {
            NmbReflectionNode n = {};
            n.kind = kind;
            n.name = name;
            n.flags = flags;
            std::copy(values.begin(), values.end(), n.values);
            std::fill(std::begin(n.children), std::end(n.children), UINT32_MAX);
            std::copy(children.begin(), children.end(), n.children);
            n.first = first;
            n.count = count;
            return n;
        };
        const uint32_t none = UINT32_MAX;
        nodes = {
            node(0, nullptr, 0, {}, {}, 2, 3),
            node(6, nullptr, 1, {MTLDataTypeFloat, MTLTextureType2D, MTLBindingAccessReadOnly}, {}),
            node(4, nullptr, 0, {MTLDataTypeTexture, 4, 8, 1}, {none, none, 1}),
            node(3, "colour", 0, {MTLDataTypeFloat4, 0, 0}, {}),
            node(3, "depths", 0, {MTLDataTypeArray, 16, 1}, {none, 2}),
            node(2, nullptr, 0, {}, {}, 0, 2),
            node(5, nullptr, 1, {MTLDataTypeStruct, MTLBindingAccessReadWrite, 16, 48}, {5}),
            node(1, "uniforms", 3, {MTLBindingTypeBuffer, MTLBindingAccessReadOnly, 3, 0, 0, 16, 48, MTLDataTypeStruct},
                 {5, none, none, 6}),
            node(1, "shadow", 5,
                 {MTLBindingTypeTexture, MTLBindingAccessReadOnly, 1, 0, 0, MTLTextureType2D, MTLDataTypeFloat, 1}, {}),
            node(1, "scratch", 0, {MTLBindingTypeThreadgroupMemory, MTLBindingAccessReadWrite, 0, 0, 0, 16, 256}, {}),
        };
    }
};

}  // namespace

TEST(GM20BReflection, AComputeStatesBindingsOfEveryTypeAnswerAsMetals)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_TRUE(device != nil);
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = [library(device) newFunctionWithName:@"k"];
    MTLComputePipelineReflection *metal = nil;
    NSError *error = nil;
    id<MTLComputePipelineState> state =
        [device newComputePipelineStateWithDescriptor:d options:kReflected reflection:&metal error:&error];
    ASSERT_TRUE(state != nil) << (error ? error.localizedDescription.UTF8String : "");
    metal_reflection::Table table;
    table.stage(metal.bindings);
    id raw = gm20bOf(table.blob(), true);
    ASSERT_TRUE(raw != nil);
    EXPECT_TRUE([raw isKindOfClass:[MTLComputePipelineReflection class]]);
    reflection_compare::expectSameList(metal.bindings, [raw bindings], "bindings");
    reflection_compare::expectSameList(metal.arguments, [raw arguments], "arguments");
    EXPECT_EQ(gm20bOf(table.blob(), false), nil) << "a compute blob read as a render one";
}

TEST(GM20BReflection, ARenderStatesAndATileStatesStagesAnswerAsMetals)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = library(device);
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [functions newFunctionWithName:@"vs"];
    d.fragmentFunction = [functions newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    MTLRenderPipelineReflection *metal = nil;
    NSError *error = nil;
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:d options:kReflected reflection:&metal error:&error];
    ASSERT_TRUE(state != nil) << (error ? error.localizedDescription.UTF8String : "");
    id raw = gm20bOf(renderBlob(metal), false);
    ASSERT_TRUE(raw != nil);
    EXPECT_TRUE([raw isKindOfClass:[MTLRenderPipelineReflection class]]);
    reflection_compare::expectSameRender(metal, raw, "render");
    for (NSString *empty : @[ @"objectBindings", @"meshBindings" ])
        EXPECT_EQ([((NSArray * (*)(id, SEL)) objc_msgSend)(raw, NSSelectorFromString(empty)) count], 0u)
            << empty.UTF8String;
    EXPECT_EQ(gm20bOf(renderBlob(metal), true), nil) << "a render blob read as a compute one";

    MTLTileRenderPipelineDescriptor *t = [MTLTileRenderPipelineDescriptor new];
    t.tileFunction = [functions newFunctionWithName:@"tk"];
    t.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    t.threadgroupSizeMatchesTileSize = YES;
    MTLRenderPipelineReflection *tile = nil;
    id<MTLRenderPipelineState> tileState =
        [device newRenderPipelineStateWithTileDescriptor:t options:kReflected reflection:&tile error:&error];
    ASSERT_TRUE(tileState != nil) << (error ? error.localizedDescription.UTF8String : "");
    reflection_compare::expectSameRender(tile, gm20bOf(renderBlob(tile), false), "tile");
}

TEST(GM20BReflection, ABlobThatIsNotOneWholeGivesNone)
{
    const Table table;
    const std::string whole = reflection_blob::of(table.nodes, table.edges, gm20b::kReflectionComputeStages);
    ASSERT_NE(gm20bOf(whole, true), nil);
    EXPECT_EQ(gm20bOf(whole.substr(0, whole.size() - 1), true), nil);
    std::string version = whole;
    version[4] = 2;
    EXPECT_EQ(gm20bOf(version, true), nil);
    Table cycle;
    cycle.nodes[2].children[2] = 7;  // the array names a node after it
    EXPECT_EQ(gm20bOf(reflection_blob::of(cycle.nodes, cycle.edges, 1), true), nil);
    Table kinds;
    kinds.nodes[4].children[1] = 1;  // a member's array is a texture reference
    EXPECT_EQ(gm20bOf(reflection_blob::of(kinds.nodes, kinds.edges, 1), true), nil);
}

TEST(GM20BReflection, ABindingOfEveryTypeAskedAnotherTypesSelectorFailsAsMetals)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = library(device);
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = [functions newFunctionWithName:@"k"];
    MTLComputePipelineReflection *compute = nil;
    NSError *error = nil;
    id<MTLComputePipelineState> state =
        [device newComputePipelineStateWithDescriptor:d options:kReflected reflection:&compute error:&error];
    ASSERT_TRUE(state != nil) << (error ? error.localizedDescription.UTF8String : "");
    MTLTileRenderPipelineDescriptor *t = [MTLTileRenderPipelineDescriptor new];
    t.tileFunction = [functions newFunctionWithName:@"tk"];
    t.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    t.threadgroupSizeMatchesTileSize = YES;
    MTLRenderPipelineReflection *tile = nil;
    id<MTLRenderPipelineState> tileState =
        [device newRenderPipelineStateWithTileDescriptor:t options:kReflected reflection:&tile error:&error];
    ASSERT_TRUE(tileState != nil) << (error ? error.localizedDescription.UTF8String : "");
    metal_reflection::Table table;
    table.stage(compute.bindings);

    std::set<NSUInteger> types;
    unsigned failures = 0;
    std::vector<std::string> off =
        askedAsMetal(compute.bindings, [gm20bOf(table.blob(), true) bindings], types, failures);
    const std::vector<std::string> tileOff =
        askedAsMetal(tile.tileBindings, [gm20bOf(renderBlob(tile), false) tileBindings], types, failures);
    off.insert(off.end(), tileOff.begin(), tileOff.end());
    EXPECT_EQ(types, (std::set<NSUInteger>{MTLBindingTypeBuffer, MTLBindingTypeThreadgroupMemory, MTLBindingTypeTexture,
                                           MTLBindingTypeSampler, MTLBindingTypeImageblock}));
    EXPECT_GT(failures, 0u) << "Metal reported no failure: its error mode ignores them";
    std::string list;
    for (const std::string &o : off)
        list += o + "\n";
    EXPECT_TRUE(off.empty()) << off.size() << " asks answered otherwise than Metal's\n" << list;
}

TEST(GM20BReflection, ABindingAskedAnotherTypesSelectorAbortsAsMetalDoes)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const Table table;
    id reflection = gm20bOf(reflection_blob::of(table.nodes, table.edges, gm20b::kReflectionComputeStages), true);
    ASSERT_NE(reflection, nil);
    id shadow = [reflection bindings][1];
    ASSERT_EQ(((NSUInteger (*)(id, SEL))objc_msgSend)(shadow, @selector(type)), NSUInteger(MTLBindingTypeTexture));
    EXPECT_EXIT(((NSUInteger (*)(id, SEL))objc_msgSend)(shadow, @selector(bufferAlignment)),
                ::testing::KilledBySignal(SIGABRT),
                "failed assertion `Querying buffer alignment on an argument that is not a buffer'");
}
