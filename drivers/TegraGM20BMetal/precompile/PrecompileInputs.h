// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace precompile {

static std::vector<std::string> split(const std::string &s, const std::string &by = " ")
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find(by, at);
        if (e == std::string::npos)
            e = s.size();
        if (e > at)
            out.push_back(s.substr(at, e - at));
        at = e + by.size();
    }
    return out;
}

// "name:type:hex" or "#index:type:hex" items (the journal's constants text) into constant values.
static MTLFunctionConstantValues *constants(const std::vector<std::string> &items)
{
    MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
    for (const std::string &item : items) {
        size_t a = item.rfind(':'), b = a == std::string::npos ? a : item.rfind(':', a - 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        std::string name = item.substr(0, b), type = item.substr(b + 1, a - b - 1), hex = item.substr(a + 1);
        uint8_t bytes[64] = {};
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < sizeof bytes; i += 2)
            bytes[i / 2] = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
        if (name[0] == '#')
            [v setConstantValue:bytes
                           type:(MTLDataType)atoi(type.c_str())
                        atIndex:strtoul(name.c_str() + 1, nullptr, 10)];
        else
            [v setConstantValue:bytes type:(MTLDataType)atoi(type.c_str()) withName:@(name.c_str())];
    }
    return v;
}

static const std::string kQuartzCore = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
static std::string gStore, gQuartzCore;
static id<MTLDevice> gDevice;

// A journal token with its %XX escapes undone (the frontend's journalToken).
static std::string unescape(const std::string &token)
{
    std::string out;
    for (size_t i = 0; i < token.size(); i++)
        if (token[i] == '%' && i + 2 < token.size()) {
            out += (char)strtoul(token.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else
            out += token[i];
    return out;
}

// The host file of the library the journal names `id` ("" is QuartzCore's).
static std::string hostFile(const std::string &id)
{
    std::string path = unescape(id);
    if (path.rfind("/System/Cryptexes/OS/", 0) == 0)
        path = path.substr(20);
    if (path.rfind("/var/", 0) == 0)
        path = "/private" + path;  // the store keeps the Data volume's paths as /private/var
    if (path.empty() || path == kQuartzCore)
        return gQuartzCore;
    if (path.rfind("data:", 0) == 0)
        return gStore + "/data/" + path.substr(5) + ".metallib";
    return gStore + "/root" + path;
}

struct Library {
    id<MTLLibrary> library;
    std::set<std::string> names;
};

static std::map<std::string, Library> gLibraries;  // by host file; nil library: none there, or the frontend refused it

static Library &library(const std::string &file)
{
    auto found = gLibraries.find(file);
    if (found != gLibraries.end())
        return found->second;
    Library &l = gLibraries[file];
    NSError *error = nil;
    if (access(file.c_str(), R_OK) == 0) {
        l.library = [gDevice newLibraryWithURL:[NSURL fileURLWithPath:@(file.c_str())] error:&error];
        if (!l.library)
            fprintf(stderr, "library %s: %s\n", file.c_str(), error.localizedDescription.UTF8String);
        for (NSString *name in l.library.functionNames)
            l.names.insert(name.UTF8String);
    }
    return l;
}

// A stage as the journal writes it (the frontend's stageText): the name, lib=<library>, "spec" for a specialization with no
// constants, then the constants.
struct Stage {
    std::string name, file, library;
    std::string as;  // the specialized name it was made under (as=)
    bool specialized = false;
    std::vector<std::string> constants;
};

static Stage stage(const std::string &text)
{
    Stage s;
    std::vector<std::string> tokens = split(text);
    if (tokens.empty())
        return s;
    s.name = tokens[0];
    std::string id;
    for (size_t i = 1; i < tokens.size(); i++)
        if (tokens[i].rfind("lib=", 0) == 0)
            id = tokens[i].substr(4);
        else if (tokens[i].rfind("as=", 0) == 0)
            s.as = tokens[i].substr(3);
        else if (tokens[i] == "spec")
            s.specialized = true;
        else
            s.constants.push_back(tokens[i]);
    s.library = unescape(id);
    s.file = hostFile(id);
    s.specialized = s.specialized || !s.constants.empty();
    return s;
}

// What became of a line, counted per library (the vertex function's, the kernel's) and kind.
enum Outcome {
    Made,
    Refused,
    NoLibrary,
    NoFunction
};

// The stage's function: newFunctionWithName: where the journal gives no constants and no "spec", else
// newFunctionWithName:constantValues:error: with exactly its constants. Nil with `outcome` when it cannot be made.
static id<MTLFunction> function(const Stage &s, Outcome &outcome, NSError **error)
{
    Library &l = library(s.file);
    if (!l.library) {
        outcome = NoLibrary;
        return nil;
    }
    if (!l.names.count(s.name)) {
        outcome = NoFunction;
        return nil;
    }
    id<MTLFunction> f = nil;
    if (!s.as.empty()) {
        MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
        d.name = @(s.name.c_str());
        d.specializedName = @(s.as.c_str());
        d.constantValues = constants(s.constants);
        f = [l.library newFunctionWithDescriptor:d error:error];
    } else
        f = s.specialized
                ? [l.library newFunctionWithName:@(s.name.c_str()) constantValues:constants(s.constants) error:error]
                : [l.library newFunctionWithName:@(s.name.c_str())];
    outcome = f ? Made : Refused;
    return f;
}

// The functions an R line's state links, after its stages (" | <vl|vp|fl|fp> <stage>", the frontend's linkedText), into `d`
static Outcome linkedStages(const std::vector<std::string> &parts, MTLRenderPipelineDescriptor *d, NSError **error,
                            std::vector<std::pair<std::string, Stage>> *resolved = nullptr)
{
    for (size_t i = 3; i < parts.size(); i++) {
        const std::string tag = parts[i].substr(0, 3);
        if (tag != "vl " && tag != "vp " && tag != "fl " && tag != "fp ")
            continue;
        Outcome outcome = Made;
        const Stage source = stage(parts[i].substr(3));
        id<MTLFunction> f = function(source, outcome, error);
        if (outcome != Made)
            return outcome;
        if (resolved)
            resolved->emplace_back(tag.substr(0, 2), source);
        MTLLinkedFunctions *l = tag[0] == 'v' ? d.vertexLinkedFunctions : d.fragmentLinkedFunctions;
        if (tag[1] == 'l')
            l.functions = [(l.functions ?: @[]) arrayByAddingObject:f];
        else
            l.privateFunctions = [(l.privateFunctions ?: @[]) arrayByAddingObject:f];
    }
    return Made;
}

struct RenderInput {
    MTLRenderPipelineDescriptor *descriptor = nil;
    MTLPipelineOption options = MTLPipelineOptionNone;
    std::string file, label;
    Stage vertex, fragment;
    std::vector<std::pair<std::string, Stage>> linked;
};

// An R line's descriptor (frontend journalRenderPipeline): samples, topology class, depth/stencil formats, options, label, color
// attachments (format:writeMask:blend:srcRGB:dstRGB:opRGB:srcA:dstA:opA), vertex attributes (format:offset:buffer:stride:step:rate),
// then both functions and the functions they link. The outcome, counted under the vertex function's library.
static Outcome renderInput(const std::string &line, RenderInput &out, NSError **why)
{
    out = {};
    auto &file = out.file;
    auto &label = out.label;
    auto &d = out.descriptor;
    auto &options = out.options;
    std::vector<std::string> parts = split(line, " | ");
    if (parts.size() < 3 || parts[1].rfind("v ", 0) != 0 || parts[2].rfind("f ", 0) != 0)
        return Refused;
    Stage v = stage(parts[1].substr(2)), f = stage(parts[2].substr(2));
    out.vertex = v;
    out.fragment = f;
    file = v.file;
    d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    options = MTLPipelineOptionNone;
    for (const std::string &h : split(parts[0])) {
        if (h.rfind("label=", 0) == 0) {
            label = h.substr(6);
            if (label != "-")
                d.label = @(label.c_str());
        } else if (h.rfind("samples=", 0) == 0)
            d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h.rfind("topo=", 0) == 0)
            d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
        else if (h.rfind("opt=", 0) == 0)
            options = (MTLPipelineOption)strtoull(h.c_str() + 4, nullptr, 10);
        else if (h.rfind("ds=", 0) == 0) {
            unsigned a = 0, b = 0;
            sscanf(h.c_str() + 3, "%u/%u", &a, &b);
            d.depthAttachmentPixelFormat = (MTLPixelFormat)a;
            d.stencilAttachmentPixelFormat = (MTLPixelFormat)b;
        } else if (h[0] == 'c' && isdigit(h[1])) {
            unsigned i, x[9] = {};
            if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6,
                       x + 7, x + 8) != 10 ||
                i >= 8)
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)x[0];
            c.writeMask = x[1];
            c.blendingEnabled = x[2];
            c.sourceRGBBlendFactor = (MTLBlendFactor)x[3];
            c.destinationRGBBlendFactor = (MTLBlendFactor)x[4];
            c.rgbBlendOperation = (MTLBlendOperation)x[5];
            c.sourceAlphaBlendFactor = (MTLBlendFactor)x[6];
            c.destinationAlphaBlendFactor = (MTLBlendFactor)x[7];
            c.alphaBlendOperation = (MTLBlendOperation)x[8];
        } else if (h[0] == 'a' && isdigit(h[1])) {
            unsigned i, x[6] = {};
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7 || i >= 31 ||
                x[2] >= 31)
                continue;
            if (!vd)
                vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3];
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
        }
    }
    d.vertexDescriptor = vd;
    Outcome outcome = Made;
    NSError *error = nil;
    d.vertexFunction = function(v, outcome, &error);
    if (f.name != "-" && outcome == Made)
        d.fragmentFunction = function(f, outcome, &error);
    if (outcome == Made)
        outcome = linkedStages(parts, d, &error, &out.linked);
    if (outcome != Made) {
        *why = error;
        return outcome;
    }
    return Made;
}

}  // namespace precompile
