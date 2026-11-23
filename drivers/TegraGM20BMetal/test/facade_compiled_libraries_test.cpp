// SPDX-License-Identifier: MIT
// Libraries the system's Metal front end makes, through the system shader compiler and never m2v
// (TegraGM20BMetalCompiledLibraries.mm): Metal source, a stitched library, MPS's kernel DAG and Core Image's
// filter-function link.
// The kernel model's compiler keeps the code generation request each one sends and answers it with a metallib in the
// store; the library is then the device's own, its functions raw's.
#include "facade_compiler_store.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "ImageFilterLink.h"
#include "bazel/testing/fresh_process.h"

#include <dispatch/dispatch.h>
#include <gtest/gtest.h>

@protocol HarnessDagWriter
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs;
@end

@protocol HarnessFunctionConstants
- (NSArray<MTLFunctionConstant *> *)functionConstants;
@end

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

void consume(LibraryCompiler &compiler, id<MTLDevice> device, id<MTLFunction> function, NSArray *linked = nil,
             bool stageInput = false)
{
    ASSERT_TRUE(function != nil);
    MTLComputePipelineDescriptor *descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = function;
    if (linked.count) {
        descriptor.linkedFunctions = [MTLLinkedFunctions linkedFunctions];
        descriptor.linkedFunctions.privateFunctions = linked;
    }
    if (stageInput) {
        descriptor.stageInputDescriptor = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        descriptor.stageInputDescriptor.attributes[0].format = MTLAttributeFormatFloat2;
        descriptor.stageInputDescriptor.attributes[0].bufferIndex = 1;
        descriptor.stageInputDescriptor.layouts[1].stride = 8;
    }
    const size_t first = compiler.consumers.size(), jobs = kernel_model::model.jobs.size();
    NSError *error = nil;
    MTLAutoreleasedComputePipelineReflection reflection = nil;
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithDescriptor:descriptor
                                                                              options:MTLPipelineOptionBufferTypeInfo
                                                                           reflection:&reflection
                                                                                error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    ASSERT_TRUE(reflection != nil) << text(error);
    EXPECT_EQ(state.threadExecutionWidth, 32u);
    EXPECT_EQ(reflection.arguments.count, 0u);
    ASSERT_EQ(compiler.consumers.size(), first + 2);
    const auto &facts = compiler.consumers[first], &reflected = compiler.consumers[first + 1];
    EXPECT_EQ(facts.kind, gm20b::RequestKind::Facts);
    EXPECT_EQ(reflected.kind, gm20b::RequestKind::Reflection);
    EXPECT_EQ(reflected.key, "reflection | " + facts.key.substr(strlen("facts | ")));
    const std::string layout = gm20b::stageInText(gm20b::stageInFieldsOf(descriptor.stageInputDescriptor));
    if (stageInput)
        ASSERT_FALSE(layout.empty());
    EXPECT_EQ(gm20b::stageInText(facts.kernel.stageIn), layout);
    EXPECT_EQ(gm20b::stageInText(reflected.kernel.stageIn), layout);
    ASSERT_EQ(reflected.stages.size(), 1 + linked.count);
    for (NSUInteger i = 0; i < linked.count; i++) {
        EXPECT_EQ(reflected.stages[i + 1].tag, "kp");
        EXPECT_EQ(reflected.stages[i + 1].function, [linked[i] name].UTF8String);
    }
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:state];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << text(commands.error);
    ASSERT_EQ(compiler.consumers.size(), first + 3);
    EXPECT_EQ(compiler.consumers.back().kind, gm20b::RequestKind::Kernel);
    EXPECT_EQ(gm20b::stageInText(compiler.consumers.back().kernel.stageIn), layout);
    EXPECT_TRUE(computeLaunched(jobs));
}

id<MTLFunction> consumerKernel(id<MTLDevice> device)
{
    const std::string bytes = made_metallib::thin({"consumer"}, {2}, {made_metallib::wrappedBitcode(0x91)});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    return [[device newLibraryWithData:data error:nil] newFunctionWithName:@"consumer"];
}

const std::string kAdd = made_metallib::wrappedBitcode(0x41), kMul = made_metallib::wrappedBitcode(0x42, 96);

// A library the device read of data, as Core Image's stitchable functions are: "add" and "mul", visible functions whose
// bitcode the metallib places
id<MTLLibrary> stitchable(id<MTLDevice> device)
{
    const std::string bytes = made_metallib::thin({"add", "mul"}, {4, 4}, {kAdd, kMul});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    return [device newLibraryWithData:data error:nil];
}

MTLStitchedLibraryDescriptor *stitching(id<MTLLibrary> library)
{
    MTLFunctionStitchingInputNode *a = [[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:0];
    MTLFunctionStitchingInputNode *b = [[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:1];
    MTLFunctionStitchingFunctionNode *add =
        [[MTLFunctionStitchingFunctionNode alloc] initWithName:@"add" arguments:@[ a, b ] controlDependencies:@[]];
    MTLFunctionStitchingFunctionNode *mul =
        [[MTLFunctionStitchingFunctionNode alloc] initWithName:@"mul" arguments:@[ add, b ] controlDependencies:@[]];
    MTLStitchedLibraryDescriptor *d = [MTLStitchedLibraryDescriptor new];
    d.functionGraphs = @[ [[MTLFunctionStitchingGraph alloc] initWithFunctionName:@"stitched"
                                                                            nodes:@[ add ]
                                                                       outputNode:mul
                                                                       attributes:@[]] ];
    d.functions = @[ [library newFunctionWithName:@"add"], [library newFunctionWithName:@"mul"] ];
    return d;
}

// A library from source is the front end's metallib, read by the device: its request is the source and the options
// Metal passes (no module cache: the compiler's own), asked once for every library of the same source
void sourceLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib = made_metallib::thin({"square"}, {2}, {made_metallib::wrappedBitcode(0x51)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    NSString *source = @"kernel void square(device uint *d [[buffer(0)]], uint i [[thread_position_in_grid]]) {}";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_EQ(error, nil);
    EXPECT_TRUE([library.functionNames isEqualToArray:@[ @"square" ]]) << text(library.functionNames);
    id<MTLFunction> square = [library newFunctionWithName:@"square"];
    ASSERT_TRUE(square != nil);
    EXPECT_EQ(square.functionType, MTLFunctionTypeKernel);
    ASSERT_EQ(compiler.asked.size(), 1u);
    const gm20b::CompileRequest &asked = compiler.asked[0];
    EXPECT_EQ(asked.codeGenType, gm20b::kCodeGenSource);
    std::string_view sent, options;
    ASSERT_TRUE(gm20b::codegen::readSource(asked.codeGen, sent, options));
    EXPECT_EQ(sent, source.UTF8String);
    EXPECT_EQ(std::string(options).rfind("--driver-mode=metal -x metal -std=metal", 0), 0u) << options;
    EXPECT_EQ(options.find("-fmodules-cache-path"), std::string_view::npos) << options;

    EXPECT_TRUE([device newLibraryWithSource:source options:nil error:&error] != nil) << text(error);
    EXPECT_EQ(compiler.asked.size(), 1u) << "the second library of the source is the store's";

    MTLCompileOptions *options2 = [MTLCompileOptions new];
    options2.preprocessorMacros = @{@"TILE" : @16, @"NAME" : @"a b"};
    options2.fastMathEnabled = NO;
    compiler.made.warnings = "program_source:1:1: warning: unused variable";
    error = [NSError errorWithDomain:MTLLibraryErrorDomain code:MTLLibraryErrorInternal userInfo:nil];
    id<MTLLibrary> warned = [device newLibraryWithSource:source options:options2 error:&error];
    EXPECT_TRUE(warned != nil);
    EXPECT_TRUE(error == nil) << "a library made with warnings has no error, as Metal's: " << text(error);
    ASSERT_EQ(compiler.asked.size(), 2u);
    ASSERT_TRUE(gm20b::codegen::readSource(compiler.asked[1].codeGen, sent, options));
    EXPECT_NE(options.find("-fno-fast-math"), std::string_view::npos) << options;
    EXPECT_NE(options.find("\"-DNAME=a b\""), std::string_view::npos) << options;
    consume(compiler, device, square);
    consume(compiler, device, square, nil, true);
}

// Source the front end refuses is nil with Metal's compile failure and the front end's log, asked again at every
// library: a refusal never stands in the store
void refusedSource()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.log = "program_source:1:13: error: expected ')'";
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    for (unsigned asks = 1; asks <= 2; asks++) {
        NSError *error = nil;
        EXPECT_EQ([device newLibraryWithSource:@"kernel void k( {}" options:nil error:&error], nil);
        EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorCompileFailure)) << text(error);
        EXPECT_EQ(text(error.localizedDescription), compiler.log);
        EXPECT_EQ(compiler.asked.size(), asks);
    }
    NSError *error = nil;
    MTLCompileOptions *dynamic = [MTLCompileOptions new];
    dynamic.libraryType = MTLLibraryTypeDynamic;
    EXPECT_EQ([device newLibraryWithSource:@"kernel void k() {}" options:dynamic error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorUnsupported));
    EXPECT_EQ(compiler.asked.size(), 2u);
}

// With no compiler this boot a library from source or stitching fails as Metal's does without its compiler
void noCompiler()
{
    kernel_model::model.compilerFlags = TegraGM20BCompilerCapable | TegraGM20BCompilerDeclined;
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    EXPECT_EQ([device newLibraryWithSource:@"kernel void k() {}" options:nil error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorInternal));
    EXPECT_NE(text(error.localizedDescription).find("no shader compiler comes this boot"), std::string::npos)
        << text(error);
    error = nil;
    EXPECT_EQ([device newLibraryWithStitchedDescriptor:stitching(stitchable(device)) error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorInternal)) << text(error);
    EXPECT_EQ(kernel_model::model.compiles, 0u);
}

// A stitched library's request is the base device's DAG text and each function's bitcode from the metallib the raw
// device read it of, as the builder writes them; the stitched function is raw's own. Each form Metal and Core Image
// reach it by takes the same path
void stitchedLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib = made_metallib::thin({"stitched"}, {4}, {made_metallib::wrappedBitcode(0x61)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = stitchable(device);
    ASSERT_TRUE(functions != nil);
    MTLStitchedLibraryDescriptor *d = stitching(functions);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithStitchedDescriptor:d error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_TRUE([library.functionNames isEqualToArray:@[ @"stitched" ]]) << text(library.functionNames);
    id<MTLFunction> stitched = [library newFunctionWithName:@"stitched"];
    ASSERT_TRUE(stitched != nil);
    EXPECT_EQ(stitched.functionType, MTLFunctionTypeVisible);

    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0].codeGenType, gm20b::kCodeGenStitch);
    NSString *dag = [(id<HarnessDagWriter>)device newDagStringWithGraphs:d.functionGraphs];
    const gm20b::codegen::Bitcode add = {reinterpret_cast<const uint8_t *>(kAdd.data()), kAdd.size()},
                                  mul = {reinterpret_cast<const uint8_t *>(kMul.data()), kMul.size()};
    EXPECT_EQ(compiler.asked[0].codeGen, gm20b::codegen::stitchRequest(dag.UTF8String, {add, mul}));

    for (const char *form : {"newLibraryWithDescriptorSPI:error:", "newLibraryWithStitchedDescriptorSPI:error:"})
        EXPECT_TRUE(((id (*)(id, SEL, id, NSError **))objc_msgSend)(device, sel_registerName(form), d, &error) != nil)
            << form << ": " << text(error);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block id<MTLLibrary> later = nil;
    [device newLibraryWithStitchedDescriptor:d
                           completionHandler:^(id<MTLLibrary> made, NSError *) {
                               later = made;
                               dispatch_semaphore_signal(done);
                           }];
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)), 0);
    EXPECT_TRUE(later != nil);
    EXPECT_EQ(compiler.asked.size(), 1u) << "every form after the first is the store's";

    MTLStitchedLibraryDescriptor *foreign = [d copy];
    const std::string placeless = made_metallib::universal({"add", "mul"}, 0xA0, 0xEE, {4, 4});
    dispatch_data_t data =
        dispatch_data_create(placeless.data(), placeless.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    id<MTLLibrary> unplaced = [device newLibraryWithData:data error:nil];
    foreign.functions = @[ [unplaced newFunctionWithName:@"add"], [unplaced newFunctionWithName:@"mul"] ];
    EXPECT_EQ([device newLibraryWithStitchedDescriptor:foreign error:&error], nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorFunctionNotFound)) << text(error);
    EXPECT_EQ(compiler.asked.size(), 1u);
    consume(compiler, device, consumerKernel(device), @[ stitched ]);
}

// MPSKernelDAG's library (MPSGraph's control flow and fused kernels, which abort on nil): its DAG text as MPS writes it
// (one graph per function it makes, ALWAYS_INLINE, nodes over INPUTs and earlier NODEs) and the visible functions its
// nodes name, in MPS's order, which is not the DAG's. The request is that text and those functions' bitcode in that
// order; the library's functions are the DAG's, raw's own
void kernelDagLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib =
        made_metallib::thin({"prefixPrimary_i8", "postfixPrimary_i8"}, {4, 4},
                            {made_metallib::wrappedBitcode(0x81), made_metallib::wrappedBitcode(0x82)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = stitchable(device);
    ASSERT_TRUE(functions != nil);
    NSString *dag = @"DAGS {\n"
                    @"prefixPrimary_i8 {\n"
                    @"ALWAYS_INLINE;\n"
                    @"NODE(Node_0) : FUNCTION(add), INPUT(0), INPUT(1);\n"
                    @"OUTPUT(Node_value) : FUNCTION(mul), NODE(Node_0), INPUT(1);\n"
                    @"}\n"
                    @"postfixPrimary_i8 {\n"
                    @"ALWAYS_INLINE;\n"
                    @"NODE(Node_0) : FUNCTION(mul), INPUT(0), INPUT(1), INPUT(2);\n"
                    @"NODE(add) : FUNCTION(add), INPUT(0), NODE(Node_0);\n"
                    @"}\n"
                    @"}\n";
    NSArray<id<MTLFunction>> *named =
        @[ [functions newFunctionWithName:@"mul"], [functions newFunctionWithName:@"add"] ];
    NSError *error = nil;
    id<MTLLibrary> (*withDag)(id, SEL, NSString *, NSArray *, NSError **) =
        (id<MTLLibrary> (*)(id, SEL, NSString *, NSArray *, NSError **))objc_msgSend;
    const SEL selector = sel_registerName("newLibraryWithDAG:functions:error:");
    id<MTLLibrary> library = withDag(device, selector, dag, named, &error);
    ASSERT_TRUE(library != nil) << text(error);
    NSArray<NSString *> *names = [library.functionNames sortedArrayUsingSelector:@selector(compare:)];
    NSArray<NSString *> *dagFunctions = @[ @"postfixPrimary_i8", @"prefixPrimary_i8" ];
    EXPECT_TRUE([names isEqualToArray:dagFunctions]) << text(names);
    id<MTLFunction> prefix = [library newFunctionWithName:@"prefixPrimary_i8"];
    ASSERT_TRUE(prefix != nil);
    EXPECT_EQ(prefix.functionType, MTLFunctionTypeVisible);
    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0].codeGenType, gm20b::kCodeGenStitch);
    const gm20b::codegen::Bitcode add = {reinterpret_cast<const uint8_t *>(kAdd.data()), kAdd.size()},
                                  mul = {reinterpret_cast<const uint8_t *>(kMul.data()), kMul.size()};
    EXPECT_EQ(compiler.asked[0].codeGen, gm20b::codegen::stitchRequest(dag.UTF8String, {mul, add}));

    EXPECT_EQ(withDag(device, selector, @"", named, &error), nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorCompileFailure)) << text(error);
    EXPECT_EQ(withDag(device, selector, dag, @[], &error), nil);
    EXPECT_EQ(error.code, NSInteger(MTLLibraryErrorCompileFailure)) << text(error);
    EXPECT_EQ(compiler.asked.size(), 1u);
    consume(compiler, device, consumerKernel(device), @[ prefix, [library newFunctionWithName:@"postfixPrimary_i8"] ]);
}

// MPS makes its DAG's functions with constant values and a specialized name (MPSLibrary's function constructor), and
// Metal's own function holds the front end's specialization of the AIR, which is what gets stitched. The device asks the
// front end for that first: the source's AIR and HASH, its values as Metal serializes them against its functionConstants
// and the specialized name. The stitch then carries the specialization's bitcode. Other values are another library; the
// same values again are the store's
void specializedDagLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    const std::vector<uint8_t> required = {1, 0, 'g', 'a', 'i', 'n', 0, uint8_t(MTLDataTypeUInt), 3, 0, 1};
    std::vector<uint8_t> tags;
    made_metallib::tag(tags, "CNST", required.data(), uint16_t(required.size()));
    const std::string bytes = made_metallib::thin({"add"}, {4}, {kAdd}, {tags});
    dispatch_data_t data = dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> functions = [device newLibraryWithData:data error:&error];
    ASSERT_TRUE(functions != nil) << text(error);
    NSArray<MTLFunctionConstant *> *listed =
        [(id<HarnessFunctionConstants>)[functions newFunctionWithName:@"add"] functionConstants];
    ASSERT_EQ(listed.count, 1u);
    EXPECT_EQ(text(listed[0].name), "gain");
    EXPECT_EQ(listed[0].index, 3u);
    EXPECT_EQ(listed[0].type, MTLDataTypeUInt);
    EXPECT_TRUE(listed[0].required);

    gm20b::Digest hash;
    hash.fill(0xB0);
    const gm20b::codegen::Bitcode source = {reinterpret_cast<const uint8_t *>(kAdd.data()), kAdd.size()};
    const SEL selector = sel_registerName("newLibraryWithDAG:functions:error:");

    struct Ask {
        uint32_t gain;
        std::string specializedName;
        bool asks;
    };

    for (const Ask &ask : {Ask{1, "", true}, Ask{2, "add_g2", true}, Ask{1, "", false}}) {
        const std::string called = ask.specializedName.empty() ? "add" : ask.specializedName;
        const std::string specializedAir = made_metallib::wrappedBitcode(uint8_t(0x60 + ask.gain));
        compiler.made.metallib =
            made_metallib::thin({called, "stitched"}, {4, 4}, {specializedAir, made_metallib::wrappedBitcode(0x7f)});
        MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
        [values setConstantValue:&ask.gain type:MTLDataTypeUInt atIndex:3];
        MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
        d.name = @"add";
        d.specializedName = ask.specializedName.empty() ? nil : @(ask.specializedName.c_str());
        d.constantValues = values;
        id<MTLFunction> function = [functions newFunctionWithDescriptor:d error:&error];
        ASSERT_TRUE(function != nil) << text(error);
        NSString *dag =
            [NSString stringWithFormat:@"DAGS {\nstitched {\nOUTPUT(value) : FUNCTION(%s), INPUT(0), INPUT(1);\n}\n}\n",
                                       called.c_str()];
        const size_t before = compiler.asked.size();
        id<MTLLibrary> library = ((id<MTLLibrary> (*)(id, SEL, NSString *, NSArray *, NSError **))objc_msgSend)(
            device, selector, dag, @[ function ], &error);
        ASSERT_TRUE(library != nil) << text(error);
        EXPECT_EQ([library newFunctionWithName:@"stitched"].functionType, MTLFunctionTypeVisible);
        if (!ask.asks) {
            EXPECT_EQ(compiler.asked.size(), before) << "the same values again are the store's";
            continue;
        }
        ASSERT_EQ(compiler.asked.size(), before + 2);
        std::string constants;
        gm20b::codegen::word(constants, 1);
        const uint16_t index = 3;
        constants.append(reinterpret_cast<const char *>(&index), sizeof index);
        constants.push_back(char(MTLDataTypeUInt));
        gm20b::codegen::word(constants, ask.gain);
        EXPECT_EQ(compiler.asked[before].codeGenType, gm20b::kCodeGenSpecialize);
        EXPECT_EQ(compiler.asked[before].codeGen,
                  gm20b::codegen::specializationRequest("add", ask.specializedName, source, hash, constants));
        EXPECT_EQ(compiler.asked[before + 1].codeGenType, gm20b::kCodeGenStitch);
        const gm20b::codegen::Bitcode specialized = {reinterpret_cast<const uint8_t *>(specializedAir.data()),
                                                     specializedAir.size()};
        EXPECT_EQ(compiler.asked[before + 1].codeGen, gm20b::codegen::stitchRequest(dag.UTF8String, {specialized}));
    }
}

// Core Image's filter-function link is ImageFilterLink.h's request of the functions' bitcode and infos
void filterLibrary()
{
    LibraryCompiler compiler;
    ASSERT_TRUE(compiler.serve());
    compiler.made.metallib = made_metallib::thin({"ciKernelMain"}, {2}, {made_metallib::wrappedBitcode(0x71)});
    id<MTLDevice> device = completing();
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> functions = stitchable(device);
    const switch_metal::ImageFilterArgument argument = {3, {1, 2}};
    const switch_metal::ImageFilterFunctionInfo infos[2] = {{true, 1, &argument}, {false, 0, nullptr}};
    NSError *error = nil;
    id<MTLLibrary> library = ((id (*)(id, SEL, NSArray *, const void *, NSError **))objc_msgSend)(
        device, sel_registerName("newLibraryWithImageFilterFunctionsSPI:imageFilterFunctionInfo:error:"),
        @[ [functions newFunctionWithName:@"add"], [functions newFunctionWithName:@"mul"] ], infos, &error);
    ASSERT_TRUE(library != nil) << text(error);
    EXPECT_TRUE([library.functionNames isEqualToArray:@[ @"ciKernelMain" ]]) << text(library.functionNames);
    ASSERT_EQ(compiler.asked.size(), 1u);
    EXPECT_EQ(compiler.asked[0].codeGenType, gm20b::kCodeGenImageFilters);
    std::vector<switch_metal::ImageFilterFunction> linked(2);
    linked[0] = {"add", reinterpret_cast<const uint8_t *>(kAdd.data()), kAdd.size(), true, {argument}};
    linked[1] = {"mul", reinterpret_cast<const uint8_t *>(kMul.data()), kMul.size(), false, {}};
    const std::vector<uint8_t> request = switch_metal::imageFilterLinkRequest(linked);
    EXPECT_EQ(compiler.asked[0].codeGen, std::string(request.begin(), request.end()));
    consume(compiler, device, [library newFunctionWithName:@"ciKernelMain"]);
}

}  // namespace

TEST(GM20BFacadeCompiledLibraries, ALibraryFromSourceIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(sourceLibrary);
}

TEST(GM20BFacadeCompiledLibraries, SourceTheFrontEndRefusesFailsAsMetalsCompileFailure)
{
    switch_ios::test::ExpectPassesInFreshProcess(refusedSource);
}

TEST(GM20BFacadeCompiledLibraries, WithNoCompilerALibraryFailsItsCreation)
{
    switch_ios::test::ExpectPassesInFreshProcess(noCompiler);
}

TEST(GM20BFacadeCompiledLibraries, AStitchedLibraryIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(stitchedLibrary);
}

TEST(GM20BFacadeCompiledLibraries, AnMpsKernelDagLibraryIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(kernelDagLibrary);
}

TEST(GM20BFacadeCompiledLibraries, ASpecializedDagFunctionIsStitchedAsTheFrontEndsSpecialization)
{
    switch_ios::test::ExpectPassesInFreshProcess(specializedDagLibrary);
}

TEST(GM20BFacadeCompiledLibraries, ACoreImageFilterLibraryIsTheShaderCompilers)
{
    switch_ios::test::ExpectPassesInFreshProcess(filterLibrary);
}
