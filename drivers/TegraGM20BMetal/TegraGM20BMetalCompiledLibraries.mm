// SPDX-License-Identifier: MIT
// Libraries the system's Metal front end makes at run time: Metal source, stitched functions, Core Image's
// filter-function link and the specializations of the functions those take. The device writes the front end's request
// from its own functions' metallibs (codegen_request.h), the system shader compiler answers it with a metallib
// (compiler/compiler_codegen.mm), and the library is read as any other (readLibrary), so its functions are this device's
// own.
#import "TegraGM20BMetalInternal.h"

#include <cstdlib>
#include <list>
#include <map>
#include <os/log.h>
#include "ImageFilterLink.h"
#include "codegen_request.h"
#include "library_entry.h"

// _MTLDevice's own writer of stitching graphs as the DAG text Metal's front end reads (the device class's base)
@protocol SwitchMetalDagWriter
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs;
@end

// Metal's serialization of the values for `function`'s functionConstants, which a specialization request carries; nil
// with Metal's message for a required constant left out or a value of another type. The buffer is malloc'd: Metal's own
// caller frees it (DISPATCH_DATA_DESTRUCTOR_FREE).
@interface MTLFunctionConstantValues (SwitchMetalSpecialization)
- (void *)serializedConstantDataForFunction:(id<MTLFunction>)function
                                   dataSize:(NSUInteger *)size
                               errorMessage:(NSString **)message;
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

static bool refused(MTLLibraryError code, const std::string &why, NSError **error)
{
    failed(code, why, error);
    return false;
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
            " " + gm20b::codegen::quotedArgument([NSString stringWithFormat:@"-D%@=%@", name, value].UTF8String ?: "");
    }];
    return arguments;
}

// `function`'s wrapped bitcode in the metallib its library read; false for a function of another device
static bool bitcodeOf(id function, gm20b::codegen::Bitcode &out)
{
    if (![function isKindOfClass:[SwitchMetalFunction class]])
        return false;
    SwitchMetalFunction *f = function;
    return f->_source && f->_source->bytes &&
           gm20b::codegen::functionBitcode(f->_source->bytes, f->_source->size, libraryName(f), out);
}

static id<MTLLibrary> compiled(id device, uint32_t type, const std::string &request, const char *what, NSError **error)
{
#if SWITCH_METAL_STATS
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    gm20b::MadeLibrary made;
    std::string failure = "no shader compiler comes this boot";
    bool refused = false;
    if (!gRM->completion || !gRM->completion->library(type, request, made, failure, refused)) {
        os_log_error(gm20bLog(), "SwitchMetal: %{public}s: %{public}s not made: %{public}.600s", getprogname(), what,
                     failure.c_str());
        return failed(refused ? MTLLibraryErrorCompileFailure : MTLLibraryErrorInternal, failure, error);
    }
    auto *metallib = new std::string(std::move(made.metallib));
    NSData *bytes = [[NSData alloc] initWithBytesNoCopy:metallib->data()
                                                 length:metallib->size()
                                            deallocator:^(void *, NSUInteger) {
                                                delete metallib;
                                            }];
    SwitchMetalLibrary *library = readLibrary(device, nil, bytes);
    if (!library)
        return failed(MTLLibraryErrorInternal,
                      std::string("the shader compiler's ") + what + " is no metallib Metal loads", error);
#if SWITCH_METAL_STATS
    os_log(gm20bLog(), "SwitchMetal: %{public}s: %{public}s made by the shader compiler in %llu ms", getprogname(),
           what, (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000));
#endif
    // A library made with warnings has no error, as Metal answers one (an M4 Max, sync and completion handler alike): a
    // CompileWarning error would throw from Swift's async makeLibrary
    if (error)
        *error = nil;
    return (id<MTLLibrary>)library;
}

namespace {
constexpr size_t kMaxSpecializedLibraries = 128;
constexpr size_t kMaxSpecializedBytes = 8u << 20;

struct SpecializedEntry {
    std::string key;
    SwitchMetalLibrary *library;
    size_t bytes;
};

struct SpecializedCache {
    gm20b::UnfairMutex lock;
    std::list<SpecializedEntry> lru;
    std::map<std::string, std::list<SpecializedEntry>::iterator, std::less<>> index;
    size_t totalBytes = 0;

    SwitchMetalLibrary *find(const std::string &key)
    {
        std::lock_guard<gm20b::UnfairMutex> guard(lock);
        const auto it = index.find(key);
        if (it == index.end())
            return nil;
        lru.splice(lru.begin(), lru, it->second);
        return it->second->library;
    }

    void put(const std::string &key, SwitchMetalLibrary *library, size_t bytes)
    {
        if (!library || bytes > kMaxSpecializedBytes)
            return;
        std::lock_guard<gm20b::UnfairMutex> guard(lock);
        const auto existing = index.find(key);
        if (existing != index.end()) {
            totalBytes -= existing->second->bytes;
            existing->second->library = library;
            existing->second->bytes = bytes;
            totalBytes += bytes;
            lru.splice(lru.begin(), lru, existing->second);
        } else {
            lru.push_front({key, library, bytes});
            index.emplace(key, lru.begin());
            totalBytes += bytes;
        }
        while (lru.size() > kMaxSpecializedLibraries || totalBytes > kMaxSpecializedBytes) {
            const SpecializedEntry &oldest = lru.back();
            totalBytes -= oldest.bytes;
            index.erase(oldest.key);
            lru.pop_back();
        }
    }

    void clear()
    {
        std::lock_guard<gm20b::UnfairMutex> guard(lock);
        index.clear();
        lru.clear();
        totalBytes = 0;
    }
};

SpecializedCache &specializedCache()
{
    static SpecializedCache *cache = new SpecializedCache;
    return *cache;
}

// The specialization's inputs themselves, each length-prefixed: a few hundred bytes, compared whole by the map
std::string specializationCacheKey(const gm20b::AirHash &air, const std::string &name, const std::string &called,
                                   const std::string &constants)
{
    std::string key(reinterpret_cast<const char *>(air.data()), air.size());
    for (const std::string *part : {&name, &called, &constants}) {
        const uint64_t length = part->size();
        key.append(reinterpret_cast<const char *>(&length), sizeof length);
        key += *part;
    }
    return key;
}
}  // namespace

void gm20bTrimCompiledLibraries()
{
    specializedCache().clear();
}

// `function`'s bitcode as Metal's own function holds it for the front end: its library's AIR, or for one made with constant
// values or under a specialized name, the front end's specialization of that AIR (MTLSpecializeFunctionToArchive), named
// as the function is. `kept` holds the specialization's library, whose bytes the bitcode is in.
static bool frontEndBitcode(id device, id function, gm20b::codegen::Bitcode &out, NSMutableArray *kept, NSError **error)
{
    const std::string called = [function name].UTF8String ?: "?";
    if (!bitcodeOf(function, out))
        return refused(MTLLibraryErrorFunctionNotFound, "function " + called + " has no bitcode here", error);
    SwitchMetalFunction *f = function;
    const std::string name = libraryName(f);
    if (!f->_specialized && name == called)
        return true;
    id<MTLFunction> source = [(id<MTLLibrary>)f->_library newFunctionWithName:@(name.c_str())];
    if (!source || !f->_air)
        return refused(MTLLibraryErrorInternal, "function " + called + " has no AIR hash to specialize", error);
    MTLFunctionConstantValues *values = f->_values ?: [MTLFunctionConstantValues new];
    NSUInteger size = 0;
    NSString *message = nil;
    void *data = [values serializedConstantDataForFunction:source dataSize:&size errorMessage:&message];
    if (!data)
        return refused(MTLLibraryErrorCompileFailure, message.UTF8String ?: "its constant values do not serialize",
                       error);
    const std::string constants(static_cast<const char *>(data), size);
    free(data);
    const std::string cacheKey = specializationCacheKey(*f->_air, name, called, constants);
    id<MTLLibrary> library = (id<MTLLibrary>)specializedCache().find(cacheKey);
    if (!library) {
        const std::string request =
            gm20b::codegen::specializationRequest(name, name == called ? "" : called, out, *f->_air, constants);
        if (request.empty())
            return refused(MTLLibraryErrorCompileFailure, "function " + called + " has no specialization request",
                           error);
        library = compiled(device, gm20b::kCodeGenSpecialize, request, "a specialized function", error);
        if (!library)
            return false;
        if ([library isKindOfClass:[SwitchMetalLibrary class]]) {
            auto *smLib = static_cast<SwitchMetalLibrary *>(library);
            const size_t bytes = smLib->_source ? smLib->_source->size : 0;
            specializedCache().put(cacheKey, smLib, bytes);
        }
    }
    [kept addObject:library];
    if (!bitcodeOf([library newFunctionWithName:@(called.c_str())], out))
        return refused(MTLLibraryErrorInternal, "the front end's specialization holds no " + called, error);
    return true;
}

@interface SwitchMetalDevice (CompiledLibraries)
@end
@implementation SwitchMetalDevice (CompiledLibraries)

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    if (!source)
        return failed(MTLLibraryErrorCompileFailure, "no source", error);
    if (options.libraryType == MTLLibraryTypeDynamic)
        return failed(MTLLibraryErrorUnsupported, "dynamic libraries are not supported", error);
    const char *text = source.UTF8String ?: "";
    return compiled(self, gm20b::kCodeGenSource, gm20b::codegen::sourceRequest(text, compileArguments(options)),
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
    return [self newLibraryWithDAG:[(id<SwitchMetalDagWriter>)self newDagStringWithGraphs:graphs]
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
    std::vector<gm20b::codegen::Bitcode> bitcodes;
    __attribute__((objc_precise_lifetime)) NSMutableArray *kept = [NSMutableArray array];
    for (id function in functions) {
        gm20b::codegen::Bitcode b;
        if (!frontEndBitcode(self, function, b, kept, error))
            return nil;
        bitcodes.push_back(b);
    }
    return compiled(self, gm20b::kCodeGenStitch, gm20b::codegen::stitchRequest(text, bitcodes), "a stitched library",
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
    __attribute__((objc_precise_lifetime)) NSMutableArray *kept = [NSMutableArray array];
    for (NSUInteger i = 0; i < functions.count; i++) {
        gm20b::codegen::Bitcode b;
        if (!frontEndBitcode(self, functions[i], b, kept, error))
            return nil;
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
    return compiled(self, gm20b::kCodeGenImageFilters, std::string(request.begin(), request.end()),
                    "a Core Image filter library", error);
}

@end
