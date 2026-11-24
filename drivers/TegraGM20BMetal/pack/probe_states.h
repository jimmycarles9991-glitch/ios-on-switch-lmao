// SPDX-License-Identifier: MIT
// A corpus R line (the frontend journal's render pipeline state) as the descriptor it names, made with a library's functions: the
// pack generator's probe (probe.mm) and the host check of the direct compile's records (harness/directcheck.mm) read
// lines alike.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdlib>
#include <string>
#include <vector>
#import "../TegraGM20BMetalPipelineKey.h"
#include "../cubic.h"
#include "../written_textures.h"

namespace probe {

inline std::vector<std::string> split(const std::string &s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t e = s.find(' ', at);
        if (e == std::string::npos)
            e = s.size();
        if (e > at)
            out.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return out;
}

inline MTLFunctionConstantValues *constants(const std::vector<std::string> &items)
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

// `key`: the function as the runtime keys it (its library function and the text of the values it is made with)
inline id<MTLFunction> function(id<MTLLibrary> qc, const std::vector<std::string> &stage, gm20b::StageKey &key)
{
    if (stage.empty() || stage[0] == "-")
        return nil;
    key.function = stage[0];
    if (stage.size() == 1)
        return [qc newFunctionWithName:@(stage[0].c_str())];
    NSError *e = nil;
    MTLFunctionConstantValues *values = constants(std::vector<std::string>(stage.begin() + 1, stage.end()));
    key.constants = gm20b::constantsText(values);
    return [qc newFunctionWithName:@(stage[0].c_str()) constantValues:values error:&e];
}

inline id<MTLLibrary> frameworkLibrary(id<MTLDevice> dev, const std::string &path);

// One linked function of an R line ("<vl|vp|fl|fp> <function> [from=<library>] [as=<specialized name>] <constants>", the
// frontend's linkedText; corpusLines renames its lib= from=) added to its stage's list, made from the library from= names (`own`
// without one) under its specialized name; `linked` gets its segment as the driver keys it (gm20b::linkedSegment)
inline bool linkedFunction(MTLRenderPipelineDescriptor *d, id<MTLLibrary> own, const std::string &segment,
                           std::string &linked)
{
    const std::vector<std::string> tokens = split(segment);
    if (tokens.size() < 2)
        return false;
    MTLFunctionDescriptor *fd = [MTLFunctionDescriptor functionDescriptor];
    fd.name = @(tokens[1].c_str());
    id<MTLLibrary> library = own;
    std::vector<std::string> values;
    std::string name = tokens[1];
    for (size_t i = 2; i < tokens.size(); i++) {
        if (tokens[i].rfind("from=", 0) == 0)
            library = frameworkLibrary(own.device, tokens[i].substr(5));
        else if (tokens[i].rfind("as=", 0) == 0) {
            name = tokens[i].substr(3);
            fd.specializedName = @(name.c_str());
        } else
            values.push_back(tokens[i]);
    }
    gm20b::StageKey key{tokens[1], ""};
    if (!values.empty()) {
        fd.constantValues = constants(values);
        key.constants = gm20b::constantsText(fd.constantValues);
    }
    NSError *e = nil;
    id<MTLFunction> f = [library newFunctionWithDescriptor:fd error:&e];
    if (!f)
        return false;
    MTLLinkedFunctions *l = tokens[0][0] == 'v' ? d.vertexLinkedFunctions : d.fragmentLinkedFunctions;
    if (tokens[0][1] == 'l')
        l.functions = [(l.functions ?: @[]) arrayByAddingObject:f];
    else
        l.privateFunctions = [(l.privateFunctions ?: @[]) arrayByAddingObject:f];
    linked += gm20b::linkedSegment(tokens[0].c_str(), key, name);
    return true;
}

struct State {
    std::string key, label;
    uint64_t fast = 0;  // its fast key (TegraGM20BMetalPipelineKey.h fastKey), of its key less any variant name
    MTLRenderPipelineDescriptor *d;
    id<MTLLibrary> library = nil;  // the library its functions are made from
    std::string framework;         // that library's framework (parseState), "" for QuartzCore's and a bundle's
    unsigned vertexBuffer = 99, vertexStride = 0;
    gm20b::CubicBinds cubic;  // a line naming the state's cubic variant (" | cubic ...", cubic.h): its binds
    // a line naming the state's sampled two-plane variant (" | xrs=<slots>", xr_pass.h): its slots, which its key
    // leaves out (the runtime names the variant after the test class, TegraGM20BMetalCubic.mm)
    uint16_t xrSlots = 0;
    // a line naming the state's written variant (" | wtex=<slot>:<format>,...", written_textures.h): the formats bound at
    // the slots it writes, which its key leaves out as it does xrSlots
    std::vector<gm20b::WrittenTexture> written;
    // its line's " tier=1": a state the generator derived and no device named, laid after the launch set (pack.h
    // PackHeader::tier0); 0: one of the launch set
    uint64_t tier = 0;
};

// A line's " | cubic s<slot>:<s><t><r><border><n|p>... t<slot>:<MTLPixelFormat>..." segment (cubic.h key) as binds,
// a vertex stage's as "vs" and "vt"
inline bool cubicBinds(const std::string &segment, gm20b::CubicBinds &out)
{
    const std::vector<std::string> tokens = split(segment);
    for (size_t i = 1; i < tokens.size(); i++) {
        const std::string &t = tokens[i];
        const bool vertex = t[0] == 'v';
        const uint8_t stage = vertex ? gm20b::kCubicVertex : gm20b::kCubicFragment;
        const size_t colon = t.find(':'), at = vertex ? 1 : 0;
        if (colon == std::string::npos || colon < at + 2)
            return false;
        const uint8_t slot = uint8_t(atoi(t.c_str() + at + 1));
        const std::string value = t.substr(colon + 1);
        if (t[at] == 's' && value.size() == 5 && (value[4] == 'n' || value[4] == 'p'))
            out.samplers.push_back({slot, uint8_t(value[0] - '0'), uint8_t(value[1] - '0'), uint8_t(value[2] - '0'),
                                    uint8_t(value[3] - '0'), value[4] == 'n', stage});
        else if (t[at] == 't')
            out.textures.push_back({slot, uint32_t(strtoul(value.c_str(), nullptr, 10)), stage});
        else
            return false;
    }
    return !out.empty();
}

// precompile.mm's R-line parser (the frontend's journal format); the state's key is the one the runtime computes of the
// descriptor made (gm20b::pipelineKey), `framework` the library's (<name>.framework/default.metallib), "" for QuartzCore's.
// False with `why` naming what of the line made no state.
inline bool parseState(id<MTLLibrary> qc, const std::string &line, State &s, const std::string &framework = "",
                       std::string *why = nullptr)
{
    const auto refuse = [why](const std::string &reason) {
        if (why)
            *why = reason;
        return false;
    };
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "), bar3 = line.find(" | ", bar2 + 5);
    if (bar1 == std::string::npos || bar2 == std::string::npos)
        return refuse("no vertex or fragment segment");
    std::vector<std::string> head = split(line.substr(0, bar1));
    std::vector<std::string> v = split(line.substr(bar1 + 5, bar2 - bar1 - 5));
    std::vector<std::string> f =
        split(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5));
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    for (const std::string &h : head) {
        if (h.rfind("label=", 0) == 0) {
            s.label = h.substr(6);
            if (s.label != "-")
                d.label = @(s.label.c_str());
        } else if (h.rfind("samples=", 0) == 0)
            d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h == "rast=0")
            d.rasterizationEnabled = NO;
        else if (h.rfind("topo=", 0) == 0)
            d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
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
        } else if (h.rfind("t=", 0) == 0 && h.find(':') != std::string::npos) {
            // TegraGM20BMetalPipelineKey.h's tessellator state (a journal's t=<ms> has no colon)
            unsigned long t[6] = {};
            int scale = 0;
            if (sscanf(h.c_str() + 2, "%lu:%lu:%lu:%lu:%lu:%lu:%d", t, t + 1, t + 2, t + 3, t + 4, t + 5, &scale) != 7)
                return refuse("its tessellator field " + h + " does not parse");
            d.tessellationPartitionMode = MTLTessellationPartitionMode(t[0]);
            d.tessellationFactorFormat = MTLTessellationFactorFormat(t[1]);
            d.tessellationFactorStepFunction = MTLTessellationFactorStepFunction(t[2]);
            d.tessellationOutputWindingOrder = MTLWinding(t[3]);
            d.tessellationControlPointIndexType = MTLTessellationControlPointIndexType(t[4]);
            d.maxTessellationFactor = t[5];
            d.tessellationFactorScaleEnabled = scale != 0;
        } else if (h.rfind("amp=", 0) == 0) {
            d.maxVertexAmplificationCount = strtoul(h.c_str() + 4, nullptr, 10);
        } else if (h == "tier=1") {
            s.tier = 1;
        } else if (h == "twr=2") {
            [d setValue:@2 forKey:@"textureWriteRoundingMode"];
        } else if (h == "alphacov=1") {
            d.alphaToCoverageEnabled = YES;
        } else if (h[0] == 'a' && isdigit(h[1])) {
            unsigned i, x[6] = {};
            unsigned long long stride = 0;
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%llu:%u:%u", &i, x, x + 1, x + 2, &stride, x + 4, x + 5) != 7 ||
                i >= 31 || x[2] >= 31)
                continue;
            if (!vd)
                vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            // MTLBufferLayoutStrideDynamic, as a journal's key wrote it (NSUIntegerMax) or TegraGM20BMetalPipelineKey.h writes it now
            const bool dynamic = stride == UINT64_MAX || stride == UINT32_MAX;
            x[3] = dynamic ? UINT32_MAX : unsigned(stride);
            vd.layouts[x[2]].stride = dynamic ? MTLBufferLayoutStrideDynamic : NSUInteger(stride);
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
            s.vertexBuffer = x[2];
            s.vertexStride = x[3];
        }
    }
    d.vertexDescriptor = vd;
    gm20b::StageKey vertex, fragment;
    d.vertexFunction = function(qc, v, vertex);
    d.fragmentFunction = function(qc, f, fragment);
    if (!d.vertexFunction)
        return refuse("its library makes no vertex function " + (v.empty() ? std::string("-") : v[0]));
    if (!d.fragmentFunction && !f.empty() && f[0] != "-")
        return refuse("its library makes no fragment function " + f[0]);
    std::string linked;
    for (size_t at = bar3; at != std::string::npos && at + 3 < line.size();) {
        const size_t next = line.find(" | ", at + 3);
        const std::string segment = line.substr(at + 3, (next == std::string::npos ? line.size() : next) - at - 3);
        const std::string tag = segment.substr(0, 3);
        const bool isLinked = tag == "vl " || tag == "vp " || tag == "fl " || tag == "fp ";
        if ((isLinked && !linkedFunction(d, qc, segment, linked)) ||
            (segment.rfind("cubic ", 0) == 0 && !cubicBinds(segment, s.cubic)) ||
            (segment.rfind("xrs=", 0) == 0 && !(s.xrSlots = uint16_t(strtoul(segment.c_str() + 4, nullptr, 10)))) ||
            (segment.rfind("wtex=", 0) == 0 && !gm20b::parseWritten(segment, s.written)))
            return refuse("its segment \"" + segment.substr(0, 80) + "\" makes nothing");
        at = next;
    }
    const gm20b::PipelineFields fields = gm20b::fieldsOf(d);
    s.key = gm20b::pipelineKey(fields, &vertex, d.fragmentFunction ? &fragment : nullptr, linked, framework) +
            (s.cubic.empty() ? std::string() : s.cubic.key());
    s.fast = gm20b::fastKey(fields, &vertex, d.fragmentFunction ? &fragment : nullptr, linked, framework);
    s.d = d;
    return true;
}

inline bool endsWith(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && !s.compare(s.size() - suffix.size(), suffix.size(), suffix);
}

// A line's library (its vertex function's lib= path) where it is another framework's default.metallib than QuartzCore's, a
// bundle's or a framework resource library of the OS (tools/gpu/make_pack.py OS_LIBRARY), or one of our own apps'
// (APP_LIBRARY)
inline std::string libraryPath(const std::string &line)
{
    const size_t at = line.find(" lib=");
    if (at == std::string::npos)
        return "";
    const std::string path = line.substr(at + 5, line.find(' ', at + 5) - at - 5);
    const bool framework = endsWith(path, ".framework/default.metallib");
    const bool resources =
        path.find(".bundle/") != std::string::npos || path.find(".framework/Resources/") != std::string::npos;
    const bool bundle = path.rfind("/System/Library/", 0) == 0 && endsWith(path, ".metallib") && resources;
    const bool app = path.rfind("/Applications/Switch", 0) == 0 && endsWith(path, ".app/default.metallib") &&
                     path.find('/', 14) == path.size() - 17;
    if ((!framework && !bundle && !app) || path.find("/QuartzCore.framework/") != std::string::npos)
        return "";
    return path;
}

// The framework a library is the default library of, "" for a bundle's (its states are keyed by their functions' AIR)
inline std::string frameworkName(const std::string &path)
{
    if (!endsWith(path, ".framework/default.metallib"))
        return "";
    const size_t end = path.rfind(".framework/"), begin = path.rfind('/', end) + 1;
    return path.substr(begin, end - begin);
}

// A framework's library under PROBE_LIB_ROOT (the crawl's root), loaded once
inline id<MTLLibrary> frameworkLibrary(id<MTLDevice> dev, const std::string &path)
{
    static NSMutableDictionary<NSString *, id<MTLLibrary>> *loaded = [NSMutableDictionary dictionary];
    NSString *key = @(path.c_str());
    if (!loaded[key]) {
        const char *root = getenv("PROBE_LIB_ROOT");
        NSError *e = nil;
        if (id<MTLLibrary> l =
                [dev newLibraryWithURL:[NSURL fileURLWithPath:@((std::string(root ? root : "") + path).c_str())]
                                 error:&e])
            loaded[key] = l;
    }
    return loaded[key];
}

// The states a corpus names, in probe order: Core Animation's R lines (PROBE_LABEL, default com.apple.coreanimation.: only lines
// whose label starts with it; "" for every line) with their lib= fields dropped, and another framework's lines marked "@L <its
// library path> " (the library of a function such a line links kept as from=); with `own`, the driver's own states after them, marked "@O ": the conversions of YCbCr textures
// (pack/ycbcr.metal) and the extended-range stand-ins (pack/xr.metal), and the moves between an RGB8_444_2P target's planes and its
// RGBA16Float stand-in (TwoPlaneStandIn.metal)
inline std::vector<std::string> corpusLines(NSString *text, bool own)
{
    std::vector<std::string> lines;
    const char *labelPrefix = getenv("PROBE_LABEL") ? getenv("PROBE_LABEL") : "com.apple.coreanimation.";
    NSRegularExpression *lib = [NSRegularExpression regularExpressionWithPattern:@" lib=\\S+" options:0 error:nil];
    NSRegularExpression *linkedLib = [NSRegularExpression regularExpressionWithPattern:@"( \\| [vf][lp] \\S+) lib="
                                                                               options:0
                                                                                 error:nil];
    for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
        if (![l hasPrefix:@"R "] || ![l containsString:@" ok=1 "])
            continue;
        if (const std::string path = libraryPath(l.UTF8String); !path.empty()) {
            NSString *kept = [linkedLib stringByReplacingMatchesInString:l
                                                                 options:0
                                                                   range:NSMakeRange(0, l.length)
                                                            withTemplate:@"$1 from="];
            lines.push_back("@L " + path + " " +
                            [lib stringByReplacingMatchesInString:kept
                                                          options:0
                                                            range:NSMakeRange(0, kept.length)
                                                     withTemplate:@""]
                                .UTF8String);
            continue;
        }
        if (*labelPrefix && ![l containsString:[@" label=" stringByAppendingString:@(labelPrefix)]])
            continue;
        NSString *plain = [lib stringByReplacingMatchesInString:l
                                                        options:0
                                                          range:NSMakeRange(0, l.length)
                                                   withTemplate:@""];
        lines.push_back(plain.UTF8String);
    }
    if (!own)
        return lines;
    for (const char *out : {"115 | v ycbcr_vert | f ycbcr_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_srgb_frag",
                            "70 | v ycbcr_vert | f ycbcr_codes_frag",
                            "90 | v ycbcr_vert | f ycbcr_codes10_frag",
                            "115 | v ycbcr_vert | f ycbcr_packed_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_packed_srgb_frag",
                            "90 | v ycbcr_vert | f ycbcr_packed_codes10_frag",
                            "90 | v ycbcr_vert | f ycbcr_packed_plane_frag",
                            "55 | v ycbcr_vert | f ycbcr_load_plane_packed_r_frag",
                            "105 | v ycbcr_vert | f ycbcr_load_plane_packed_rg_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_srgb_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_codes_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_luma_first_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_converted_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_srgb_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_codes10_frag",
                            "125 | v ycbcr_vert | f ycbcr_load_packed_luma_first_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_srgb_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_srgb_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_srgb_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_srgb_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_srgb_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_srgb_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_plane_packed_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_masked_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_masked_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_masked_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_masked_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_store_luma8_srgb_masked_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_store_luma16_srgb_masked_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_store_chroma8_srgb_masked_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_store_chroma16_srgb_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_luma_packed_srgb_masked_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_store_chroma_packed_srgb_masked_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_srgb_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_masked_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_store_words_srgb_masked_frag",
                            "10 | v ycbcr_store_vert | f ycbcr_overlap_r8_frag",
                            "20 | v ycbcr_store_vert | f ycbcr_overlap_r16_frag",
                            "30 | v ycbcr_store_vert | f ycbcr_overlap_rg8_frag",
                            "60 | v ycbcr_store_vert | f ycbcr_overlap_rg16_frag",
                            "90 | v ycbcr_store_vert | f ycbcr_overlap_rgb10a2_frag",
                            "53 | v ycbcr_store_vert | f ycbcr_overlap_r32ui_frag",
                            "103 | v ycbcr_store_vert | f ycbcr_overlap_rg32ui_frag",
                            "123 | v ycbcr_store_vert | f ycbcr_overlap_rgba32ui_frag",
                            "13 | v ycbcr_store_vert | f ycbcr_overlap_planes_frag",
                            "13 | v ycbcr_store_vert | f ycbcr_overlap_meet_frag",
                            "13 | v turn_vert | f turn_copy8_frag",
                            "23 | v turn_vert | f turn_copy16_frag",
                            "53 | v turn_vert | f turn_copy32_frag",
                            "103 | v turn_vert | f turn_copy64_frag",
                            "123 | v turn_vert | f turn_copy128_frag",
                            "13 | v turn_vert | f turn_masked8_frag",
                            "23 | v turn_vert | f turn_masked16_frag",
                            "53 | v turn_vert | f turn_masked32_frag",
                            "103 | v turn_vert | f turn_masked64_frag",
                            "123 | v turn_vert | f turn_masked128_frag",
                            "13 | v turn_vert | f turn_reach_frag",
                            "13 | v turn_vert | f turn_reach_reduce_frag",
                            "115 | v xr_vert | f xr_bgr10_frag",
                            "115 | v xr_vert | f xr_bgr10_srgb_frag",
                            "115 | v xr_vert | f xr_bgra10_frag",
                            "115 | v xr_vert | f xr_bgra10_srgb_frag",
                            "115 | v xr_vert | f xr_planes_frag",
                            "115 | v xr_vert | f xr_planes_srgb_frag",
                            "115 | v xr_vert | f xr_two_planes_frag",
                            "115 | v xr_vert | f xr_two_planes_srgb_frag",
                            "103 | v xr_vert | f xr_planes_merge_frag",
                            "113 | v xr_vert | f xr_copy_clamp_frag",
                            "70 | v resolve_vert | f resolve_rgba8_frag",
                            "70 | v resolve_vert | f resolve_rgba8_srgb_frag",
                            "80 | v resolve_vert | f resolve_bgra8_frag",
                            "80 | v resolve_vert | f resolve_bgra8_srgb_frag",
                            "70 | v resolve_vert | f resolve_rgba8_2x_frag",
                            "70 | v resolve_vert | f resolve_rgba8_4x_frag",
                            "80 | v resolve_vert | f resolve_bgra8_2x_frag",
                            "80 | v resolve_vert | f resolve_bgra8_4x_frag",
                            "92 | v resolve_vert | f resolve_rg11b10f_frag",
                            "115 | v resolve_vert | f resolve_rgba16f_frag",
                            "1 | v resolve_vert | f resolve_a8_frag",
                            "10 | v resolve_vert | f resolve_r8_frag",
                            "30 | v resolve_vert | f resolve_rg8_frag",
                            "20 | v resolve_vert | f resolve_r16_frag",
                            "60 | v resolve_vert | f resolve_rg16_frag",
                            "110 | v resolve_vert | f resolve_rgba16_frag",
                            "90 | v resolve_vert | f resolve_rgb10a2_frag",
                            "94 | v resolve_vert | f resolve_bgr10a2_frag",
                            "12 | v resolve_vert | f resolve_r8snorm_frag",
                            "32 | v resolve_vert | f resolve_rg8snorm_frag",
                            "72 | v resolve_vert | f resolve_rgba8snorm_frag",
                            "25 | v resolve_vert | f resolve_r16f_frag",
                            "65 | v resolve_vert | f resolve_rg16f_frag",
                            "55 | v resolve_vert | f resolve_r32f_frag",
                            "105 | v resolve_vert | f resolve_rg32f_frag",
                            "125 | v resolve_vert | f resolve_rgba32f_frag",
                            "40 | v resolve_vert | f resolve_b5g6r5_frag",
                            "43 | v resolve_vert | f resolve_bgr5a1_frag",
                            "1 | v mip_vert | f mip_a8_frag",
                            "10 | v mip_vert | f mip_r8_frag",
                            "12 | v mip_vert | f mip_r8snorm_frag",
                            "20 | v mip_vert | f mip_r16_frag",
                            "22 | v mip_vert | f mip_r16snorm_frag",
                            "25 | v mip_vert | f mip_r16f_frag",
                            "30 | v mip_vert | f mip_rg8_frag",
                            "32 | v mip_vert | f mip_rg8snorm_frag",
                            "40 | v mip_vert | f mip_b5g6r5_frag",
                            "43 | v mip_vert | f mip_bgr5a1_frag",
                            "55 | v mip_vert | f mip_r32f_frag",
                            "60 | v mip_vert | f mip_rg16_frag",
                            "62 | v mip_vert | f mip_rg16snorm_frag",
                            "65 | v mip_vert | f mip_rg16f_frag",
                            "70 | v mip_vert | f mip_rgba8_frag",
                            "72 | v mip_vert | f mip_rgba8snorm_frag",
                            "71 | v mip_vert | f mip_rgba8_srgb_frag",
                            "80 | v mip_vert | f mip_bgra8_frag",
                            "81 | v mip_vert | f mip_bgra8_srgb_frag",
                            "90 | v mip_vert | f mip_rgb10a2_frag",
                            "92 | v mip_vert | f mip_rg11b10f_frag",
                            "94 | v mip_vert | f mip_bgr10a2_frag",
                            "105 | v mip_vert | f mip_rg32f_frag",
                            "110 | v mip_vert | f mip_rgba16_frag",
                            "112 | v mip_vert | f mip_rgba16snorm_frag",
                            "115 | v mip_vert | f mip_rgba16f_frag",
                            "125 | v mip_vert | f mip_rgba32f_frag",
                            "1 | v mip_vert | f mip_3d_a8_frag",
                            "10 | v mip_vert | f mip_3d_r8_frag",
                            "12 | v mip_vert | f mip_3d_r8snorm_frag",
                            "20 | v mip_vert | f mip_3d_r16_frag",
                            "22 | v mip_vert | f mip_3d_r16snorm_frag",
                            "25 | v mip_vert | f mip_3d_r16f_frag",
                            "30 | v mip_vert | f mip_3d_rg8_frag",
                            "32 | v mip_vert | f mip_3d_rg8snorm_frag",
                            "40 | v mip_vert | f mip_3d_b5g6r5_frag",
                            "43 | v mip_vert | f mip_3d_bgr5a1_frag",
                            "55 | v mip_vert | f mip_3d_r32f_frag",
                            "60 | v mip_vert | f mip_3d_rg16_frag",
                            "62 | v mip_vert | f mip_3d_rg16snorm_frag",
                            "65 | v mip_vert | f mip_3d_rg16f_frag",
                            "70 | v mip_vert | f mip_3d_rgba8_frag",
                            "72 | v mip_vert | f mip_3d_rgba8snorm_frag",
                            "71 | v mip_vert | f mip_3d_rgba8_srgb_frag",
                            "80 | v mip_vert | f mip_3d_bgra8_frag",
                            "81 | v mip_vert | f mip_3d_bgra8_srgb_frag",
                            "90 | v mip_vert | f mip_3d_rgb10a2_frag",
                            "92 | v mip_vert | f mip_3d_rg11b10f_frag",
                            "94 | v mip_vert | f mip_3d_bgr10a2_frag",
                            "105 | v mip_vert | f mip_3d_rg32f_frag",
                            "110 | v mip_vert | f mip_3d_rgba16_frag",
                            "112 | v mip_vert | f mip_3d_rgba16snorm_frag",
                            "115 | v mip_vert | f mip_3d_rgba16f_frag",
                            "125 | v mip_vert | f mip_3d_rgba32f_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    // the formats the GM20B renders none of (runtime/format_standin.h): their stand-ins' load and store states and their
    // mip states, each into its words' integer view
    for (const char *out : {"125 | v fmt_vert | f fmt_load_frag",
                            "125 | v fmt_vert | f fmt_load_rgb9e5_frag",
                            "23 | v fmt_vert | f fmt_store_abgr4_frag",
                            "23 | v fmt_vert | f fmt_store_a1bgr5_frag",
                            "23 | v fmt_vert | f fmt_store_bgra4_frag",
                            "53 | v fmt_vert | f fmt_store_rgb9e5_frag",
                            "23 | v fmt_vert | f fmt_mip_abgr4_frag",
                            "23 | v fmt_vert | f fmt_mip_a1bgr5_frag",
                            "53 | v fmt_vert | f fmt_mip_rgb9e5_frag",
                            "13 | v fmt_vert | f fmt_mip_srgb8_frag",
                            "53 | v fmt_vert | f fmt_mip_bgr10_xr_frag",
                            "53 | v fmt_vert | f fmt_mip_bgr10_xr_srgb_frag",
                            "113 | v fmt_vert | f fmt_mip_bgra10_xr_frag",
                            "113 | v fmt_vert | f fmt_mip_bgra10_xr_srgb_frag",
                            "53 | v fmt_vert | f fmt_mip_3d_bgr10_xr_frag",
                            "53 | v fmt_vert | f fmt_mip_3d_bgr10_xr_srgb_frag",
                            "113 | v fmt_vert | f fmt_mip_3d_bgra10_xr_frag",
                            "113 | v fmt_vert | f fmt_mip_3d_bgra10_xr_srgb_frag",
                            "13 | v fmt_vert | f fmt_mip_3d_srgb8_frag",
                            "23 | v fmt_vert | f fmt_mip_3d_abgr4_frag",
                            "23 | v fmt_vert | f fmt_mip_3d_a1bgr5_frag",
                            "53 | v fmt_vert | f fmt_mip_3d_rgb9e5_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    // the extended-range targets' stand-ins (runtime/xr.h xrTargetState): their loads, stores (into the codes' integer view)
    // and resolves; the stores over a surface's two planes are among the pairs below
    for (const char *out : {"115 | v xr_target_vert | f xr_target_load_bgr10_frag",
                            "115 | v xr_target_vert | f xr_target_load_bgra10_frag",
                            "115 | v xr_target_vert | f xr_target_load_planes_frag",
                            "115 | v xr_target_vert | f xr_target_load_two_planes_frag",
                            "115 | v xr_target_vert | f xr_target_load_bgr10_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_load_bgra10_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_load_planes_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_load_two_planes_srgb_frag",
                            "53 | v xr_target_vert | f xr_target_store_bgr10_frag",
                            "113 | v xr_target_vert | f xr_target_store_bgra10_frag",
                            "103 | v xr_target_vert | f xr_target_store_planes_frag",
                            "53 | v xr_target_vert | f xr_target_store_bgr10_srgb_frag",
                            "113 | v xr_target_vert | f xr_target_store_bgra10_srgb_frag",
                            "103 | v xr_target_vert | f xr_target_store_planes_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgr10_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgra10_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgr10_srgb_frag",
                            "115 | v xr_target_vert | f xr_target_resolve_bgra10_srgb_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    // the formats decoded on upload (runtime/decoded.h): each decode state into its stored format's integer view
    for (const char *out :
         {"53 | v decode_vert | f decode_pvrtc_frag", "53 | v decode_vert | f decode_astc_unorm8_frag",
          "113 | v decode_vert | f decode_astc_unorm16_frag", "113 | v decode_vert | f decode_astc_half_frag"}) {
        std::string o = out;
        const size_t bar = o.find(' ');
        lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=" + o.substr(0, bar) + ":15:0:1:0:0:1:0:0" +
                        o.substr(bar));
    }
    for (const char *depth :
         {"250/0 | v resolve_vert | f resolve_depth16_", "252/0 | v resolve_vert | f resolve_depth32f_",
          "260/260 | v resolve_vert | f resolve_depth32fs8_"})
        for (const char *filter : {"sample0_frag", "min_frag", "max_frag"})
            lines.push_back(std::string("@O R ok=1 label=raw.own samples=1 topo=0 ds=") + depth + filter);
    for (const char *depth :
         {"250/0 | v turn_vert | f turn_depth16_frag", "252/0 | v turn_vert | f turn_depth32f_frag",
          "260/260 | v turn_vert | f turn_depth32fs8_frag", "0/253 | v stencil_bits_vert | f stencil_bits_s8_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_d32fs8_frag",
          "0/253 | v stencil_bits_vert | f stencil_bits_ms_s8_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_min_frag",
          "260/260 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_max_frag"})
        lines.push_back(std::string("@O R ok=1 label=raw.own samples=1 topo=0 ds=") + depth);
    for (const char *samples : {"2", "4"})
        for (const char *depth :
             {"250/0 | v turn_vert | f turn_depth16_ms", "260/260 | v turn_vert | f turn_depth32fs8_ms"})
            lines.push_back(std::string("@O R ok=1 label=raw.own samples=") + samples + " topo=0 ds=" + depth +
                            samples + "_frag");
    // a multisampled format stand-in's per-sample load and store states (runtime/format_standin.h bySamples), drawn at
    // the target's sample count; RGB9E5Float's resolve of one (runtime/attachment_target.h formatResolveState)
    for (const char *samples : {"2", "4"})
        for (const char *out : {"125 | v fmt_vert | f fmt_load_ms", "125 | v fmt_vert | f fmt_load_rgb9e5_ms",
                                "23 | v fmt_vert | f fmt_store_abgr4_ms", "23 | v fmt_vert | f fmt_store_bgra4_ms",
                                "23 | v fmt_vert | f fmt_store_a1bgr5_ms", "53 | v fmt_vert | f fmt_store_rgb9e5_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgr10_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgra10_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgr10_srgb_ms",
                                "115 | v xr_target_vert | f xr_target_load_bgra10_srgb_ms",
                                "53 | v xr_target_vert | f xr_target_store_bgr10_ms",
                                "113 | v xr_target_vert | f xr_target_store_bgra10_ms",
                                "53 | v xr_target_vert | f xr_target_store_bgr10_srgb_ms",
                                "113 | v xr_target_vert | f xr_target_store_bgra10_srgb_ms"}) {
            const std::string o = out;
            const size_t bar = o.find(' ');
            lines.push_back(std::string("@O R ok=1 label=raw.own samples=") + samples + " topo=0 ds=0/0 c0=" +
                            o.substr(0, bar) + ":15:0:1:0:0:1:0:0" + o.substr(bar) + samples + "_frag");
        }
    lines.push_back("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 c0=125:15:0:1:0:0:1:0:0 | v fmt_vert | f "
                    "fmt_resolve_rgb9e5_frag");
    // and the one-draw stencil route's (runtime/zeta_colour.h): a multisampled Depth32Float_Stencil8 source's through
    // write mask G alone (MTLColorWriteMaskGreen), its target's depth word kept
    for (const char *out : {"c0=115:15:0:1:0:0:1:0:0 | v two_plane_cover | f two_plane_load",
                            "c0=10:15:0:1:0:0:1:0:0 c1=30:15:0:1:0:0:1:0:0 | v two_plane_cover | f two_plane_store",
                            "c0=53:15:0:1:0:0:1:0:0 c1=13:15:0:1:0:0:1:0:0 | v xr_vert | f xr_planes_split_frag",
                            "c0=53:15:0:1:0:0:1:0:0 c1=13:15:0:1:0:0:1:0:0 | v xr_target_vert | f "
                            "xr_target_store_two_planes_frag",
                            "c0=53:15:0:1:0:0:1:0:0 c1=13:15:0:1:0:0:1:0:0 | v xr_target_vert | f "
                            "xr_target_store_two_planes_srgb_frag",
                            "c0=13:15:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_s8_colour_frag",
                            "c0=103:15:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_d32fs8_colour_frag",
                            "c0=13:15:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_s8_colour_frag",
                            "c0=103:4:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_colour_frag",
                            "c0=103:4:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_min_colour_frag",
                            "c0=103:4:0:1:0:0:1:0:0 | v stencil_bits_vert | f stencil_bits_ms_d32fs8_max_colour_frag"})
        lines.push_back(std::string("@O R ok=1 label=raw.own samples=1 topo=0 ds=0/0 ") + out);
    return lines;
}

}  // namespace probe
