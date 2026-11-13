// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include "bridge/NativeMetalComputeArtifact.h"

@interface NBReflectionValue : NSObject {
@public
    NmbReflectionNode _node;
    NSArray *_members;
    id _children[4];
    NSString *_name;
}
@end

@implementation NBReflectionValue

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    if (_node.kind == 1) {
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
    Class reflected = Nil;
    switch (_node.kind) {
    case 1:
        reflected = [MTLArgument class];
        break;
    case 2:
        reflected = [MTLStructType class];
        break;
    case 3:
        reflected = [MTLStructMember class];
        break;
    case 4:
        reflected = [MTLArrayType class];
        break;
    case 5:
        reflected = [MTLPointerType class];
        break;
    case 6:
        reflected = [MTLTextureReferenceType class];
        break;
    }
    return (reflected && [reflected isSubclassOfClass:cls]) || [super isKindOfClass:cls];
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
    return _node.values[_node.kind == 6 ? 2 : 1];
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
    return MTLTextureType(_node.values[_node.kind == 6 ? 1 : 5]);
}

- (MTLDataType)textureDataType
{
    return MTLDataType(_node.values[_node.kind == 6 ? 0 : 6]);
}

- (BOOL)isDepthTexture
{
    return _node.flags & (_node.kind == 6 ? 1 : 4);
}

- (NSUInteger)arrayLength
{
    return _node.values[_node.kind == 4 ? 1 : 7];
}

- (MTLDataType)dataType
{
    switch (_node.kind) {
    case 2:
        return MTLDataTypeStruct;
    case 4:
        return MTLDataTypeArray;
    case 5:
        return MTLDataTypePointer;
    case 6:
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
    for (NBReflectionValue *member in _members)
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

@interface NBComputeReflection : NSObject
@property (nonatomic, copy) NSArray *bindings;
@end
@implementation NBComputeReflection

- (BOOL)isKindOfClass:(Class)cls
{
    return [[MTLComputePipelineReflection class] isSubclassOfClass:cls] || [super isKindOfClass:cls];
}

- (NSArray *)arguments
{
    return _bindings;
}

@end

static id computeReflection(NmbComputeArtifact *artifact)
{
    const NmbReflectionNode *nodes;
    const uint32_t *edges;
    size_t count, edgeCount;
    if (!nmb_compute_artifact_reflection(artifact, &nodes, &count, &edges, &edgeCount) || !count)
        return nil;
    NSMutableArray *objects = [NSMutableArray arrayWithCapacity:count];
    for (size_t i = 0; i < count; ++i) {
        NBReflectionValue *value = [NBReflectionValue new];
        value->_node = nodes[i];
        value->_name = nodes[i].name ? @(nodes[i].name) : nil;
        value->_node.name = nullptr;
        [objects addObject:value];
    }
    for (size_t i = 0; i < count; ++i) {
        NBReflectionValue *value = objects[i];
        const auto &node = nodes[i];
        for (unsigned c = 0; c < 4; ++c) {
            if (node.children[c] != UINT32_MAX) {
                if (node.children[c] >= count)
                    return nil;
                value->_children[c] = objects[node.children[c]];
            }
        }
        if (node.first > edgeCount || node.count > edgeCount - node.first)
            return nil;
        NSMutableArray *members = [NSMutableArray arrayWithCapacity:node.count];
        for (size_t e = node.first; e < node.first + node.count; ++e) {
            if (edges[e] >= count)
                return nil;
            [members addObject:objects[edges[e]]];
        }
        value->_members = members;
    }
    NBComputeReflection *reflection = [NBComputeReflection new];
    reflection.bindings = ((NBReflectionValue *)objects[0])->_members;
    return reflection;
}
