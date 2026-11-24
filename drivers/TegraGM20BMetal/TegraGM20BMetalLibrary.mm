// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalFunctionTags.h"
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPipelineKey.h"
#import "TegraGM20BMetalStatesKey.h"
#include "metallib.h"

// The function list entry's TESS tag in the AIR slice the facade read (MetallibEntries.h); false where the facade read none
static bool ownTess(SwitchMetalFunction *f, switch_metal::FunctionTess &tess)
{
    return f->_source && f->_source->bytes &&
           switch_metal::metallibFunctionTess(f->_source->bytes, f->_source->size, f->_libraryName, tess);
}

@implementation SwitchMetalFunction

// 0 for a Core Image extern, as m2v's translator measured none for its TYPE (metallib_properties: Other(5))
- (MTLFunctionType)functionType
{
    return MTLFunctionType(_type);
}

- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary
{
    return gm20bFunctionConstants(self);
}

- (MTLFunctionOptions)options
{
    return _options;
}

- (NSArray<MTLVertexAttribute *> *)vertexAttributes
{
    return gm20bFunctionAttributes(self, false);
}

- (NSArray<MTLAttribute *> *)stageInputAttributes
{
    return gm20bFunctionAttributes(self, true);
}

- (MTLPatchType)patchType
{
    return MTLPatchType(_patchType);
}

- (NSInteger)patchControlPointCount
{
    switch_metal::FunctionTess tess;
    return ownTess(self, tess) ? tess.controlPoints : 0;
}

- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
{
    return gm20bFunctionArgumentEncoder(self, index);
}

// No reflection object is made: the reflection form answers nil there, as Metal does when it was not asked for (m2v's)
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
                                                 reflection:(MTLAutoreleasedArgument *)reflection
{
    if (reflection)
        *reflection = nil;
    return gm20bFunctionArgumentEncoder(self, index);
}

@end

// Metal's answer to a name the library lacks (the frontend's functionNotFound)
static id functionNotFound(NSString *name, NSError **error)
{
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorFunctionNotFound
                                 userInfo:@{
                                     NSLocalizedDescriptionKey :
                                         [NSString stringWithFormat:@"Function %@ was not found in the library", name]
                                 }];
    return nil;
}

@implementation SwitchMetalLibrary

// A function of a library the facade read: `libraryName` is listed with a type the facade answers
- (SwitchMetalFunction *)function:(NSString *)libraryName
                               as:(NSString *)specializedName
                           values:(MTLFunctionConstantValues *)values
                      specialized:(bool)specialized
{
    SwitchMetalFunction *f = [SwitchMetalFunction new];
    f->_device = _device;
    f->_library = self;
    f->_constants = gm20b::constantsText(values);
    f->_framework = _framework;
    f->_source = _source;
    f->_specialized = specialized;
    f->_values = [values copy];
    f->_libraryName = libraryName.UTF8String ?: "";
    f->_type = _functions.type(f->_libraryName);
    if (const auto tess = _functions.tess.find(f->_libraryName); tess != _functions.tess.end())
        f->_patchType = switch_metal::decodeTess(tess->second).patchType;
    if (const auto air = _airHashes.find(f->_libraryName); air != _airHashes.end())
        f->_air = air->second;
    f.name = specializedName.length ? specializedName : libraryName;
    f->_stageHash = gm20b::stageHash({::libraryName(f), f->_constants});
    return f;
}

// Whether the library makes a function of `name`: one of a type the facade answers, or one of its Core Image externs (a
// dynamic library's export is none: Metal's newFunctionWithName: stops at an assertion for it)
- (bool)ownsFunction:(NSString *)name
{
    const std::string n = name.UTF8String ?: "";
    return name && (_functions.type(n) || _functions.isExtern(n));
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    return [self ownsFunction:name] ? (id<MTLFunction>)[self function:name as:nil values:nil specialized:false] : nil;
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    if (![self ownsFunction:name])
        return functionNotFound(name, error);
    if (error)
        *error = nil;
    return (id<MTLFunction>)[self function:name as:nil values:values specialized:true];
}

- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error = nil;
    id<MTLFunction> function = [self newFunctionWithName:name constantValues:values error:&error];
    handler(function, error);
}

// Named as Metal names it: the specialized name it was made under, else the library function's
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    if (![self ownsFunction:d.name])
        return functionNotFound(d.name, error);
    if ((d.options & MTLFunctionOptionCompileToBinary) &&
        _functions.type(d.name.UTF8String) != MTLFunctionTypeVisible) {
        if (error)
            *error =
                [NSError errorWithDomain:MTLLibraryErrorDomain
                                    code:MTLLibraryErrorCompileFailure
                                userInfo:@{NSLocalizedDescriptionKey : @"CompileToBinary requires a visible function"}];
        return nil;
    }
    if (error)
        *error = nil;
    SwitchMetalFunction *function = [self function:d.name
                                                as:d.specializedName
                                            values:d.constantValues
                                       specialized:d.constantValues != nil];
    function->_options = d.options;
    return (id<MTLFunction>)function;
}

// Made at once, through the descriptor, so the function is this library's (its states keyed by the pack)
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d
                completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error = nil;
    id<MTLFunction> function = [self newFunctionWithDescriptor:d error:&error];
    handler(function, error);
}

// A pipeline library holds an Apple GPU's compiled functions, none for this device: the function is made as without one
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                       pipelineLibrary:(id)pipelineLibrary
                                 error:(NSError **)error
{
    (void)pipelineLibrary;
    MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
    d.name = name;
    d.constantValues = values;
    return [self newFunctionWithDescriptor:d error:error];
}

static NSArray<NSString *> *namesOf(const std::vector<std::string> &names)
{
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:names.size()];
    for (const std::string &name : names)
        if (NSString *s = [[NSString alloc] initWithBytes:name.data() length:name.size() encoding:NSUTF8StringEncoding])
            [out addObject:s];
    return out;
}

- (NSArray<NSString *> *)functionNames
{
    return namesOf(_functions.names);
}

// Core Image counts its own kernels by these (ci_stdlib, ci_filters) and finds a kernel's library by them
- (NSArray<NSString *> *)externFunctionNames
{
    return namesOf(_functions.externs);
}

// As Metal's: the function of any name the library has, extern or not
- (id<MTLFunction>)newExternFunctionWithName:(NSString *)name
{
    return [self newFunctionWithName:name];
}

// No dynamic library is made on this device (a source compile asking for one fails)
- (MTLLibraryType)type
{
    return MTLLibraryTypeExecutable;
}

@end

SwitchMetalLibrary *readLibrary(id device, NSString *path, NSData *bytes)
{
    gm20b::MetallibFunctions functions;
    if (!bytes || !functions.read(static_cast<const uint8_t *>(bytes.bytes), bytes.length))
        return nil;
    SwitchMetalLibrary *l = [SwitchMetalLibrary new];
    l->_device = device;
    const void *kept = CFBridgingRetain(bytes);
    l->_source = std::make_shared<gm20b::LibrarySource>(path.UTF8String ?: "", functions.slice, functions.sliceBytes,
                                                        std::shared_ptr<const void>(kept, [](const void *b) {
                                                            CFRelease(b);
                                                        }));
    l->_framework = gm20b::frameworkOf(l->_source->path);
    l->_functions = std::move(functions);
    gm20b::metallibAirHashes(static_cast<const uint8_t *>(bytes.bytes), bytes.length, l->_airHashes);
    return l;
}

bool gm20bFunctionSource(id<MTLFunction> function, const void **bytes, size_t *length, NSString **name)
{
    if (![(id)function isKindOfClass:[SwitchMetalFunction class]])
        return false;
    SwitchMetalFunction *f = (SwitchMetalFunction *)function;
    if (!f->_source || !f->_source->bytes)
        return false;
    *bytes = f->_source->bytes;
    *length = f->_source->size;
    *name = [NSString stringWithUTF8String:libraryName(f).c_str()];
    return true;
}

// The library function a function of this facade was made from, which the journal names (its name is a specialized one's)
std::string libraryName(SwitchMetalFunction *f)
{
    if (!f->_libraryName.empty())
        return f->_libraryName;
    return f.name.UTF8String ?: "?";
}

// The descriptor's linked functions as the journal's R line names them without their libraries (NativeBridgeMetalFrontend.mm
// linkedText): " | <vl|vp|fl|fp> <library function>[ as=<specialized name>] <constants>", "-" for one not this facade's
std::string gm20bLinkedText(NSArray *vertex, NSArray *vertexPrivate, NSArray *fragment, NSArray *fragmentPrivate)
{
    std::string key;
    static const char *const tags[4] = {"vl", "vp", "fl", "fp"};
    NSArray *lists[4] = {vertex, vertexPrivate, fragment, fragmentPrivate};
    for (int k = 0; k < 4; k++)
        for (id f in lists[k]) {
            const char *tag = tags[k];
            if (![f isKindOfClass:[SwitchMetalFunction class]]) {
                key += std::string(" | ") + tag + " - ";
                continue;
            }
            SwitchMetalFunction *rf = f;
            key += gm20b::linkedSegment(tag, {libraryName(rf), rf->_constants}, rf.name.UTF8String ?: "");
        }
    return key;
}

bool metal_linked_packable(MTLRenderPipelineDescriptor *d)
{
    for (MTLLinkedFunctions *l : {d.vertexLinkedFunctions, d.fragmentLinkedFunctions})
        for (NSArray *functions : {l.functions, l.privateFunctions})
            for (id f in functions)
                if (![f isKindOfClass:[SwitchMetalFunction class]] || ((SwitchMetalFunction *)f)->_framework.empty())
                    return false;
    return true;
}

NSString *metal_pipeline_key(MTLRenderPipelineDescriptor *d)
{
    SwitchMetalFunction *vf = [(id)d.vertexFunction isKindOfClass:[SwitchMetalFunction class]]
                                  ? (SwitchMetalFunction *)d.vertexFunction
                                  : nil;
    SwitchMetalFunction *ff = [(id)d.fragmentFunction isKindOfClass:[SwitchMetalFunction class]]
                                  ? (SwitchMetalFunction *)d.fragmentFunction
                                  : nil;
    const gm20b::StageKey vertex{vf ? libraryName(vf) : "", vf ? vf->_constants : ""};
    const gm20b::StageKey fragment{ff ? libraryName(ff) : "", ff ? ff->_constants : ""};
    const std::string linked =
        gm20bLinkedText(d.vertexLinkedFunctions.functions, d.vertexLinkedFunctions.privateFunctions,
                        d.fragmentLinkedFunctions.functions, d.fragmentLinkedFunctions.privateFunctions);
    const std::string key =
        gm20b::pipelineKey(d, vf ? &vertex : nullptr, ff ? &fragment : nullptr, linked, vf ? vf->_framework : "");
    return [NSString stringWithUTF8String:key.c_str()];
}
