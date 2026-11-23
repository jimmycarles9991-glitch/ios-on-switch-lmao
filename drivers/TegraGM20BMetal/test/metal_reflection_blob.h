// SPDX-License-Identifier: MIT
// This Mac's Metal's own reflection of a state as a blob (reflection_blob.h), each binding and type
// laid out as translate/NativeMetalReflection.rs lays out m2v's: what the facade's objects read back must answer as
// Metal did. Stages are added in the blob's order (compute: one; render: vertex, fragment, tile, object, mesh).
#pragma once
#import <Metal/Metal.h>
#import <objc/message.h>
#include <algorithm>
#include <deque>
#include <iterator>
#include <string>
#include <vector>
#include "reflection_blob_writer.h"

namespace metal_reflection {

class Table {
public:
    void stage(NSArray<id<MTLBinding>> *bindings)
    {
        std::vector<uint32_t> roots;
        for (id<MTLBinding> b in bindings)
            roots.push_back(binding(b));
        stages_.push_back({uint32_t(edges_.size()), uint32_t(roots.size())});
        edges_.insert(edges_.end(), roots.begin(), roots.end());
    }

    std::string blob() const
    {
        return reflection_blob::blob(nodes_, edges_, stages_);
    }

private:
    std::vector<NmbReflectionNode> nodes_;
    std::vector<uint32_t> edges_;
    std::vector<gm20b::ReflectionRange> stages_;
    std::deque<std::string> names_;

    NmbReflectionNode node(uint32_t kind, NSString *name)
    {
        NmbReflectionNode n = {};
        n.kind = kind;
        std::fill(std::begin(n.children), std::end(n.children), gm20b::kReflectionAbsent);
        if (name)
            n.name = names_.emplace_back(name.UTF8String).c_str();
        return n;
    }

    uint32_t push(const NmbReflectionNode &n)
    {
        nodes_.push_back(n);
        return uint32_t(nodes_.size() - 1);
    }

    uint32_t binding(id<MTLBinding> b)
    {
        NmbReflectionNode n = node(gm20b::ReflectionBinding, b.name);
        n.flags = uint32_t([b isUsed]) | uint32_t([b isArgument]) << 1;
        n.values[0] = b.type;
        n.values[1] = b.access;
        n.values[2] = b.index;
        n.values[7] = ((NSUInteger (*)(id, SEL))objc_msgSend)(b, @selector(arrayLength));
        if (b.type == MTLBindingTypeBuffer) {
            id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)b;
            n.values[3] = buffer.bufferDataType;
            n.values[5] = buffer.bufferAlignment;
            n.values[6] = buffer.bufferDataSize;
            n.children[0] = structure(buffer.bufferStructType);
            n.children[3] = pointer(buffer.bufferPointerType);
        } else if (b.type == MTLBindingTypeThreadgroupMemory) {
            id<MTLThreadgroupBinding> memory = (id<MTLThreadgroupBinding>)b;
            n.values[5] = memory.threadgroupMemoryAlignment;
            n.values[6] = memory.threadgroupMemoryDataSize;
        } else if (b.type == MTLBindingTypeTexture) {
            id<MTLTextureBinding> texture = (id<MTLTextureBinding>)b;
            n.values[5] = texture.textureType;
            n.values[6] = texture.textureDataType;
            n.flags |= uint32_t([texture isDepthTexture]) << 2;
        }
        return push(n);
    }

    uint32_t structure(MTLStructType *s)
    {
        if (!s)
            return gm20b::kReflectionAbsent;
        std::vector<uint32_t> members;
        for (MTLStructMember *m in s.members)
            members.push_back(member(m));
        NmbReflectionNode n = node(gm20b::ReflectionStruct, nil);
        n.first = uint32_t(edges_.size());
        n.count = uint32_t(members.size());
        edges_.insert(edges_.end(), members.begin(), members.end());
        return push(n);
    }

    uint32_t member(MTLStructMember *m)
    {
        NmbReflectionNode n = node(gm20b::ReflectionMember, m.name);
        n.values[0] = m.dataType;
        n.values[1] = m.offset;
        n.values[2] = m.argumentIndex;
        n.children[0] = structure(m.structType);
        n.children[1] = array(m.arrayType);
        n.children[2] = texture(m.textureReferenceType);
        n.children[3] = pointer(m.pointerType);
        return push(n);
    }

    uint32_t array(MTLArrayType *a)
    {
        if (!a)
            return gm20b::kReflectionAbsent;
        NmbReflectionNode n = node(gm20b::ReflectionArray, nil);
        n.values[0] = a.elementType;
        n.values[1] = a.arrayLength;
        n.values[2] = a.stride;
        n.values[3] = a.argumentIndexStride;
        n.children[0] = structure(a.elementStructType);
        n.children[1] = array(a.elementArrayType);
        n.children[2] = texture(a.elementTextureReferenceType);
        n.children[3] = pointer(a.elementPointerType);
        return push(n);
    }

    uint32_t pointer(MTLPointerType *p)
    {
        if (!p)
            return gm20b::kReflectionAbsent;
        NmbReflectionNode n = node(gm20b::ReflectionPointer, nil);
        n.values[0] = p.elementType;
        n.values[1] = p.access;
        n.values[2] = p.alignment;
        n.values[3] = p.dataSize;
        n.flags = p.elementIsArgumentBuffer;
        n.children[0] = structure(p.elementStructType);
        n.children[1] = array(p.elementArrayType);
        return push(n);
    }

    uint32_t texture(MTLTextureReferenceType *t)
    {
        if (!t)
            return gm20b::kReflectionAbsent;
        NmbReflectionNode n = node(gm20b::ReflectionTexture, nil);
        n.values[0] = t.textureDataType;
        n.values[1] = t.textureType;
        n.values[2] = t.access;
        n.flags = [t isDepthTexture];
        return push(n);
    }
};

}  // namespace metal_reflection
