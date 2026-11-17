// SPDX-License-Identifier: MIT
// Libraries the system's Metal front end makes at run time: Metal source, stitched functions and Core Image's
// filter-function link. The device writes the front end's request from its own functions' metallibs
// (raw_codegen_request.h), the system shader compiler answers it with a metallib (compiler/compiler_codegen.mm), and the
// library is read as any other (readLibrary), so its functions are this device's own.
#import "RawMetalInternal.h"

#include <os/log.h>
#include "ImageFilterLink.h"
#include "raw_codegen_request.h"
#include "raw_library_entry.h"

// _MTLDevice's own writer of stitching graphs as the DAG text Metal's front end reads (the device class's base)
@protocol RMDagWriter
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs;
@end

static NSError *libraryError(MTLLibraryError code, const std::string &why)
{
    NSString *text = [[NSString alloc] initWithBytes:why.data() length:why.size() encoding:NSUTF8StringEncoding];
    return [NSError errorWithDomain:MTLLibraryErrorDomain
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey : text ?: @"the front end's log is not UTF-8"}];
}

static id<MTLLibrary> failed(MTLLibraryError code, const std::string &why, NSError **error)
{
    if (error)
        *error = libraryError(code, why);
    return nil;
}

// The front end's arguments for `options`, as Metal passes them (nil: Metal's default options): the language standard,
// fast math, invariance, size optimisation and the preprocessor macros. The shader compiler adds its own module cache.
static std::string compileArguments(MTLCompileOptions *options)
{
    MTLCompileOptions *o = options ?: [MTLCompileOptions new];
    const NSUInteger version = NSUInteger(o.languageVersion);
    char standard[48];
    snprintf(standard, sizeof standard, version >> 16 >= 3 ? "-std=metal%u.%u" : "-std=ios-metal%u.%u",
             unsigned(version >> 16), unsigned(version & 0xffff));
    __block std::string arguments = std::string("--driver-mode=metal -x metal ") + standard;
    arguments += o.fastMathEnabled ? " -ffast-math" : " -fno-fast-math";
    if (o.preserveInvariance)
        arguments += " -fpreserve-invariance";
    if (o.optimizationLevel == MTLLibraryOptimizationLevelSize)
        arguments += " -Os";
    [o.preprocessorMacros enumerateKeysAndObjectsUsingBlock:^(NSString *name, NSObject *value, BOOL *) {
        arguments +=
            " " + raw::codegen::quotedArgument([NSString stringWithFormat:@"-D%@=%@", name, value].UTF8String ?: "");
    }];
    return arguments;
}

// `function`'s wrapped bitcode in the metallib its library read; false for a function of another device
static bool bitcodeOf(id function, raw::codegen::Bitcode &out)
{
    if (![function isKindOfClass:[RMFunction class]])
        return false;
    RMFunction *f = function;
    return f->_source && f->_source->bytes &&
           raw::codegen::functionBitcode(f->_source->bytes, f->_source->size, libraryName(f), out);
}

static id<MTLLibrary> compiled(id device, uint32_t type, const std::string &request, const char *what, NSError **error)
{
#if SWITCH_RAW_DIAGNOSTICS
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    raw::MadeLibrary made;
    std::string failure = "no shader compiler comes this boot";
    bool refused = false;
    if (!gRM->completion || !gRM->completion->library(type, request, made, failure, refused)) {
        os_log_error(rawLog(), "raw: %{public}s: %{public}s not made: %{public}.600s", getprogname(), what,
                     failure.c_str());
        return failed(refused ? MTLLibraryErrorCompileFailure : MTLLibraryErrorInternal, failure, error);
    }
    auto *metallib = new std::string(std::move(made.metallib));
    NSData *bytes = [[NSData alloc] initWithBytesNoCopy:metallib->data()
                                                 length:metallib->size()
                                            deallocator:^(void *, NSUInteger) {
                                                delete metallib;
                                            }];
    RMLibrary *library = readLibrary(device, nil, bytes);
    if (!library)
        return failed(MTLLibraryErrorInternal,
                      std::string("the shader compiler's ") + what + " is no metallib Metal loads", error);
#if SWITCH_RAW_DIAGNOSTICS
    os_log(rawLog(), "raw: %{public}s: %{public}s made by the shader compiler in %llu ms", getprogname(), what,
           (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000));
#endif
    if (error)
        *error = made.warnings.empty() ? nil : libraryError(MTLLibraryErrorCompileWarning, made.warnings);
    return (id<MTLLibrary>)library;
}

@interface RMDevice (CompiledLibraries)
@end
@implementation RMDevice (CompiledLibraries)

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    if (!source)
        return failed(MTLLibraryErrorCompileFailure, "no source", error);
    if (options.libraryType == MTLLibraryTypeDynamic)
        return failed(MTLLibraryErrorUnsupported, "dynamic libraries are not supported", error);
    const char *text = source.UTF8String ?: "";
    return compiled(self, raw::kCodeGenSource, raw::codegen::sourceRequest(text, compileArguments(options)),
                    "a library from source", error);
}

// Core Image's SPI descriptor (newLibraryWithDescriptorSPI:, its graphs with SPI texture and builtin nodes) is of the same
// class family: the base class's newDagStringWithGraphs: writes its DAG text too
- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    NSArray<MTLFunctionStitchingGraph *> *graphs = d.functionGraphs;
    if (!graphs.count || !d.functions.count)
        return failed(MTLLibraryErrorCompileFailure, "a stitched library needs graphs and functions", error);
    if (![self respondsToSelector:@selector(newDagStringWithGraphs:)])
        return failed(MTLLibraryErrorInternal, "no DAG writer in the base device", error);
    return [self newLibraryWithDAG:[(id<RMDagWriter>)self newDagStringWithGraphs:graphs]
                         functions:d.functions
                             error:error];
}

// The stitching DAG's text and the visible functions its nodes name: a stitched descriptor's, and MPS's kernel DAGs
// (MPSKernelDAG, which MPSGraph's control flow and fused kernels make and abort on nil)
- (id<MTLLibrary>)newLibraryWithDAG:(NSString *)dag
                          functions:(NSArray<id<MTLFunction>> *)functions
                              error:(NSError **)error
{
    const char *text = [dag isKindOfClass:[NSString class]] ? dag.UTF8String : nullptr;
    if (!text || !*text)
        return failed(MTLLibraryErrorCompileFailure, "a stitched library needs its DAG text", error);
    if (!functions.count)
        return failed(MTLLibraryErrorCompileFailure, "a stitched library needs the functions its DAG names", error);
    std::vector<raw::codegen::Bitcode> bitcodes;
    for (id function in functions) {
        raw::codegen::Bitcode b;
        if (!bitcodeOf(function, b))
            return failed(MTLLibraryErrorFunctionNotFound,
                          std::string("stitched function ") + ([function name].UTF8String ?: "?") +
                              " has no bitcode here",
                          error);
        bitcodes.push_back(b);
    }
    return compiled(self, raw::kCodeGenStitch, raw::codegen::stitchRequest(text, bitcodes), "a stitched library",
                    error);
}

// Core Image's legacy DAG and a kernel's constants reflection (_MTLDevice's newLibraryWithImageFilterFunctionsSPI:)
- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const void *)info
                                    error:(NSError **)error
{
    const auto *infos = static_cast<const switch_metal::ImageFilterFunctionInfo *>(info);
    if (!functions.count || !infos)
        return failed(MTLLibraryErrorCompileFailure, "filter-function linking needs functions and their infos", error);
    std::vector<switch_metal::ImageFilterFunction> linked(functions.count);
    for (NSUInteger i = 0; i < functions.count; i++) {
        raw::codegen::Bitcode b;
        if (!bitcodeOf(functions[i], b))
            return failed(MTLLibraryErrorFunctionNotFound,
                          std::string("filter function ") + ([functions[i] name].UTF8String ?: "?") +
                              " has no bitcode here",
                          error);
        switch_metal::ImageFilterFunction &f = linked[i];
        f.name = [functions[i] name].UTF8String ?: "";
        f.bitcode = b.bytes;
        f.bitcodeBytes = b.size;
        f.earlyOut = infos[i].earlyOut;
        if (infos[i].argumentCount && infos[i].arguments)
            f.arguments.assign(infos[i].arguments, infos[i].arguments + infos[i].argumentCount);
    }
    const std::vector<uint8_t> request = switch_metal::imageFilterLinkRequest(linked);
    if (request.empty())
        return failed(MTLLibraryErrorCompileFailure, "the filter functions do not fit one link request", error);
    return compiled(self, raw::kCodeGenImageFilters, std::string(request.begin(), request.end()),
                    "a Core Image filter library", error);
}

@end
