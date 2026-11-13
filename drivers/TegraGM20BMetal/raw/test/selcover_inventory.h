// SPDX-License-Identifier: MIT
// selcover's API inventory (raw/test/inventory.tsv): every item m2v implements (its METAL-COVERAGE table,
// third_party/m2v/scripts/metal-coverage/coverage.tsv, status I) and every selector a kind sends, against raw's answer:
//   raw      a method of the facade's object (protocols; MTLResource on textures and buffers, MTLAllocation on heaps too,
//            MTLCommandEncoder on the three encoders: raw only when every one answers)
//   zero     the facade answers it zero, as m2v's object does (selcover's who())
//   m2v, m2vobj, base, none   as selcover's who()
//   read, unread   a class's property (descriptors, reflection): whether the raw runtime's sources name its getter
//   refused  raw/test/refused.txt names it, with the reason the families or the hardware give
// A row: container, item (the table's name; "-" for a sent selector the table lacks), selector, m2v ("I" or "-"), the kinds
// that send it ("-": none), raw's answer.
#pragma once
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace inventory {

struct Row {
    std::string container, item, selector, m2v, sent, raw;

    std::string line() const
    {
        return container + "\t" + item + "\t" + selector + "\t" + m2v + "\t" + (sent.empty() ? "-" : sent) + "\t" + raw;
    }
};

using Who = std::function<std::string(const std::string &protocol, SEL selector)>;

inline std::vector<std::vector<std::string>> tsv(const std::string &path)
{
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> fields;
        std::stringstream s(line);
        std::string field;
        while (std::getline(s, field, '\t'))
            fields.push_back(field);
        rows.push_back(fields);
    }
    return rows;
}

// A property's getter and, unless it is readonly, its setter, as the SDK's protocol or class declares them
inline std::vector<std::string> propertySelectors(const std::string &container, const std::string &name)
{
    objc_property_t p = nullptr;
    if (Protocol *protocol = objc_getProtocol(container.c_str()))
        for (int required = 1; required >= 0 && !p; --required)
            p = protocol_getProperty(protocol, name.c_str(), required, YES);
    else if (Class cls = objc_getClass(container.c_str()))
        p = class_getProperty(cls, name.c_str());
    std::string getter = name, setter = "set" + std::string(1, char(toupper(name[0]))) + name.substr(1) + ":";
    bool readonly = !p;
    if (p) {
        if (char *g = property_copyAttributeValue(p, "G")) {
            getter = g;
            free(g);
        }
        if (char *s = property_copyAttributeValue(p, "S")) {
            setter = s;
            free(s);
        }
        if (char *r = property_copyAttributeValue(p, "R")) {
            readonly = true;
            free(r);
        }
    }
    return readonly ? std::vector<std::string>{getter} : std::vector<std::string>{getter, setter};
}

// Every source file of the raw runtime, as one text (a class's property counts as read where its getter appears after a '.'
// or a blank and before a non-identifier character, or its own name after a '.': dot syntax of a getter named isX)
inline std::string runtimeText(const std::string &dir)
{
    std::string text;
    for (NSString *file in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(dir.c_str()) error:nil])
        if ([file hasSuffix:@".mm"] || [file hasSuffix:@".cpp"] || [file hasSuffix:@".h"])
            text += [NSString stringWithContentsOfFile:[@(dir.c_str()) stringByAppendingPathComponent:file]
                                              encoding:NSUTF8StringEncoding
                                                 error:nil]
                            .UTF8String
                        ?: "";
    return text;
}

inline bool named(const std::string &text, const std::string &getter, bool dotOnly = false)
{
    for (size_t at = text.find(getter); at != std::string::npos; at = text.find(getter, at + 1)) {
        const char before = at ? text[at - 1] : ' ',
                   after = at + getter.size() < text.size() ? text[at + getter.size()] : ' ';
        if ((before == '.' || (before == ' ' && !dotOnly)) && !isalnum(after) && after != '_' && after != ':')
            return true;
    }
    return false;
}

// The protocols whose items other objects' answers stand for: a resource's on the texture and buffer (and an allocation's on
// the heap too), an encoder's on the three encoders
inline std::vector<std::string> objectsOf(const std::string &protocol)
{
    if (protocol == "MTLResource")
        return {"MTLTexture", "MTLBuffer"};
    if (protocol == "MTLAllocation")
        return {"MTLTexture", "MTLBuffer", "MTLHeap"};
    if (protocol == "MTLCommandEncoder")
        return {"MTLRenderCommandEncoder", "MTLBlitCommandEncoder", "MTLComputeCommandEncoder"};
    return {protocol};
}

// The worst answer of several objects', by how far it is from raw's own
inline std::string worst(const std::vector<std::string> &answers)
{
    static const char *order[] = {"raw", "zero", "base", "m2v", "m2vobj", "none"};
    size_t at = 0;
    for (const std::string &a : answers)
        for (size_t i = 0; i < 6; i++)
            if (a == order[i] && i > at)
                at = i;
    return order[at];
}

// `sent`: (protocol, selector) -> the kinds that send it, comma-separated
inline std::vector<Row> make(const std::string &coverage, const std::string &refusedPath, const std::string &runtimeDir,
                             const std::map<std::pair<std::string, std::string>, std::string> &sent, const Who &who)
{
    std::set<std::pair<std::string, std::string>> refused;
    for (const auto &f : tsv(refusedPath))
        if (f.size() >= 3)
            refused.insert({f[0], f[1]});
    const std::string text = runtimeText(runtimeDir);
    std::map<std::tuple<std::string, std::string, std::string>, Row> rows;
    auto answer = [&](const std::string &container, const std::string &selector) {
        std::vector<std::string> answers;
        for (const std::string &p : objectsOf(container))
            answers.push_back(who(p, sel_registerName(selector.c_str())));
        return worst(answers);
    };
    for (const auto &f : tsv(coverage)) {
        if (f.size() < 6 || f[0] == "header" || f[5] != "I" || (f[2] != "method" && f[2] != "property"))
            continue;
        const std::string &container = f[1], &name = f[3];
        const bool protocol = objc_getProtocol(container.c_str()) != nullptr;
        const std::vector<std::string> selectors =
            f[2] == "method" ? std::vector<std::string>{name} : propertySelectors(container, name);
        for (const std::string &selector : selectors) {
            Row r{container, name, selector, "I", "", ""};
            if (refused.count({container, name}))
                r.raw = "refused";
            else if (protocol)
                r.raw = answer(container, selector);
            else
                r.raw = named(text, selectors[0]) || named(text, name, true) ? "read" : "unread";
            const auto s = sent.find({container, selector});
            r.sent = s == sent.end() ? "" : s->second;
            rows[{container, name, selector}] = r;
        }
    }
    for (const auto &[key, kinds] : sent) {
        bool listed = false;
        for (const auto &[k, r] : rows)
            listed |= r.container == key.first && r.selector == key.second;
        if (!listed)
            rows[{key.first, "-", key.second}] = Row{
                key.first, "-",   key.second,
                "-",       kinds, refused.count({key.first, key.second}) ? "refused" : answer(key.first, key.second)};
    }
    std::vector<Row> out;
    for (const auto &[key, r] : rows)
        out.push_back(r);
    return out;
}

// The deletion gate's count: an item m2v implements that raw neither answers, reads nor refuses, and a sent selector raw
// neither answers itself nor zero as m2v does
inline unsigned open(const std::vector<Row> &rows)
{
    unsigned n = 0;
    for (const Row &r : rows) {
        const bool ok = r.raw == "raw" || r.raw == "read" || r.raw == "refused" || (r.m2v == "-" && r.raw == "zero");
        n += !ok;
    }
    return n;
}

inline std::string totals(const std::vector<Row> &rows)
{
    std::map<std::string, unsigned> items, sent;
    for (const Row &r : rows) {
        if (r.m2v == "I")
            items[r.raw]++;
        if (!r.sent.empty())
            sent[r.raw]++;
    }
    std::string out = "# m2v's items:";
    for (const auto &[k, v] : items)
        out += " " + k + " " + std::to_string(v);
    out += "\n# sent selectors:";
    for (const auto &[k, v] : sent)
        out += " " + k + " " + std::to_string(v);
    return out + "\n";
}

}  // namespace inventory
