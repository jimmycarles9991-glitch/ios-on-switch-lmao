// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawPipelineKey.h"
#include "NativeBridgeMetalFrontend.h"

// The function's m2v twin, made of its library's at the first ask for one: `need` names what asked
#define RM_FUNCTION_TWIN(f, need) ((f)->_twin ?: [(f) twinOn:RM_M2V(need)])

// The function list entry's TESS tag in the AIR slice the facade read (MetallibEntries.h), as m2v's function answers it;
// false where the facade read none. Every render state's creation asks it (its key, rmUnsupportedRenderState,
// rmM2vMissingState), so it makes no twin.
static bool ownTess(RMFunction *f, switch_metal::FunctionTess &tess)
{
    return f->_source && f->_source->bytes &&
           switch_metal::metallibFunctionTess(f->_source->bytes, f->_source->size, f->_libraryName, tess);
}

@implementation RMFunction

+ (Class)twinClass
{
    return NSClassFromString(@"NBFunction");
}

// m2v's function of the same library function, name and constant values, of the library's twin
- (id)twinOn:(id<MTLDevice>)m2v
{
    @synchronized(self) {
        if (_twin || !_library)
            return _twin;
    }
    id<MTLLibrary> library = [_library twinOn:m2v];
    NSString *name = [NSString stringWithUTF8String:_libraryName.c_str()];
    id made = nil;
    if (_values || ![self.name isEqualToString:name]) {
        MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
        d.name = name;
        if (![self.name isEqualToString:name])
            d.specializedName = self.name;
        d.constantValues = _values;
        made = [library newFunctionWithDescriptor:d error:nil];
    } else {
        made = [library newFunctionWithName:name];
    }
    return twinPublish(self, _twin, made);
}

- (id)rmTwin
{
    return RM_FUNCTION_TWIN(self, "a function's twin");
}

- (MTLFunctionType)functionType
{
    return _type ? MTLFunctionType(_type) : [(id<MTLFunction>)RM_FUNCTION_TWIN(self, "a function's type") functionType];
}

// Reflection: the AIR's, as m2v's function decodes it
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary
{
    return [(id<MTLFunction>)RM_FUNCTION_TWIN(self, "a function's constants") functionConstantsDictionary];
}

- (MTLFunctionOptions)options
{
    return [(id<MTLFunction>)RM_FUNCTION_TWIN(self, "a function's options") options];
}

- (NSArray<MTLVertexAttribute *> *)vertexAttributes
{
    return [(id<MTLFunction>)RM_FUNCTION_TWIN(self, "a function's vertex attributes") vertexAttributes];
}

- (NSArray<MTLAttribute *> *)stageInputAttributes
{
    return [(id<MTLFunction>)RM_FUNCTION_TWIN(self, "a function's stage input attributes") stageInputAttributes];
}

- (MTLPatchType)patchType
{
    switch_metal::FunctionTess tess;
    return ownTess(self, tess) ? MTLPatchType(tess.patchType) : [(id<MTLFunction>)_twin patchType];
}

- (NSInteger)patchControlPointCount
{
    switch_metal::FunctionTess tess;
    return ownTess(self, tess) ? tess.controlPoints : [(id<MTLFunction>)_twin patchControlPointCount];
}

- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
{
    return rmFunctionArgumentEncoder(self, index);
}

// No reflection object is made: the reflection form answers nil there, as Metal does when it was not asked for (m2v's)
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
                                                 reflection:(MTLAutoreleasedArgument *)reflection
{
    if (reflection)
        *reflection = nil;
    return rmFunctionArgumentEncoder(self, index);
}

@end

// The library's m2v twin, made at the first ask for one: `need` names what asked
#define RM_LIBRARY_TWIN(l, need) ((l)->_twin ?: [(l) twinOn:RM_M2V(need)])

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

@implementation RMLibrary

+ (Class)twinClass
{
    return NSClassFromString(@"NBLibrary");
}

// m2v's library of the same metallib: from its path, or from the bytes the facade holds
- (id)twinOn:(id<MTLDevice>)m2v
{
    @synchronized(self) {
        if (_twin || _functions.types.empty())
            return _twin;
    }
    std::shared_ptr<raw::LibrarySource> source = _source;
    id made = nil;
    if (!source->path.empty()) {
        made = [m2v newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:source->path.c_str()]]
                                error:nil];
    } else {
        dispatch_data_t data = dispatch_data_create(source->bytes, source->size, nullptr, ^{
            (void)source;
        });
        made = [m2v newLibraryWithData:data error:nil];
    }
    return twinPublish(self, _twin, made);
}

- (id)rmTwin
{
    return RM_LIBRARY_TWIN(self, "a library's twin");
}

- (RMFunction *)wrap:(id<MTLFunction>)twin name:(NSString *)name constants:(MTLFunctionConstantValues *)values
{
    if (!twin)
        return nil;
    RMFunction *f = [RMFunction new];
    f->_device = _device;
    f->_twin = twin;
    f->_constants = raw::constantsText(values);
    f->_framework = _framework;
    f->_source = _source;
    f->_specialized = values != nil;
    f->_libraryName = native_bridge_metal_function_library_name(twin).UTF8String ?: "";
    f.name = name;
    return f;
}

// A function of a library the facade read, with no twin: `libraryName` is listed with a type the facade answers
- (RMFunction *)function:(NSString *)libraryName
                      as:(NSString *)specializedName
                  values:(MTLFunctionConstantValues *)values
             specialized:(bool)specialized
{
    RMFunction *f = [RMFunction new];
    f->_device = _device;
    f->_library = self;
    f->_constants = raw::constantsText(values);
    f->_framework = _framework;
    f->_source = _source;
    f->_specialized = specialized;
    f->_values = [values copy];
    f->_libraryName = libraryName.UTF8String ?: "";
    f->_type = _functions.type(f->_libraryName);
    f.name = specializedName.length ? specializedName : libraryName;
    return f;
}

// Whether a function of `name` is made here without m2v: the facade read the library and its type is one it answers
- (bool)ownsFunction:(NSString *)name
{
    return !_functions.types.empty() && name && _functions.type(name.UTF8String ?: "");
}

- (bool)listsFunction:(NSString *)name
{
    return _functions.types.empty() || (name && _functions.lists(name.UTF8String ?: ""));
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    if ([self ownsFunction:name])
        return (id<MTLFunction>)[self function:name as:nil values:nil specialized:false];
    if (![self listsFunction:name])
        return nil;
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)RM_LIBRARY_TWIN(self, "a function of a kind raw does not list")
                                           newFunctionWithName:name]
                                  name:name
                             constants:nil];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    if (values && [self ownsFunction:name]) {
        if (error)
            *error = nil;
        return (id<MTLFunction>)[self function:name as:nil values:values specialized:true];
    }
    if (![self listsFunction:name])
        return functionNotFound(name, error);
    return (id<MTLFunction>)[self
             wrap:[(id<MTLLibrary>)RM_LIBRARY_TWIN(self, "a specialized function raw does not list")
                      newFunctionWithName:name
                           constantValues:values
                                    error:error]
             name:name
        constants:values];
}

- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    if (values && [self ownsFunction:name]) {
        handler((id<MTLFunction>)[self function:name as:nil values:values specialized:true], nil);
        return;
    }
    RMLibrary *me = self;
    MTLFunctionConstantValues *copy = [values copy];
    [(id<MTLLibrary>)RM_LIBRARY_TWIN(self, "a specialized function raw does not list")
        newFunctionWithName:name
             constantValues:values
          completionHandler:^(id<MTLFunction> f, NSError *e) {
              handler((id<MTLFunction>)[me wrap:f name:name constants:copy], e);
          }];
}

// Named as Metal names it: the specialized name it was made under, else the library function's
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    if ([self ownsFunction:d.name]) {
        if (error)
            *error = nil;
        return (id<MTLFunction>)[self function:d.name
                                            as:d.specializedName
                                        values:d.constantValues
                                   specialized:d.constantValues != nil];
    }
    if (d.name && ![self listsFunction:d.name])
        return functionNotFound(d.name, error);
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)RM_LIBRARY_TWIN(self, "a function raw does not list")
                                           newFunctionWithDescriptor:d
                                                               error:error]
                                  name:d.specializedName.length ? d.specializedName : d.name
                             constants:d.constantValues];
}

// Made at once, as m2v's library makes it: through the descriptor, so the function is this library's (its states keyed by
// the pack), never m2v's own
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

- (NSArray<NSString *> *)functionNames
{
    return [(id<MTLLibrary>)RM_LIBRARY_TWIN(self, "a library's function names") functionNames];
}

- (NSArray<NSString *> *)externFunctionNames
{
    return [RM_LIBRARY_TWIN(self, "a library's extern function names") externFunctionNames];
}

- (id<MTLFunction>)newExternFunctionWithName:(NSString *)name
{
    return (id<MTLFunction>)[self wrap:[RM_LIBRARY_TWIN(self, "an extern function") newExternFunctionWithName:name]
                                  name:name
                             constants:nil];
}

- (MTLLibraryType)type
{
    return [(id<MTLLibrary>)RM_LIBRARY_TWIN(self, "a library's type") type];
}

@end

RMLibrary *wrapLibrary(id device, id<MTLLibrary> twin, NSString *path)
{
    if (!twin)
        return nil;
    RMLibrary *l = [RMLibrary new];
    l->_device = device;
    l->_twin = twin;
    l->_source = std::make_shared<raw::LibrarySource>(path.UTF8String ?: "");
    l->_framework = raw::frameworkOf(l->_source->path);
    return l;
}

RMLibrary *readLibrary(id device, NSString *path, NSData *bytes)
{
    raw::MetallibFunctions functions;
    if (!bytes || !functions.read(static_cast<const uint8_t *>(bytes.bytes), bytes.length))
        return nil;
    RMLibrary *l = [RMLibrary new];
    l->_device = device;
    const void *kept = CFBridgingRetain(bytes);
    l->_source = std::make_shared<raw::LibrarySource>(path.UTF8String ?: "", functions.slice, functions.sliceBytes,
                                                      std::shared_ptr<const void>(kept, [](const void *b) {
                                                          CFRelease(b);
                                                      }));
    l->_framework = raw::frameworkOf(l->_source->path);
    l->_functions = std::move(functions);
    return l;
}

bool rmFunctionSource(id<MTLFunction> function, const void **bytes, size_t *length, NSString **name)
{
    if (![(id)function isKindOfClass:[RMFunction class]])
        return false;
    RMFunction *f = (RMFunction *)function;
    if (f->_source && f->_source->bytes) {
        *bytes = f->_source->bytes;
        *length = f->_source->size;
        *name = [NSString stringWithUTF8String:libraryName(f).c_str()];
        return true;
    }
    id twin = f->_twin;
    if (!native_bridge_metal_function_metallib(twin, bytes, length))
        return false;
    *name = native_bridge_metal_function_library_name(twin);
    return true;
}

// The library function a function of this facade was made from, which the journal names (its name is a specialized one's)
std::string libraryName(RMFunction *f)
{
    if (!f->_libraryName.empty())
        return f->_libraryName;
    return f.name.UTF8String ?: "?";
}

// The descriptor's linked functions as the journal's R line names them without their libraries (NativeBridgeMetalFrontend.mm
// linkedText): " | <vl|vp|fl|fp> <library function>[ as=<specialized name>] <constants>", "-" for one not this facade's
static std::string linkedKey(MTLRenderPipelineDescriptor *d)
{
    std::string key;
    const std::pair<const char *, NSArray *> lists[] = {{"vl", d.vertexLinkedFunctions.functions},
                                                        {"vp", d.vertexLinkedFunctions.privateFunctions},
                                                        {"fl", d.fragmentLinkedFunctions.functions},
                                                        {"fp", d.fragmentLinkedFunctions.privateFunctions}};
    for (const auto &[tag, functions] : lists)
        for (id f in functions) {
            if (![f isKindOfClass:[RMFunction class]]) {
                key += std::string(" | ") + tag + " - ";
                continue;
            }
            RMFunction *rf = f;
            key += raw::linkedSegment(tag, {libraryName(rf), rf->_constants}, rf.name.UTF8String ?: "");
        }
    return key;
}

bool raw_metal_linked_packable(MTLRenderPipelineDescriptor *d)
{
    for (MTLLinkedFunctions *l : {d.vertexLinkedFunctions, d.fragmentLinkedFunctions})
        for (NSArray *functions : {l.functions, l.privateFunctions})
            for (id f in functions)
                if (![f isKindOfClass:[RMFunction class]] || ((RMFunction *)f)->_framework.empty())
                    return false;
    return true;
}

NSString *raw_metal_pipeline_key(MTLRenderPipelineDescriptor *d)
{
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    const raw::StageKey vertex{vf ? libraryName(vf) : "", vf ? vf->_constants : ""};
    const raw::StageKey fragment{ff ? libraryName(ff) : "", ff ? ff->_constants : ""};
    const std::string key =
        raw::pipelineKey(d, vf ? &vertex : nullptr, ff ? &fragment : nullptr, linkedKey(d), vf ? vf->_framework : "");
    return [NSString stringWithUTF8String:key.c_str()];
}
