// SPDX-License-Identifier: MIT
// See TegraGM20BMetalReflection.h: a blob's nodes as objects of one class per kind, as Metal's are. A binding answers
// MTLArgument's and the MTLBinding protocols' selectors and each type class its own, so a selector Metal's class lacks
// is unrecognized here too. A binding asked another binding type's selector (a texture's bufferAlignment) fails as
// Metal's MTLBindingInternal does: Metal's failed assertion, then 0, NO, nil or MTLTextureType2D.
#import "TegraGM20BMetalReflection.h"

#import <objc/runtime.h>
#include <iterator>
#include "reflection_blob.h"

// Metal's own failure report, exported by both builds' Metal and declared by no SDK header. A failed assertion aborts
// the process unless a report-failure block (MTLSetReportFailureBlock) or Metal's error mode for it says otherwise,
// and returns where they do.
extern "C" void MTLReportFailure(uint64_t type, const char *function, int line, NSString *format, ...)
    NS_FORMAT_FUNCTION(4, 5);
static constexpr uint64_t kFailedAssertion = 0;

@interface SwitchMetalReflectionNode : NSObject {
@public
    gm20b::ReflectionNode _node;
    NSArray *_members;
    id _children[4];
    NSString *_name;
}
@end

@implementation SwitchMetalReflectionNode

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

@end

@interface SwitchMetalReflectionBinding : SwitchMetalReflectionNode
@end

static bool ofType(SwitchMetalReflectionBinding *b, MTLBindingType type)
{
    return b->_node.values[0] == uint64_t(type);
}

@implementation SwitchMetalReflectionBinding

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
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
    return protocol_conformsToProtocol(binding, protocol) || [super conformsToProtocol:protocol];
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
    return _node.values[1];
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

- (NSUInteger)arrayLength
{
    return _node.values[7];
}

- (NSUInteger)bufferAlignment
{
    if (ofType(self, MTLBindingTypeBuffer))
        return _node.values[5];
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying buffer alignment on an argument that is not a buffer");
    return 0;
}

- (NSUInteger)bufferDataSize
{
    if (ofType(self, MTLBindingTypeBuffer))
        return _node.values[6];
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying buffer data size on an argument that is not a buffer");
    return 0;
}

- (MTLDataType)bufferDataType
{
    if (ofType(self, MTLBindingTypeBuffer))
        return MTLDataType(_node.values[3]);
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying buffer data type on an argument that is not a buffer");
    return MTLDataTypeNone;
}

- (id)bufferStructType
{
    if (ofType(self, MTLBindingTypeBuffer))
        return _children[0];
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying buffer struct type on an argument that is not a buffer");
    return nil;
}

- (id)bufferPointerType
{
    return _children[3];
}

- (id)structType
{
    return _children[0];
}

- (NSUInteger)threadgroupMemoryAlignment
{
    if (ofType(self, MTLBindingTypeThreadgroupMemory))
        return _node.values[5];
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying threadgroup memory alignment on an argument that is not a threadgroup memory");
    return 0;
}

- (NSUInteger)threadgroupMemoryDataSize
{
    if (ofType(self, MTLBindingTypeThreadgroupMemory))
        return _node.values[6];
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying threadgroup memory data size on an argument that is not a threadgroup memory");
    return 0;
}

- (MTLTextureType)textureType
{
    if (ofType(self, MTLBindingTypeTexture))
        return MTLTextureType(_node.values[5]);
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying texture type on an argument that is not a texture");
    return MTLTextureType2D;
}

- (MTLDataType)textureDataType
{
    if (ofType(self, MTLBindingTypeTexture))
        return MTLDataType(_node.values[6]);
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying texture data type on an argument that is not a texture");
    return MTLDataTypeNone;
}

- (BOOL)isDepthTexture
{
    if (ofType(self, MTLBindingTypeTexture))
        return (_node.flags & 4) != 0;
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying isDepthTexture on an argument that is not a texture");
    return NO;
}

// No state of this device has an object or mesh stage (it refuses mesh pipelines), so no binding is an object payload
- (NSUInteger)objectPayloadAlignment
{
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying object payload alignment on an argument that is not an object payload");
    return 0;
}

- (NSUInteger)objectPayloadDataSize
{
    MTLReportFailure(kFailedAssertion, __func__, __LINE__,
                     @"Querying object payload size on an argument that is not an object payload");
    return 0;
}

@end

@interface SwitchMetalReflectionStructType : SwitchMetalReflectionNode
@end

@implementation SwitchMetalReflectionStructType

- (MTLDataType)dataType
{
    return MTLDataTypeStruct;
}

- (NSArray *)members
{
    return _members;
}

// The last of the members of that name, as Metal answers among anonymous members (several named "")
- (id)memberByName:(NSString *)name
{
    for (SwitchMetalReflectionNode *member in [_members reverseObjectEnumerator])
        if ([member->_name isEqualToString:name])
            return member;
    return nil;
}

@end

@interface SwitchMetalReflectionMember : SwitchMetalReflectionNode
@end

@implementation SwitchMetalReflectionMember

- (NSString *)name
{
    return _name;
}

- (MTLDataType)dataType
{
    return MTLDataType(_node.values[0]);
}

- (NSUInteger)offset
{
    return _node.values[1];
}

- (NSUInteger)argumentIndex
{
    return _node.values[2];
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

@end

@interface SwitchMetalReflectionArrayType : SwitchMetalReflectionNode
@end

@implementation SwitchMetalReflectionArrayType

- (MTLDataType)dataType
{
    return MTLDataTypeArray;
}

- (MTLDataType)elementType
{
    return MTLDataType(_node.values[0]);
}

- (NSUInteger)arrayLength
{
    return _node.values[1];
}

- (NSUInteger)stride
{
    return _node.values[2];
}

- (NSUInteger)argumentIndexStride
{
    return _node.values[3];
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

@interface SwitchMetalReflectionPointerType : SwitchMetalReflectionNode
@end

@implementation SwitchMetalReflectionPointerType

- (MTLDataType)dataType
{
    return MTLDataTypePointer;
}

- (MTLDataType)elementType
{
    return MTLDataType(_node.values[0]);
}

- (NSUInteger)access
{
    return _node.values[1];
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

- (id)elementStructType
{
    return _children[0];
}

- (id)elementArrayType
{
    return _children[1];
}

- (id)structType
{
    return _children[0];
}

@end

@interface SwitchMetalReflectionTextureType : SwitchMetalReflectionNode
@end

@implementation SwitchMetalReflectionTextureType

- (MTLDataType)dataType
{
    return MTLDataTypeTexture;
}

- (MTLDataType)textureDataType
{
    return MTLDataType(_node.values[0]);
}

- (MTLTextureType)textureType
{
    return MTLTextureType(_node.values[1]);
}

- (NSUInteger)access
{
    return _node.values[2];
}

- (BOOL)isDepthTexture
{
    return _node.flags & 1;
}

@end

@interface SwitchMetalComputeReflection : NSObject
@property (nonatomic, copy) NSArray *bindings;
@end

@implementation SwitchMetalComputeReflection

- (BOOL)isKindOfClass:(Class)cls
{
    return [[MTLComputePipelineReflection class] isSubclassOfClass:cls] || [super isKindOfClass:cls];
}

- (NSArray *)arguments
{
    return _bindings;
}

@end

@interface SwitchMetalRenderReflection : NSObject
@property (nonatomic, copy) NSArray *vertexBindings, *fragmentBindings, *tileBindings, *objectBindings, *meshBindings;
@end

@implementation SwitchMetalRenderReflection

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

id gm20bReflectionOf(const uint8_t *bytes, size_t length, bool compute)
{
    gm20b::ReflectionView v;
    if (!gm20b::readReflection(bytes, length,
                               compute ? gm20b::kReflectionComputeStages : gm20b::kReflectionRenderStages, v))
        return nil;
    static const Class kinds[] = {Nil,
                                  [SwitchMetalReflectionBinding class],
                                  [SwitchMetalReflectionStructType class],
                                  [SwitchMetalReflectionMember class],
                                  [SwitchMetalReflectionArrayType class],
                                  [SwitchMetalReflectionPointerType class],
                                  [SwitchMetalReflectionTextureType class]};
    NSMutableArray<SwitchMetalReflectionNode *> *objects = [NSMutableArray arrayWithCapacity:v.header->nodes];
    for (uint32_t i = 0; i < v.header->nodes; i++) {
        SwitchMetalReflectionNode *value = [kinds[v.nodes[i].kind] new];
        value->_node = v.nodes[i];
        const char *name = v.name(v.nodes[i]);
        value->_name = name ? @(name) : nil;
        for (uint32_t c = 0; c < 4; c++)
            if (v.nodes[i].children[c] != gm20b::kReflectionAbsent)
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
        SwitchMetalComputeReflection *r = [SwitchMetalComputeReflection new];
        r.bindings = stages[0];
        return r;
    }
    SwitchMetalRenderReflection *r = [SwitchMetalRenderReflection new];
    r.vertexBindings = stages[0];
    r.fragmentBindings = stages[1];
    r.tileBindings = stages[2];
    r.objectBindings = stages[3];
    r.meshBindings = stages[4];
    return r;
}
