// SPDX-License-Identifier: MIT
// See RawMetalReflection.h. A value answers every reflection class's selectors from its node, as the m2v frontend's
// NBReflectionValue (NativeComputeReflection.h) answered them, which reflection_values_test holds it to.
#import "RawMetalReflection.h"
#import "RawMetalComputeState.h"
#import "RawMetalInternal.h"

#import <objc/runtime.h>
#include <iterator>
#include "raw_compute_key.h"
#include "raw_reflection_blob.h"

@interface RMReflectionValue : NSObject {
@public
    raw::ReflectionNode _node;
    NSArray *_members;
    id _children[4];
    NSString *_name;
}
@end

@implementation RMReflectionValue

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    if (_node.kind == raw::ReflectionBinding) {
        Protocol *binding = @protocol(MTLBinding);
        switch (_node.values[0]) {
        case MTLBindingTypeBuffer:
            binding = @protocol(MTLBufferBinding);
            break;
        case MTLBindingTypeTexture:
            binding = @protocol(MTLTextureBinding);
            break;
        case MTLBindingTypeThreadgroupMemory:
            binding = @protocol(MTLThreadgroupBinding);
            break;
        }
        if (protocol_conformsToProtocol(binding, protocol))
            return YES;
    }
    return [super conformsToProtocol:protocol];
}

- (BOOL)isKindOfClass:(Class)cls
{
    static const Class reflected[] = {Nil,
                                      [MTLArgument class],
                                      [MTLStructType class],
                                      [MTLStructMember class],
                                      [MTLArrayType class],
                                      [MTLPointerType class],
                                      [MTLTextureReferenceType class]};
    const Class kind = _node.kind < std::size(reflected) ? reflected[_node.kind] : Nil;
    return (kind && [kind isSubclassOfClass:cls]) || [super isKindOfClass:cls];
}

- (NSString *)name
{
    return _name;
}

- (NSUInteger)type
{
    return _node.values[0];
}

- (NSUInteger)access
{
    return _node.values[_node.kind == raw::ReflectionTexture ? 2 : 1];
}

- (NSUInteger)index
{
    return _node.values[2];
}

- (BOOL)isUsed
{
    return _node.flags & 1;
}

- (BOOL)isActive
{
    return [self isUsed];
}

- (BOOL)isArgument
{
    return _node.flags & 2;
}

- (NSUInteger)bufferAlignment
{
    return _node.values[5];
}

- (NSUInteger)bufferDataSize
{
    return _node.values[6];
}

- (MTLDataType)bufferDataType
{
    return MTLDataType(_node.values[7]);
}

- (id)bufferStructType
{
    return _children[0];
}

- (id)bufferPointerType
{
    return _children[3];
}

- (NSUInteger)threadgroupMemoryAlignment
{
    return _node.values[5];
}

- (NSUInteger)threadgroupMemoryDataSize
{
    return _node.values[6];
}

- (MTLTextureType)textureType
{
    return MTLTextureType(_node.values[_node.kind == raw::ReflectionTexture ? 1 : 5]);
}

- (MTLDataType)textureDataType
{
    return MTLDataType(_node.values[_node.kind == raw::ReflectionTexture ? 0 : 6]);
}

- (BOOL)isDepthTexture
{
    return _node.flags & (_node.kind == raw::ReflectionTexture ? 1 : 4);
}

- (NSUInteger)arrayLength
{
    return _node.values[_node.kind == raw::ReflectionArray ? 1 : 7];
}

- (MTLDataType)dataType
{
    switch (_node.kind) {
    case raw::ReflectionStruct:
        return MTLDataTypeStruct;
    case raw::ReflectionArray:
        return MTLDataTypeArray;
    case raw::ReflectionPointer:
        return MTLDataTypePointer;
    case raw::ReflectionTexture:
        return MTLDataTypeTexture;
    default:
        return MTLDataType(_node.values[0]);
    }
}

- (NSUInteger)offset
{
    return _node.values[1];
}

- (NSUInteger)argumentIndex
{
    return _node.values[2];
}

- (NSArray *)members
{
    return _members;
}

- (id)memberByName:(NSString *)name
{
    for (RMReflectionValue *member in _members)
        if ([member.name isEqualToString:name])
            return member;
    return nil;
}

- (MTLDataType)elementType
{
    return MTLDataType(_node.values[0]);
}

- (NSUInteger)stride
{
    return _node.values[2];
}

- (NSUInteger)argumentIndexStride
{
    return _node.values[3];
}

- (NSUInteger)alignment
{
    return _node.values[2];
}

- (NSUInteger)dataSize
{
    return _node.values[3];
}

- (BOOL)elementIsArgumentBuffer
{
    return _node.flags & 1;
}

- (id)structType
{
    return _children[0];
}

- (id)arrayType
{
    return _children[1];
}

- (id)textureReferenceType
{
    return _children[2];
}

- (id)pointerType
{
    return _children[3];
}

- (id)elementStructType
{
    return _children[0];
}

- (id)elementArrayType
{
    return _children[1];
}

- (id)elementTextureReferenceType
{
    return _children[2];
}

- (id)elementPointerType
{
    return _children[3];
}

@end

@interface RMComputeReflection : NSObject
@property (nonatomic, copy) NSArray *bindings;
@end

@implementation RMComputeReflection

- (BOOL)isKindOfClass:(Class)cls
{
    return [[MTLComputePipelineReflection class] isSubclassOfClass:cls] || [super isKindOfClass:cls];
}

- (NSArray *)arguments
{
    return _bindings;
}

@end

@interface RMRenderReflection : NSObject
@property (nonatomic, copy) NSArray *vertexBindings, *fragmentBindings, *tileBindings, *objectBindings, *meshBindings;
@end

@implementation RMRenderReflection

- (BOOL)isKindOfClass:(Class)cls
{
    return [[MTLRenderPipelineReflection class] isSubclassOfClass:cls] || [super isKindOfClass:cls];
}

- (NSArray *)vertexArguments
{
    return _vertexBindings;
}

- (NSArray *)fragmentArguments
{
    return _fragmentBindings;
}

- (NSArray *)tileArguments
{
    return _tileBindings;
}

@end

id rmReflectionOf(const uint8_t *bytes, size_t length, bool compute)
{
    raw::ReflectionView v;
    if (!raw::readReflection(bytes, length, compute ? raw::kReflectionComputeStages : raw::kReflectionRenderStages, v))
        return nil;
    NSMutableArray<RMReflectionValue *> *objects = [NSMutableArray arrayWithCapacity:v.header->nodes];
    for (uint32_t i = 0; i < v.header->nodes; i++) {
        RMReflectionValue *value = [RMReflectionValue new];
        value->_node = v.nodes[i];
        const char *name = v.name(v.nodes[i]);
        value->_name = name ? @(name) : nil;
        for (uint32_t c = 0; c < 4; c++)
            if (v.nodes[i].children[c] != raw::kReflectionAbsent)
                value->_children[c] = objects[v.nodes[i].children[c]];
        NSMutableArray *members = [NSMutableArray arrayWithCapacity:v.nodes[i].count];
        for (uint32_t e = 0; e < v.nodes[i].count; e++)
            [members addObject:objects[v.edges[v.nodes[i].first + e]]];
        value->_members = members;
        [objects addObject:value];
    }
    NSMutableArray<NSArray *> *stages = [NSMutableArray arrayWithCapacity:v.header->stages];
    for (uint32_t s = 0; s < v.header->stages; s++) {
        NSMutableArray *bindings = [NSMutableArray arrayWithCapacity:v.stages[s].count];
        for (uint32_t e = 0; e < v.stages[s].count; e++)
            [bindings addObject:objects[v.edges[v.stages[s].first + e]]];
        [stages addObject:bindings];
    }
    if (compute) {
        RMComputeReflection *r = [RMComputeReflection new];
        r.bindings = stages[0];
        return r;
    }
    RMRenderReflection *r = [RMRenderReflection new];
    r.vertexBindings = stages[0];
    r.fragmentBindings = stages[1];
    r.tileBindings = stages[2];
    r.objectBindings = stages[3];
    r.meshBindings = stages[4];
    return r;
}

static bool reflectionAsked(MTLPipelineOption o)
{
    return o & (MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo);
}

static id fromBlob(const std::string &blob, bool compute)
{
    return rmReflectionOf(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), compute);
}

MTLAutoreleasedRenderPipelineReflection rmRenderReflection(RMPipeline *p, MTLRenderPipelineDescriptor *d,
                                                           MTLPipelineOption o)
{
    if (!reflectionAsked(o))
        return nil;
    std::string blob, why = "a function of another device, which no request can name";
    if (p->_completable) {
        uint32_t length = 0;
        if (const uint8_t *packed = gRM->raw->pack().reflection(p->_packKey, false, length))
            return rmReflectionOf(packed, length, false);
        if (gRM->completion && gRM->completion->reflection(rmRecordAsk(p, d), blob, why))
            if (id made = fromBlob(blob, false))
                return made;
    }
    os_log_error(rawLog(), "raw: %{public}s: no reflection of a render state, none given: %{public}s (%{public}s)",
                 getprogname(), why.c_str(), d.label.UTF8String ?: "-");
    return nil;
}

bool rmComputeReflection(RMComputeState *s, MTLPipelineOption o, MTLAutoreleasedComputePipelineReflection *reflection,
                         NSError **error)
{
    *reflection = nil;
    if (!reflectionAsked(o))
        return true;
    if (!s->_keyed || !s->_function) {
        os_log_error(rawLog(),
                     "raw: %{public}s: no reflection of a compute state of functions not all raw's "
                     "(%{public}s)",
                     getprogname(), s->_direct.label.UTF8String ?: "-");
        return true;
    }
    const std::string key = raw::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth);
    uint32_t length = 0;
    std::string blob, why = "no completion";
    if (const uint8_t *packed = gRM->raw->pack().reflection(raw::Pack::fnv(key.data(), key.size()), true, length))
        *reflection = rmReflectionOf(packed, length, true);
    else if (gRM->completion && gRM->completion->reflection(s->_function, s->_direct, key, blob, why))
        *reflection = fromBlob(blob, true);
    if (*reflection)
        return true;
    os_log_error(rawLog(), "raw: %{public}s: no reflection of a compute state: %{public}s", getprogname(), why.c_str());
    if (error) {
        NSString *text = [NSString stringWithFormat:@"no reflection: %s", why.c_str()];
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorInternal
                                 userInfo:@{NSLocalizedDescriptionKey : text}];
    }
    return false;
}
