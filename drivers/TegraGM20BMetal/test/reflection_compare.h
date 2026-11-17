// SPDX-License-Identifier: MIT
// Two reflection objects compared selector by selector, recursively: every answer Metal's reflection classes give
// (MTLArgument, MTLBinding and its kinds, MTLStructType, MTLStructMember, MTLArrayType, MTLPointerType,
// MTLTextureReferenceType), the m2v frontend's against the raw facade's.
#pragma once
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <gtest/gtest.h>
#include <string>

namespace reflection_compare {

inline constexpr const char *kCounts[] = {"type",
                                          "access",
                                          "index",
                                          "bufferAlignment",
                                          "bufferDataSize",
                                          "bufferDataType",
                                          "threadgroupMemoryAlignment",
                                          "threadgroupMemoryDataSize",
                                          "textureType",
                                          "textureDataType",
                                          "arrayLength",
                                          "dataType",
                                          "offset",
                                          "argumentIndex",
                                          "elementType",
                                          "stride",
                                          "argumentIndexStride",
                                          "alignment",
                                          "dataSize"};
inline constexpr const char *kFlags[] = {"isUsed", "isActive", "isArgument", "isDepthTexture",
                                         "elementIsArgumentBuffer"};
inline constexpr const char *kObjects[] = {"bufferStructType",  "bufferPointerType",    "structType",
                                           "arrayType",         "textureReferenceType", "pointerType",
                                           "elementStructType", "elementArrayType",     "elementTextureReferenceType",
                                           "elementPointerType"};

inline void expectSame(id m2v, id raw, const std::string &path);

inline void expectSameList(NSArray *m2v, NSArray *raw, const std::string &path)
{
    ASSERT_EQ(m2v.count, raw.count) << path;
    for (NSUInteger i = 0; i < m2v.count; i++)
        expectSame(m2v[i], raw[i], path + "[" + std::to_string(i) + "]");
}

inline void expectSame(id m2v, id raw, const std::string &path)
{
    ASSERT_EQ(m2v == nil, raw == nil) << path;
    if (!m2v)
        return;
    EXPECT_TRUE([[m2v name] isEqual:[raw name]] || (![m2v name] && ![raw name])) << path;
    for (const char *s : kCounts)
        EXPECT_EQ(((NSUInteger (*)(id, SEL))objc_msgSend)(m2v, sel_registerName(s)),
                  ((NSUInteger (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)))
            << path << " " << s;
    for (const char *s : kFlags)
        EXPECT_EQ(((BOOL (*)(id, SEL))objc_msgSend)(m2v, sel_registerName(s)),
                  ((BOOL (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)))
            << path << " " << s;
    for (Class c : {[MTLArgument class],
                    [MTLStructType class],
                    [MTLStructMember class],
                    [MTLArrayType class],
                    [MTLPointerType class],
                    [MTLTextureReferenceType class],
                    [MTLType class]})
        EXPECT_EQ([m2v isKindOfClass:c], [raw isKindOfClass:c]) << path << " " << class_getName(c);
    for (Protocol *p : {
             @protocol (MTLBinding), @protocol (MTLBufferBinding), @protocol (MTLTextureBinding),
                 @protocol (MTLThreadgroupBinding)
         })
        EXPECT_EQ([m2v conformsToProtocol:p], [raw conformsToProtocol:p]) << path << " " << protocol_getName(p);
    for (const char *s : kObjects)
        expectSame(((id (*)(id, SEL))objc_msgSend)(m2v, sel_registerName(s)),
                   ((id (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)), path + "." + s);
    expectSameList([m2v members], [raw members], path + ".members");
    for (id member in [m2v members])
        expectSame([m2v memberByName:[member name]], [raw memberByName:[member name]], path + ".memberByName");
}

}  // namespace reflection_compare
