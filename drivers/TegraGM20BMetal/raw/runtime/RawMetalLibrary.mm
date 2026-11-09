// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "NativeBridgeMetalFrontend.h"

// MTLDataType sizes, as the frontend journals constants (NativeBridgeMetalFrontend.mm constantBytes)
static size_t constantBytes(uint64_t type)
{
    switch (type) {
    case 3:
    case 29:
    case 33:
        return 4;
    case 4:
    case 30:
    case 34:
        return 8;
    case 6:
    case 32:
    case 36:
        return 16;
    case 16:
    case 37:
    case 41:
        return 2;
    case 17:
    case 38:
    case 42:
        return 4;
    case 19:
    case 40:
    case 44:
        return 8;
    case 45:
    case 49:
    case 53:
        return 1;
    case 46:
    case 50:
    case 54:
        return 2;
    case 48:
    case 52:
    case 56:
        return 4;
    case 81:
    case 85:
        return 8;
    case 82:
    case 86:
        return 16;
    case 84:
    case 88:
        return 32;
    default:
        return 0;
    }
}

@protocol RMConstantValue  // MTLFunctionConstantValues' private entries (the frontend reads them the same way)
- (MTLDataType)dataType;
- (const void *)data;
- (NSString *)name;
- (NSUInteger)index;
@end
@interface MTLFunctionConstantValues (RMPrivate)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end

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

static std::string constantsText(MTLFunctionConstantValues *values)
{
    std::string text;
    if (!values)
        return text;
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *groups[2] = {[snapshot newIndexedConstantArray], [snapshot newNamedConstantArray]};
    for (int g = 0; g < 2; g++)
        for (id<RMConstantValue> v in groups[g]) {
            const size_t n = constantBytes(v.dataType);
            char head[48];
            if (g)
                text += std::string(text.empty() ? "" : " ") + v.name.UTF8String;
            else {
                snprintf(head, sizeof head, "%s#%lu", text.empty() ? "" : " ", (unsigned long)v.index);
                text += head;
            }
            snprintf(head, sizeof head, ":%lu:", (unsigned long)v.dataType);
            text += head;
            for (size_t i = 0; i < n; i++) {
                snprintf(head, sizeof head, "%02x", ((const uint8_t *)v.data)[i]);
                text += head;
            }
        }
    return text;
}

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
    f->_constants = constantsText(values);
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
            key += std::string(" | ") + tag + " ";
            if (![f isKindOfClass:[RMFunction class]]) {
                key += "- ";
                continue;
            }
            RMFunction *rf = f;
            const std::string own = libraryName(rf);
            key += own;
            if (own != (rf.name.UTF8String ?: ""))
                key += std::string(" as=") + rf.name.UTF8String;
            key += " " + rf->_constants;
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
    // the journal's R-line fields in its order (NativeBridgeMetalFrontend.mm journalRenderPipeline), but the label, each with a
    // blank after it, then the functions; no trailing blank (the pack's keys: pack/probelog.py)
    char head[160];
    snprintf(head, sizeof head, "samples=%lu topo=%lu ds=%lu/%lu ", (unsigned long)d.rasterSampleCount,
             (unsigned long)d.inputPrimitiveTopology, (unsigned long)d.depthAttachmentPixelFormat,
             (unsigned long)d.stencilAttachmentPixelFormat);
    std::string key = head;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid)
            continue;
        snprintf(head, sizeof head, "c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.pixelFormat,
                 (unsigned long)a.writeMask, (int)a.blendingEnabled, (unsigned long)a.sourceRGBBlendFactor,
                 (unsigned long)a.destinationRGBBlendFactor, (unsigned long)a.rgbBlendOperation,
                 (unsigned long)a.sourceAlphaBlendFactor, (unsigned long)a.destinationAlphaBlendFactor,
                 (unsigned long)a.alphaBlendOperation);
        key += head;
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            snprintf(head, sizeof head, "a%u=%lu:%lu:%lu:%lu:%lu:%lu ", i, (unsigned long)a.format,
                     (unsigned long)a.offset, (unsigned long)a.bufferIndex, (unsigned long)l.stride,
                     (unsigned long)l.stepFunction, (unsigned long)l.stepRate);
            key += head;
        }
    RMFunction *vf = [(id)d.vertexFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.vertexFunction : nil;
    RMFunction *ff = [(id)d.fragmentFunction isKindOfClass:[RMFunction class]] ? (RMFunction *)d.fragmentFunction : nil;
    key += " | v " + (vf ? libraryName(vf) : std::string("-")) + " " + (vf ? vf->_constants : "");
    key += " | f " + (ff ? libraryName(ff) : std::string("-")) + " " + (ff ? ff->_constants : "");
    key += linkedKey(d);
    if (vf && vf->_framework != "QuartzCore" && !vf->_framework.empty())
        key += " | l " + vf->_framework;
    while (!key.empty() && key.back() == ' ')
        key.pop_back();
    return [NSString stringWithUTF8String:key.c_str()];
}
