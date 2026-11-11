// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawPipelineKey.h"
#include "NativeBridgeMetalFrontend.h"

@implementation RMFunction

+ (Class)twinClass
{
    return NSClassFromString(@"NBFunction");
}

- (MTLFunctionType)functionType
{
    return [(id<MTLFunction>)_twin functionType];
}

// Reflection: the AIR's, as m2v's function decodes it (the library raw keeps of m2v)
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary
{
    return [(id<MTLFunction>)_twin functionConstantsDictionary];
}

- (MTLFunctionOptions)options
{
    return [(id<MTLFunction>)_twin options];
}

- (NSArray<MTLVertexAttribute *> *)vertexAttributes
{
    return [(id<MTLFunction>)_twin vertexAttributes];
}

- (NSArray<MTLAttribute *> *)stageInputAttributes
{
    return [(id<MTLFunction>)_twin stageInputAttributes];
}

- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
{
    return [(id<MTLFunction>)_twin newArgumentEncoderWithBufferIndex:index];
}

- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
                                                 reflection:(MTLAutoreleasedArgument *)reflection
{
    return [(id<MTLFunction>)_twin newArgumentEncoderWithBufferIndex:index reflection:reflection];
}

@end

@implementation RMLibrary

+ (Class)twinClass
{
    return NSClassFromString(@"NBLibrary");
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
    f.name = name;
    return f;
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithName:name] name:name constants:nil];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithName:name
                                                                   constantValues:values
                                                                            error:error]
                                  name:name
                             constants:values];
}

- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    RMLibrary *me = self;
    MTLFunctionConstantValues *copy = [values copy];
    [(id<MTLLibrary>)_twin newFunctionWithName:name
                                constantValues:values
                             completionHandler:^(id<MTLFunction> f, NSError *e) {
                                 handler((id<MTLFunction>)[me wrap:f name:name constants:copy], e);
                             }];
}

// Named as Metal names it: the specialized name it was made under, else the library function's
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    return (id<MTLFunction>)[self wrap:[(id<MTLLibrary>)_twin newFunctionWithDescriptor:d error:error]
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
    return [(id<MTLLibrary>)_twin functionNames];
}

- (MTLLibraryType)type
{
    return [(id<MTLLibrary>)_twin type];
}

@end

RMLibrary *wrapLibrary(id device, id<MTLLibrary> twin, NSString *path)
{
    if (!twin)
        return nil;
    RMLibrary *l = [RMLibrary new];
    l->_device = device;
    l->_twin = twin;
    if ([path hasSuffix:@".framework/default.metallib"])
        l->_framework =
            path.stringByDeletingLastPathComponent.lastPathComponent.stringByDeletingPathExtension.UTF8String;
    return l;
}

// The library function a function of this facade was made from, which the journal names (its name is a specialized one's)
static std::string libraryName(RMFunction *f)
{
    NSString *name = native_bridge_metal_function_library_name(f->_twin) ?: f.name;
    return name.UTF8String ?: "?";
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
