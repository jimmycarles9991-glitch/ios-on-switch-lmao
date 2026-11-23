// SPDX-License-Identifier: MIT
// Two reflection objects compared selector by selector, recursively: every answer Metal's reflection classes give
// (MTLBinding and its kinds, MTLStructType, MTLStructMember, MTLArrayType, MTLPointerType, MTLTextureReferenceType),
// Metal's against the facade's, and which of the classes' selectors each responds to. Each difference is collected
// with its state and path, and a failure prints them summarised, one line per path and selector.
#pragma once
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace reflection_compare {

// What Metal answers each kind of object: it fails an assertion on a binding asked another type's selector (a texture
// binding's bufferAlignment) and does not implement a type class's selectors on another type class. Probed on macOS 27.2
// per binding type and type class; the first object's kind picks the set, and raw is asked the same set.
struct Selectors {
    std::vector<const char *> counts, flags, objects;
    bool named = false, members = false;
};

// Every selector of the reflection classes' public API, MTLArgument's, the MTLBinding protocols' and the type classes':
// an object responds to those of its own class alone
inline const std::vector<std::string> &everySelector()
{
    static const std::vector<std::string> all = [] {
        std::istringstream names(
            "name type access index isUsed isActive isArgument arrayLength bufferAlignment "
            "bufferDataSize bufferDataType bufferStructType bufferPointerType structType "
            "threadgroupMemoryAlignment threadgroupMemoryDataSize textureType textureDataType "
            "isDepthTexture dataType offset argumentIndex arrayType textureReferenceType pointerType "
            "members memberByName: elementType stride argumentIndexStride elementStructType "
            "elementArrayType elementTextureReferenceType elementPointerType alignment dataSize "
            "elementIsArgumentBuffer objectPayloadAlignment objectPayloadDataSize");
        return std::vector<std::string>(std::istream_iterator<std::string>(names), {});
    }();
    return all;
}

inline Selectors selectorsOf(id metal)
{
    if ([metal conformsToProtocol:@protocol(MTLBinding)]) {
        Selectors s{{"type", "access", "index", "arrayLength"},
                    {"isUsed", "isActive", "isArgument"},
                    {"structType", "bufferPointerType"},
                    true};
        switch (((NSUInteger (*)(id, SEL))objc_msgSend)(metal, @selector(type))) {
        case MTLBindingTypeBuffer:
            s.counts.insert(s.counts.end(), {"bufferAlignment", "bufferDataSize", "bufferDataType"});
            s.objects.push_back("bufferStructType");
            break;
        case MTLBindingTypeTexture:
            s.counts.insert(s.counts.end(), {"textureType", "textureDataType"});
            s.flags.push_back("isDepthTexture");
            break;
        case MTLBindingTypeThreadgroupMemory:
            s.counts.insert(s.counts.end(), {"threadgroupMemoryAlignment", "threadgroupMemoryDataSize"});
            break;
        default:
            break;
        }
        return s;
    }
    if ([metal isKindOfClass:[MTLStructMember class]])
        return {{"offset", "dataType", "argumentIndex"},
                {},
                {"structType", "arrayType", "textureReferenceType", "pointerType"},
                true};
    if ([metal isKindOfClass:[MTLStructType class]])
        return {{"dataType"}, {}, {}, false, true};
    if ([metal isKindOfClass:[MTLArrayType class]])
        return {{"dataType", "elementType", "arrayLength", "stride", "argumentIndexStride"},
                {},
                {"elementStructType", "elementArrayType", "elementTextureReferenceType", "elementPointerType"}};
    if ([metal isKindOfClass:[MTLPointerType class]])
        return {{"dataType", "elementType", "access", "alignment", "dataSize"},
                {"elementIsArgumentBuffer"},
                {"elementStructType", "elementArrayType", "structType"}};
    if ([metal isKindOfClass:[MTLTextureReferenceType class]])
        return {{"dataType", "textureType", "textureDataType", "access"}, {"isDepthTexture"}, {}};
    return {};
}

// What a comparison judges. usedOneWay: a binding raw answers used and Metal unused is counted apart, not a difference.
// Metal answers isUsed and isActive of a render state after its own compiler has optimised the specialized functions,
// which can drop a read the specialized AIR still makes; raw answers a binding used where the specialized AIR reads it.
// The other way, a binding Metal uses and raw does not, stays a difference.
struct Options {
    bool usedOneWay = false;
};

// One answer raw gives otherwise than Metal: the state, the node's path, what was asked and both answers
struct Difference {
    std::string state, path, what, metal, raw;
};

class Differences {
public:
    std::string state;
    std::vector<Difference> found, usedOnlyByRaw;

    void add(const std::string &path, const std::string &what, const std::string &metal, const std::string &raw)
    {
        found.push_back({state, path, what, metal, raw});
    }

    // One line per group of `list` (the node's path without its indices, and what was asked): its count, then its
    // first difference
    static std::string summary(const std::vector<Difference> &list)
    {
        std::vector<std::pair<std::string, std::vector<const Difference *>>> groups;
        for (const Difference &d : list) {
            const std::string key = withoutIndices(d.path) + " " + d.what;
            auto at = std::find_if(groups.begin(), groups.end(), [&](const auto &g) {
                return g.first == key;
            });
            if (at == groups.end())
                groups.push_back({key, {&d}});
            else
                at->second.push_back(&d);
        }
        std::ostringstream out;
        for (const auto &[key, members] : groups) {
            const Difference &d = *members.front();
            out << members.size() << " x " << key << ": first " << d.state << " " << d.path << " metal=" << d.metal
                << " raw=" << d.raw << "\n";
        }
        return out.str();
    }

private:
    static std::string withoutIndices(const std::string &path)
    {
        std::string out;
        for (size_t i = 0; i < path.size(); i++) {
            if (path[i] != '[') {
                out += path[i];
                continue;
            }
            out += "[]";
            i = path.find(']', i);
            if (i == std::string::npos)
                break;
        }
        return out;
    }
};

inline std::string number(NSUInteger n)
{
    return std::to_string((unsigned long long)n);
}

// The names of a list's objects, a struct member's with its offset and data type
inline std::string names(NSArray *list)
{
    std::string out;
    for (id x in list) {
        out += out.empty() ? "" : ",";
        out += [x name] ? [[x name] UTF8String] : "-";
        if ([x isKindOfClass:[MTLStructMember class]])
            out += ":" + number([(MTLStructMember *)x offset]) + ":" + number([(MTLStructMember *)x dataType]);
    }
    return "{" + out + "}";
}

inline void compare(id metal, id raw, const std::string &path, const Options &o, Differences &d);

inline void compareList(NSArray *metal, NSArray *raw, const std::string &path, const Options &o, Differences &d)
{
    if (metal.count != raw.count)
        return d.add(path, "count", number(metal.count) + " " + names(metal), number(raw.count) + " " + names(raw));
    for (NSUInteger i = 0; i < metal.count; i++)
        compare(metal[i], raw[i], path + "[" + std::to_string(i) + "]", o, d);
}

inline void compare(id metal, id raw, const std::string &path, const Options &o, Differences &d)
{
    if (!metal || !raw) {
        if (metal != raw)
            d.add(path, "presence", metal ? "an object" : "nil", raw ? "an object" : "nil");
        return;
    }
    for (Class c : {[MTLArgument class],
                    [MTLStructType class],
                    [MTLStructMember class],
                    [MTLArrayType class],
                    [MTLPointerType class],
                    [MTLTextureReferenceType class],
                    [MTLType class]})
        if ([metal isKindOfClass:c] != [raw isKindOfClass:c])
            d.add(path, std::string("isKindOfClass ") + class_getName(c), [metal isKindOfClass:c] ? "YES" : "NO",
                  [raw isKindOfClass:c] ? "YES" : "NO");
    for (Protocol *p : {
             @protocol (MTLBinding), @protocol (MTLBufferBinding), @protocol (MTLTextureBinding),
                 @protocol (MTLThreadgroupBinding)
         })
        if ([metal conformsToProtocol:p] != [raw conformsToProtocol:p])
            d.add(path, std::string("conformsToProtocol ") + protocol_getName(p),
                  [metal conformsToProtocol:p] ? "YES" : "NO", [raw conformsToProtocol:p] ? "YES" : "NO");
    for (const std::string &s : everySelector())
        if (const SEL sel = sel_registerName(s.c_str()); [metal respondsToSelector:sel] != [raw respondsToSelector:sel])
            d.add(path, "respondsToSelector " + s, [metal respondsToSelector:sel] ? "YES" : "NO",
                  [raw respondsToSelector:sel] ? "YES" : "NO");
    // A binding of another type than Metal's would fail Metal's assertion, ending the process, on its type's selectors
    const auto type = [](id binding) {
        return ((NSUInteger (*)(id, SEL))objc_msgSend)(binding, @selector(type));
    };
    if ([metal conformsToProtocol:@protocol(MTLBinding)] && type(metal) != type(raw))
        return d.add(path, "type", number(type(metal)), number(type(raw)));
    const Selectors selectors = selectorsOf(metal);
    if (selectors.named && ![[metal name] isEqual:[raw name]] && ([metal name] || [raw name]))
        d.add(path, "name", [metal name] ? [[metal name] UTF8String] : "nil",
              [raw name] ? [[raw name] UTF8String] : "nil");
    for (const char *s : selectors.counts) {
        const NSUInteger m = ((NSUInteger (*)(id, SEL))objc_msgSend)(metal, sel_registerName(s)),
                         r = ((NSUInteger (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s));
        if (m != r)
            d.add(path, s, number(m), number(r));
    }
    for (const char *s : selectors.flags) {
        const BOOL m = ((BOOL (*)(id, SEL))objc_msgSend)(metal, sel_registerName(s)),
                   r = ((BOOL (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s));
        if (m == r)
            continue;
        if (o.usedOneWay && !m && (!strcmp(s, "isUsed") || !strcmp(s, "isActive")))
            d.usedOnlyByRaw.push_back({d.state, path, s, "NO", "YES"});
        else
            d.add(path, s, m ? "YES" : "NO", r ? "YES" : "NO");
    }
    for (const char *s : selectors.objects)
        compare(((id (*)(id, SEL))objc_msgSend)(metal, sel_registerName(s)),
                ((id (*)(id, SEL))objc_msgSend)(raw, sel_registerName(s)), path + "." + s, o, d);
    if (!selectors.members)
        return;
    compareList([metal members], [raw members], path + ".members", o, d);
    for (id member in [metal members]) {
        const NSUInteger m = [[metal members] indexOfObject:[metal memberByName:[member name]]],
                         r = [[raw members] indexOfObject:[raw memberByName:[member name]]];
        if (m != r)
            d.add(path, std::string("memberByName ") + ([member name].UTF8String ?: "nil"), "member " + number(m),
                  r == NSNotFound ? "none" : "member " + number(r));
    }
}

// A render state's stages Metal reflects: its vertex, fragment and tile bindings
inline void compareRender(MTLRenderPipelineReflection *metal, id raw, const Options &o, Differences &d)
{
    compareList(metal.vertexBindings, [raw vertexBindings], "vertex", o, d);
    compareList(metal.fragmentBindings, [raw fragmentBindings], "fragment", o, d);
    compareList(metal.tileBindings, [raw tileBindings], "tile", o, d);
}

// Every difference between two lists of bindings, judged in full, as one test failure that summarises them
inline void expectSameList(NSArray *metal, NSArray *raw, const std::string &path)
{
    Differences d;
    compareList(metal, raw, path, {}, d);
    EXPECT_TRUE(d.found.empty()) << d.found.size() << " differences\n" << Differences::summary(d.found);
}

inline void expectSameRender(MTLRenderPipelineReflection *metal, id raw, const std::string &path)
{
    Differences d;
    d.state = path;
    compareRender(metal, raw, {}, d);
    EXPECT_TRUE(d.found.empty()) << d.found.size() << " differences\n" << Differences::summary(d.found);
}

}  // namespace reflection_compare
