// SPDX-License-Identifier: MIT
// See TegraGM20BMetalOffline.h. Each call answers as the frontend's (NativeBridgeMetalFrontend.mm NBLibrary, NBFunction,
// renderDescriptor, computeDescriptor) does for its own objects, with the translator's functions in place of the runtime's.
#import "TegraGM20BMetalOffline.h"

#include <string>
#include <vector>
#include "TegraGM20BMetalPipelineKey.h"
#include "TegraGM20BMetalRenderKey.h"
#include "MetallibEntries.h"
#include "MetallibSlice.h"
#include "translate/NativeMetalTranslate.h"

@interface GM20BOfflineLibrary : NSObject
@property (nonatomic) NmbOfflineLibrary *handle;
@property (nonatomic, strong) GM20BOfflineDevice *device;
@property (nonatomic, strong) id storage;
@property (nonatomic) const uint8_t *bytes;
@property (nonatomic) size_t length;
@end

@interface GM20BOfflineFunction : NSObject <GM20BLibraryFunction>
@property (nonatomic) NmbOfflineFunction *handle;
@property (nonatomic, strong) GM20BOfflineLibrary *library;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *libraryFunctionName;
@property (nonatomic) MTLFunctionType functionType;
@property (nonatomic) MTLFunctionOptions options;
+ (instancetype)of:(NmbOfflineFunction *)handle
           library:(GM20BOfflineLibrary *)library
              name:(NSString *)name
       libraryName:(NSString *)libraryName;
@end

// What Direct::make reads of a vertexAttributes entry
@interface GM20BOfflineAttribute : NSObject
@property (nonatomic) NSUInteger attributeIndex;
@property (nonatomic, getter=isActive) BOOL active;
@end

@implementation GM20BOfflineAttribute
@end

namespace {

NSError *refusal(NSString *why)
{
    return [NSError errorWithDomain:MTLLibraryErrorDomain
                               code:MTLLibraryErrorUnsupported
                           userInfo:@{NSLocalizedDescriptionKey : why}];
}

// The translator's library holds its storage's bytes in place until it lets go of them here
void releaseStorage(const void *owner)
{
    CFRelease(owner);
}

}  // namespace

@implementation GM20BOfflineFunction

- (void)dealloc
{
    nmb_offline_function_release(_handle);
}

+ (instancetype)of:(NmbOfflineFunction *)handle
           library:(GM20BOfflineLibrary *)library
              name:(NSString *)name
       libraryName:(NSString *)libraryName
{
    if (!handle)
        return nil;
    GM20BOfflineFunction *f = [GM20BOfflineFunction new];
    f.handle = handle;
    f.library = library;
    f.name = name;
    f.libraryFunctionName = libraryName;
    f.functionType = MTLFunctionType(nmb_offline_function_type(handle));
    return f;
}

- (const void *)libraryBytes:(size_t *)length
{
    *length = _library.length;
    return _library.bytes;
}

// The library's function list entry's TESS tag, as NBFunction answers it
- (switch_metal::FunctionTess)tess
{
    switch_metal::FunctionTess tess;
    switch_metal::metallibFunctionTess(_library.bytes, _library.length,
                                       (_libraryFunctionName ?: _name).UTF8String ?: "", tess);
    return tess;
}

- (MTLPatchType)patchType
{
    return MTLPatchType([self tess].patchType);
}

- (NSInteger)patchControlPointCount
{
    return [self tess].controlPoints;
}

- (NSArray *)vertexAttributes
{
    const int64_t count = nmb_offline_function_vertex_attribute_count(_handle);
    if (count < 0)
        return nil;
    NSMutableArray *out = [NSMutableArray arrayWithCapacity:NSUInteger(count)];
    for (uint32_t i = 0; i < uint32_t(count); i++) {
        char name[256];
        uint64_t info[5] = {};
        if (!nmb_offline_function_vertex_attribute(_handle, i, name, sizeof name, info))
            continue;
        GM20BOfflineAttribute *a = [GM20BOfflineAttribute new];
        a.attributeIndex = info[0];
        a.active = info[2] != 0;
        [out addObject:a];
    }
    return out;
}

@end

@implementation GM20BOfflineLibrary

- (void)dealloc
{
    nmb_offline_library_release(_handle);
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    if (!name)
        return nil;
    return (id<MTLFunction>)[GM20BOfflineFunction of:nmb_offline_function_new(_handle, name.UTF8String)
                                             library:self
                                                name:name
                                         libraryName:name];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    return [self newFunctionWithName:name specializedName:nil constantValues:values error:error];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                       specializedName:(NSString *)specialized
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    if (!name || !values) {
        if (error)
            *error = refusal(@"unqualified function constant values");
        return nil;
    }
    MTLFunctionConstantValues *snapshot = [values copy];
    // Wrapper elements own and free `v.data` on dealloc; keep both arrays alive until the call below copies them.
    __attribute__((objc_precise_lifetime)) NSArray *indexed = [snapshot newIndexedConstantArray];
    __attribute__((objc_precise_lifetime)) NSArray *named = [snapshot newNamedConstantArray];
    std::vector<NmbConstant> constants;
    for (unsigned group = 0; group < 2; group++)
        for (id<SwitchMetalConstantValue> v in (group ? named : indexed)) {
            const uint64_t bytes = gm20b::constantBytes(v.dataType);
            if (!bytes || !v.data) {
                if (error)
                    *error = refusal(
                        [NSString stringWithFormat:@"constant type %lu is not qualified", (unsigned long)v.dataType]);
                return nil;
            }
            constants.push_back({v.dataType, group ? 0 : v.index, group ? v.name.UTF8String : nullptr, v.data, bytes});
        }
    if (!specialized.length)
        specialized = nil;
    GM20BOfflineFunction *f =
        [GM20BOfflineFunction of:nmb_offline_function_new_specialized(_handle, name.UTF8String, specialized.UTF8String,
                                                                      constants.data(), uint32_t(constants.size()))
                         library:self
                            name:specialized ?: name
                     libraryName:name];
    if (!f && error)
        *error = refusal([NSString stringWithFormat:@"specialized function: %s", nmb_last_error()]);
    return (id<MTLFunction>)f;
}

- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    if (!d.name) {
        if (error)
            *error = refusal(@"a function name is required");
        return nil;
    }
    GM20BOfflineFunction *function = nil;
    if (d.constantValues || d.specializedName.length)
        function = (GM20BOfflineFunction *)[self newFunctionWithName:d.name
                                                     specializedName:d.specializedName
                                                      constantValues:d.constantValues ?: [MTLFunctionConstantValues new]
                                                               error:error];
    else
        function = (GM20BOfflineFunction *)[self newFunctionWithName:d.name];
    if (!function) {
        if (error)
            *error = refusal([NSString stringWithFormat:@"function %@: %s", d.name, nmb_last_error()]);
        return nil;
    }
    if ((d.options & MTLFunctionOptionCompileToBinary) && function.functionType != MTLFunctionTypeVisible) {
        if (error)
            *error = refusal(@"CompileToBinary requires a visible function");
        return nil;
    }
    function.options = d.options;
    return (id<MTLFunction>)function;
}

@end

@implementation GM20BOfflineDevice

// The library of a metallib's AIR slice, as the frontend's metallibLibrary makes it; `storage` keeps the bytes, which the
// translator reads in place. `scope`: 0 for a sealed system volume's file, else the translator scope it is read in
// (nmb_offline_library_new_held).
- (GM20BOfflineLibrary *)libraryOf:(id)storage
                             bytes:(const uint8_t *)file
                            length:(size_t)length
                             scope:(uint64_t)scope
                             error:(NSError **)error
{
    size_t offset = 0, size = 0;
    if (!switch_metal::airSlice(file, length, offset, size)) {
        if (error)
            *error = refusal(@"This library does not have a compatible AIR slice for this device");
        return nil;
    }
    NmbOfflineLibrary *handle =
        nmb_offline_library_new_held(file + offset, size, scope, CFBridgingRetain(storage), releaseStorage);
    if (!handle) {
        if (error)
            *error = refusal(@(nmb_last_error()));
        return nil;
    }
    GM20BOfflineLibrary *library = [GM20BOfflineLibrary new];
    library.handle = handle;
    library.device = self;
    library.storage = storage;
    library.bytes = file + offset;
    library.length = size;
    return library;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    NSData *bytes =
        url.isFileURL ? [NSData dataWithContentsOfFile:url.path options:NSDataReadingMappedIfSafe error:error] : nil;
    if (!bytes)
        return nil;
    return (id<MTLLibrary>)[self libraryOf:bytes
                                     bytes:static_cast<const uint8_t *>(bytes.bytes)
                                    length:bytes.length
                                     scope:nmb_offline_scope_new()
                                     error:error];
}

- (id<MTLLibrary>)newSealedLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    NSData *bytes =
        url.isFileURL ? [NSData dataWithContentsOfFile:url.path options:NSDataReadingMappedAlways error:error] : nil;
    if (!bytes)
        return nil;
    return (id<MTLLibrary>)[self libraryOf:bytes
                                     bytes:static_cast<const uint8_t *>(bytes.bytes)
                                    length:bytes.length
                                     scope:0
                                     error:error];
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    return [self newLibraryWithData:data scope:nmb_offline_scope_new() error:error];
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data scope:(uint64_t)scope error:(NSError **)error
{
    if (!scope) {
        if (error)
            *error = refusal(@"a library of data in the sealed scope");
        return nil;
    }
    const void *bytes = nullptr;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &size);
    return (id<MTLLibrary>)[self libraryOf:mapped
                                     bytes:static_cast<const uint8_t *>(bytes)
                                    length:size
                                     scope:scope
                                     error:error];
}

@end

namespace offline {
namespace {

bool handles(NSArray *functions, std::vector<NmbOfflineFunction *> &out)
{
    for (id f in functions) {
        if (![f isKindOfClass:[GM20BOfflineFunction class]])
            return false;
        out.push_back(((GM20BOfflineFunction *)f).handle);
    }
    return true;
}

// `linked` gets the stage's functions, then its privateFunctions: `visible` and `hidden` of them
bool stageLinked(MTLLinkedFunctions *functions, std::vector<NmbOfflineFunction *> &linked, size_t &visible,
                 size_t &hidden)
{
    if (functions.binaryFunctions.count || functions.groups.count)
        return false;
    const size_t before = linked.size();
    if (!handles(functions.functions, linked))
        return false;
    visible = linked.size() - before;
    if (!handles(functions.privateFunctions, linked))
        return false;
    hidden = linked.size() - before - visible;
    return true;
}

// `linked` holds the handles `p` points into
bool renderDescriptor(MTLRenderPipelineDescriptor *d, NmbOfflineRenderPipelineDescriptor &p,
                      std::vector<NmbOfflineFunction *> &linked)
{
    if (![(id)d.vertexFunction isKindOfClass:[GM20BOfflineFunction class]] ||
        (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[GM20BOfflineFunction class]]))
        return false;
    size_t vertexVisible = 0, vertexHidden = 0, fragmentVisible = 0, fragmentHidden = 0;
    if (!stageLinked(d.vertexLinkedFunctions, linked, vertexVisible, vertexHidden) ||
        !stageLinked(d.fragmentLinkedFunctions, linked, fragmentVisible, fragmentHidden))
        return false;
    p.vertex_linked = linked.data();
    p.vertex_linked_count = vertexVisible;
    p.vertex_private_linked = linked.data() + vertexVisible;
    p.vertex_private_count = vertexHidden;
    p.fragment_linked = linked.data() + vertexVisible + vertexHidden;
    p.fragment_linked_count = fragmentVisible;
    p.fragment_private_linked = linked.data() + vertexVisible + vertexHidden + fragmentVisible;
    p.fragment_private_count = fragmentHidden;
    p.vertex = ((GM20BOfflineFunction *)d.vertexFunction).handle;
    p.fragment = d.fragmentFunction ? ((GM20BOfflineFunction *)d.fragmentFunction).handle : nullptr;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        // A two-plane code format renders into its texture's RGBA16Float stand-in (the frontend's rendersThroughStandIn)
        const NSUInteger format = a.pixelFormat == 542 ? MTLPixelFormatRGBA16Float : a.pixelFormat;
        p.colors[i] = {(uint64_t)format,
                       (uint64_t)a.writeMask,
                       (uint64_t)a.blendingEnabled,
                       (uint64_t)a.sourceRGBBlendFactor,
                       (uint64_t)a.destinationRGBBlendFactor,
                       (uint64_t)a.rgbBlendOperation,
                       (uint64_t)a.sourceAlphaBlendFactor,
                       (uint64_t)a.destinationAlphaBlendFactor,
                       (uint64_t)a.alphaBlendOperation};
    }
    p.depth_format = d.depthAttachmentPixelFormat;
    p.stencil_format = d.stencilAttachmentPixelFormat;
    p.samples = d.rasterSampleCount;
    p.topology = d.inputPrimitiveTopology;
    p.rasterization = d.rasterizationEnabled;
    // Metal's defaults are what the bridge takes without it (m2v TessellationDescriptor::default)
    if (d.maxTessellationFactor != 16 || d.tessellationPartitionMode || d.tessellationFactorFormat ||
        d.tessellationFactorStepFunction || d.tessellationOutputWindingOrder || d.tessellationControlPointIndexType ||
        d.tessellationFactorScaleEnabled) {
        p.tessellation = 1;
        p.max_tessellation_factor = d.maxTessellationFactor;
        p.partition_mode = d.tessellationPartitionMode;
        p.factor_format = d.tessellationFactorFormat;
        p.factor_step_function = d.tessellationFactorStepFunction;
        p.winding = d.tessellationOutputWindingOrder;
        p.control_point_index_type = d.tessellationControlPointIndexType;
        p.factor_scale_enabled = d.tessellationFactorScaleEnabled;
    }
    p.max_amplification = d.maxVertexAmplificationCount;
    p.texture_write_rounding = gm20b::textureWriteRoundingOf(d);
    p.alpha_to_coverage = d.alphaToCoverageEnabled ? 1 : 0;
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            p.vertex_inputs = 1;
            p.attributes[i] = {(uint64_t)a.format, a.offset, a.bufferIndex};
            if (a.bufferIndex < 31) {
                MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex];
                p.layouts[a.bufferIndex] = {l.stride, (uint64_t)l.stepFunction, l.stepRate};
            }
        }
    return true;
}

bool computeDescriptor(MTLComputePipelineDescriptor *d, NmbOfflineComputePipelineDescriptor &p,
                       std::vector<NmbOfflineFunction *> &linked, std::vector<switch_linked_group_name> &groupNames)
{
    if (!d || ![(id)d.computeFunction isKindOfClass:[GM20BOfflineFunction class]])
        return false;
    // Metal materializes an empty stage input descriptor on access: no attribute set is none
    if (MTLStageInputOutputDescriptor *stage = d.stageInputDescriptor) {
        for (NSUInteger index = 0; index < 31; ++index) {
            MTLAttributeDescriptor *a = stage.attributes[index];
            if (a.format == MTLAttributeFormatInvalid)
                continue;
            p.stage_inputs = 1;
            p.stage_attributes[index] = {(uint64_t)a.format, a.offset, a.bufferIndex};
            if (a.bufferIndex < 31) {
                MTLBufferLayoutDescriptor *l = stage.layouts[a.bufferIndex];
                p.stage_layouts[a.bufferIndex] = {l.stride, (uint64_t)l.stepFunction, l.stepRate};
            }
        }
        p.stage_index_type = stage.indexType;
        p.stage_index_buffer = stage.indexBufferIndex;
    }
    MTLLinkedFunctions *functions = d.linkedFunctions;
    if (d.preloadedLibraries.count || d.insertLibraries.count)
        return false;
    if (!handles(functions.functions, linked))
        return false;
    const size_t visible = linked.size();
    if (!handles(functions.privateFunctions, linked))
        return false;
    const size_t binary = linked.size();
    if (!handles(functions.binaryFunctions, linked))
        return false;
    for (id<MTLFunction> function in functions.binaryFunctions)
        if (!(function.options & MTLFunctionOptionCompileToBinary))
            return false;
    const size_t grouped = linked.size();
    for (NSString *group in [functions.groups.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSArray *members = functions.groups[group];
        if (!handles(members, linked))
            return false;
        groupNames.insert(groupNames.end(), members.count,
                          {group.UTF8String, [group lengthOfBytesUsingEncoding:NSUTF8StringEncoding]});
    }
    p.linked = linked.data();
    p.linked_count = visible;
    p.private_linked = linked.data() + visible;
    p.private_count = binary - visible;
    p.binary_linked = linked.data() + binary;
    p.binary_count = grouped - binary;
    p.group_linked = linked.data() + grouped;
    p.group_names = groupNames.data();
    p.group_count = groupNames.size();
    p.function = ((GM20BOfflineFunction *)d.computeFunction).handle;
    p.max_threads = d.maxTotalThreadsPerThreadgroup;
    p.multiple_of_execution_width = d.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    p.indirect = d.supportIndirectCommandBuffers;
    p.max_call_stack_depth = d.maxCallStackDepth;
    p.adding_binary_functions = d.supportAddingBinaryFunctions;
    for (NSUInteger index = 0; index < 31; ++index)
        p.mutability[index] = d.buffers[index].mutability;
    return true;
}

bool render(id descriptor, bool barrier, NSUInteger textureFormat, NSUInteger textureSamples,
            const NmbDirectBinds *binds, bool (*each)(const NmbDirectState *, void *), void *ctx, const char **why)
{
    NmbOfflineRenderPipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    if (!renderDescriptor((MTLRenderPipelineDescriptor *)descriptor, p, linked)) {
        *why = "a function of another device";
        return false;
    }
    if (nmb_render_pipeline_offline(&p, barrier, textureFormat, uint32_t(textureSamples), binds, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

bool compute(id descriptor, const NmbDirectDispatch *dispatch, bool (*each)(const NmbDirectKernel *, void *), void *ctx,
             const char **why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    std::vector<switch_linked_group_name> groupNames;
    if (!computeDescriptor((MTLComputePipelineDescriptor *)descriptor, p, linked, groupNames)) {
        *why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    if (nmb_compute_pipeline_offline(&p, dispatch, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

}  // namespace

bool kernelMade(MTLComputePipelineDescriptor *descriptor, std::string &why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    std::vector<switch_linked_group_name> groupNames;
    if (!computeDescriptor(descriptor, p, linked, groupNames)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    if (nmb_offline_compute_pipeline_compiles(&p))
        return true;
    why = nmb_last_error();
    return false;
}

bool kernelFacts(MTLComputePipelineDescriptor *descriptor, uint32_t facts[3], std::string &why)
{
    NmbOfflineComputePipelineDescriptor p = {};
    std::vector<NmbOfflineFunction *> linked;
    std::vector<switch_linked_group_name> groupNames;
    if (!computeDescriptor(descriptor, p, linked, groupNames)) {
        why = "a function of another device, or what the frontend does not represent";
        return false;
    }
    if (nmb_offline_compute_pipeline_facts(&p, facts))
        return true;
    why = std::string("no compute prepare: ") + nmb_last_error();
    return false;
}

bool descriptorOf(MTLRenderPipelineDescriptor *d, NmbOfflineRenderPipelineDescriptor &p,
                  std::vector<NmbOfflineFunction *> &linked)
{
    return renderDescriptor(d, p, linked);
}

bool descriptorOf(MTLComputePipelineDescriptor *d, NmbOfflineComputePipelineDescriptor &p,
                  std::vector<NmbOfflineFunction *> &linked, std::vector<switch_linked_group_name> &groupNames)
{
    return computeDescriptor(d, p, linked, groupNames);
}

NmbOfflineLibrary *libraryHandle(id<MTLLibrary> library)
{
    return [(id)library isKindOfClass:[GM20BOfflineLibrary class]] ? ((GM20BOfflineLibrary *)library).handle : nullptr;
}

gm20b::DirectTranslator translator()
{
    gm20b::DirectTranslator t;
    t.render = render;
    t.compute = compute;
    return t;
}

}  // namespace offline
