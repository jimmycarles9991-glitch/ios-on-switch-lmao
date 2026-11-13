// SPDX-License-Identifier: MIT
// See raw_emit.h. The rules are NVK's (nvk_cmd_draw.c: blend, the write-mask macro, viewport clip control; nvk_shader.c: the
// fragment program's push) applied to what m2v makes of a Metal descriptor.
#include "raw_emit.h"
#include "raw_sample_shading.h"
#include "raw_xr_pass.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <map>

namespace raw {
namespace {

constexpr uint32_t kEarlyZ = 0x0210;         // SET_API_MANDATED_EARLY_Z
constexpr uint32_t kSingleCtWrite = 0x0f90;  // SET_SINGLE_CT_WRITE_CONTROL
constexpr uint32_t kBlend = 0x1360;          // SET_BLEND(j)
constexpr uint32_t kViewportClip = 0x193c;   // SET_VIEWPORT_CLIP_CONTROL
constexpr uint32_t kCtWrite = 0x1a00;        // SET_CT_WRITE(j)
constexpr uint32_t kBlendPerTarget =
    0x1e00;  // SET_BLEND_PER_TARGET_SEPARATE_FOR_ALPHA(j), then its ops and coefficients
constexpr uint32_t kRegisterCount[2] = {0x204c, 0x214c};  // SET_PIPELINE_REGISTER_COUNT(vertex B, pixel)
constexpr uint32_t kAttribute = 0x1160;                   // SET_VERTEX_ATTRIBUTE_A(i)
constexpr uint32_t kStreamFormat = 0x1c00;                // SET_VERTEX_STREAM_A_FORMAT(s), 16 bytes apart
constexpr uint32_t kOpAdd = 0x8006, kZero = 0x4000;
constexpr uint32_t kViewportClipBase = 0x181c;  // NVK's, z_clamp_zero_one in bit 0

// MTLBlendOperation -> NV9097 OGL op
constexpr uint32_t kOps[] = {0x8006, 0x800a, 0x800b, 0x8007, 0x8008};
// MTLBlendFactor -> NV9097 OGL coefficient, through m2v's VkBlendFactor
constexpr uint32_t kFactors[] = {0x4000, 0x4001, 0x4300, 0x4301, 0x4302, 0x4303, 0x4306, 0x4307, 0x4304, 0x4305,
                                 0x4308, 0xc001, 0xc002, 0xc003, 0xc004, 0xc900, 0xc901, 0xc902, 0xc903};

// "c<j>=format:writeMask:blending:srcRGB:dstRGB:opRGB:srcAlpha:dstAlpha:opAlpha"
struct Color {
    unsigned format, writeMask, blending, factor[6];  // srcRGB, dstRGB, opRGB, srcAlpha, dstAlpha, opAlpha
};

// m2v holds an extended-range attachment as its raw codes (RGB10A8_2P_XR10, BGRA10_XR, BGR10_XR and _sRGB twins): the program
// writes the components of the format it is stored in, and blends in itself
bool extendedRange(unsigned format)
{
    return format >= 550 && format <= 555;
}

bool identityBlend(const Color &c)
{
    return c.factor[0] == 1 && c.factor[1] == 0 && c.factor[2] == 0 && c.factor[3] == 1 && c.factor[4] == 0 &&
           c.factor[5] == 0;
}

uint32_t bits(const uint8_t *header, unsigned lo, unsigned count)
{
    uint32_t v = 0;
    for (unsigned b = 0; b < count; b++)
        v |= uint32_t(header[(lo + b) / 8] >> ((lo + b) % 8) & 1) << b;
    return v;
}

// SET_CT_WRITE's layout: a component's enable in bits 0, 4, 8, 12 (r, g, b, a)
uint32_t ctWrite(unsigned components)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        if (components >> i & 1)
            v |= 1u << (4 * i);
    return v;
}

bool parse(const std::string &key, std::map<unsigned, Color> &colors, unsigned &depthFormat, std::string &why)
{
    depthFormat = 0;
    size_t at = 0;
    while (at < key.size()) {
        size_t end = key.find(' ', at);
        if (end == std::string::npos)
            end = key.size();
        const std::string field = key.substr(at, end - at);
        at = end + 1;
        unsigned j = 0, stencil = 0;
        Color c = {};
        if (field.rfind("ds=", 0) == 0 && sscanf(field.c_str(), "ds=%u/%u", &depthFormat, &stencil) == 2)
            continue;
        if (field[0] != 'c' || field.size() < 3 || field[1] < '0' || field[1] > '7')
            continue;
        if (sscanf(field.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &j, &c.format, &c.writeMask, &c.blending,
                   &c.factor[0], &c.factor[1], &c.factor[2], &c.factor[3], &c.factor[4], &c.factor[5]) != 10 ||
            c.factor[0] >= std::size(kFactors) || c.factor[1] >= std::size(kFactors) ||
            c.factor[3] >= std::size(kFactors) || c.factor[4] >= std::size(kFactors) ||
            c.factor[2] >= std::size(kOps) || c.factor[5] >= std::size(kOps)) {
            why = "a color field it can't read: " + field;
            return false;
        }
        colors[j] = c;
    }
    return true;
}

// MTLVertexFormat -> SET_VERTEX_ATTRIBUTE_A's component bit widths, numerical type and R/B swap, through m2v's VkFormat and
// NVK's nvk_vf_formats; 0 widths: one this has no rule for
struct VertexFormat {
    uint8_t widths, type, swap;
};

VertexFormat vertexFormat(unsigned format)
{
    constexpr uint8_t kSnorm = 1, kUnorm = 2, kUint = 4, kFloat = 7;
    constexpr uint8_t kR32G32B32A32 = 0x01, kR32G32B32 = 0x02, kR16G16B16A16 = 0x03, kR32G32 = 0x04, kR16G16B16 = 0x05,
                      kR8G8B8A8 = 0x0a, kR16G16 = 0x0f, kR32 = 0x12;
    switch (format) {
    case 9:  // UChar4Normalized
        return {kR8G8B8A8, kUnorm, 0};
    case 12:  // Char4Normalized
        return {kR8G8B8A8, kSnorm, 0};
    case 13:  // UShort2
        return {kR16G16, kUint, 0};
    case 19:  // UShort2Normalized
        return {kR16G16, kUnorm, 0};
    case 21:  // UShort4Normalized
        return {kR16G16B16A16, kUnorm, 0};
    case 22:  // Short2Normalized
        return {kR16G16, kSnorm, 0};
    case 24:  // Short4Normalized
        return {kR16G16B16A16, kSnorm, 0};
    case 25:  // Half2
        return {kR16G16, kFloat, 0};
    case 26:  // Half3
        return {kR16G16B16, kFloat, 0};
    case 27:  // Half4
        return {kR16G16B16A16, kFloat, 0};
    case 28:  // Float
        return {kR32, kFloat, 0};
    case 29:  // Float2
        return {kR32G32, kFloat, 0};
    case 30:  // Float3
        return {kR32G32B32, kFloat, 0};
    case 31:  // Float4
        return {kR32G32B32A32, kFloat, 0};
    case 42:  // UChar4Normalized_BGRA
        return {kR8G8B8A8, kUnorm, 1};
    default:
        return {0, 0, 0};
    }
}

// Each RGB10A8_2P_XR10 colour's alpha byte as the R8Uint target at its alpha index (raw_xr_pass.h), written where Metal's mask
// writes alpha (MTLColorWriteMaskAlpha, bit 0) and never blended; returns those targets (xrAlphaTargets)
uint32_t addAlphaPlanes(std::map<unsigned, Color> &colors)
{
    uint32_t formats[8] = {};
    for (const auto &[j, c] : colors)
        if (j < 8)
            formats[j] = c.format;
    const std::array<int8_t, 8> alpha = xrAlphaSlots(formats);
    for (uint32_t j = 0; j < 8; j++)
        if (alpha[j] >= 0)
            colors[unsigned(alpha[j])] = {kXrAlphaFormat, colors[j].writeMask & 1 ? 8u : 0u, 0, {1, 0, 0, 1, 0, 0}};
    return xrAlphaTargets(formats);
}

// "a<i>=format:offset:buffer:stride:stepFunction:stepRate"
struct Attribute {
    unsigned format, offset, buffer, stride, step, rate;
};

}  // namespace

bool emitVertexInput(const std::string &keyFields, const std::vector<uint32_t> &attributes,
                     std::vector<EmitRegister> &out, std::string &why)
{
    std::vector<EmitStream> streams;
    return emitVertexInput(keyFields, attributes, out, streams, why);
}

bool emitVertexInput(const std::string &keyFields, const std::vector<uint32_t> &attributes,
                     std::vector<EmitRegister> &out, std::vector<EmitStream> &laidOut, std::string &why)
{
    constexpr uint32_t kStreamEnable = 1u << 12, kStepPerVertex = 1;
    constexpr uint32_t kSourceInactive = 1u << 6, kInactive = kSourceInactive | 0x01u << 21 | 7u << 27;
    std::map<unsigned, Attribute> descriptor;
    for (size_t at = 0; at < keyFields.size();) {
        size_t end = keyFields.find(' ', at);
        if (end == std::string::npos)
            end = keyFields.size();
        const std::string field = keyFields.substr(at, end - at);
        at = end + 1;
        unsigned i = 0;
        Attribute a = {};
        if (field.size() > 2 && field[0] == 'a' && field[1] >= '0' && field[1] <= '9' &&
            sscanf(field.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, &a.format, &a.offset, &a.buffer, &a.stride, &a.step,
                   &a.rate) == 7)
            descriptor[i] = a;
    }
    std::vector<uint32_t> sorted(attributes);
    std::sort(sorted.begin(), sorted.end());
    std::map<unsigned, unsigned> streams;  // Metal buffer index -> its stream, densely in buffer-index order
    for (uint32_t index : sorted) {
        const auto it = descriptor.find(index);
        if (it == descriptor.end()) {
            why = "the vertex function reads attribute " + std::to_string(index) + ", which the descriptor lacks";
            return false;
        }
        if (!vertexFormat(it->second.format).widths || it->second.step != kStepPerVertex) {
            why = "attribute " + std::to_string(index) + " is fetched in the shader (its format or step)";
            return false;
        }
        streams[it->second.buffer] = 0;
    }
    unsigned next = 0;
    for (auto &[buffer, stream] : streams)
        stream = next++;
    for (uint32_t location = 0; location < 32; location++) {
        if (location >= sorted.size()) {
            out.push_back({kAttribute + 4 * location, kInactive, true});
            continue;
        }
        const Attribute &a = descriptor[sorted[location]];
        const VertexFormat f = vertexFormat(a.format);
        out.push_back({kAttribute + 4 * location,
                       streams[a.buffer] | (a.offset & 0x3fff) << 7 | uint32_t(f.widths) << 21 |
                           uint32_t(f.type) << 27 | uint32_t(f.swap) << 31,
                       true});
    }
    // a stream no attribute reads is off, its stride unread
    if (streams.empty())
        out.push_back({kStreamFormat, 0, false});
    laidOut.clear();
    for (const auto &[buffer, stream] : streams) {
        unsigned stride = 0;
        for (const auto &[i, a] : descriptor)
            if (a.buffer == buffer)
                stride = a.stride;
        const bool bindStride = stride == UINT32_MAX;  // RawPipelineKey.h's dynamic stride: the draw's
        // SET_VERTEX_STREAM_A_FORMAT's STRIDE is bits 11:0; m2v pulls a layout past maxVertexInputBindingStride (2048)
        if (!bindStride && stride > 0xfff) {
            why = "vertex buffer " + std::to_string(buffer) + " fetched by fixed function at a stride past 12 bits";
            return false;
        }
        out.push_back({kStreamFormat + 0x10 * stream, kStreamEnable | (bindStride ? 0 : stride), true});
        laidOut.push_back({uint8_t(buffer), bindStride});
    }
    return true;
}

bool emitState(const std::string &keyFields, const EmitProgram &vertex, const EmitProgram &fragment,
               std::vector<EmitRegister> &out, std::string &why, const uint32_t *blended, bool twoPlane,
               uint32_t *alphaTargets)
{
    std::map<unsigned, Color> colors;
    unsigned depthFormat = 0;
    if (!parse(keyFields, colors, depthFormat, why))
        return false;
    const uint32_t alpha = twoPlane ? addAlphaPlanes(colors) : 0;
    if (alphaTargets)
        *alphaTargets = alpha;
    // the fragment program's output map: 4 components per target, from bit 576 of its header (NAK sph.rs omap_target)
    const uint32_t outputs = fragment.header ? bits(fragment.header, 576, 32) : 0;
    out.clear();
    std::map<unsigned, uint32_t> writes;
    for (const auto &[j, c] : colors) {
        const unsigned written = outputs >> (4 * j) & 0xF;
        // m2v's write mask: Metal's (red in bit 3) where the program writes the target; an extended-range target's the
        // components its stored format has
        unsigned components = 0;
        if (written && c.writeMask)
            for (unsigned i = 0; i < 4; i++)
                components |= (extendedRange(c.format) ? written >> i & 1 : c.writeMask >> (3 - i) & 1) << i;
        writes[j] = ctWrite(components);
        out.push_back({kCtWrite + 4 * j, writes[j], true});
        // m2v leaves a blend of ONE and ZERO on where the program does not round the target's writes itself
        const bool vulkanBlend = blended ? (*blended >> j & 1) : c.blending && !identityBlend(c);
        const bool blends = vulkanBlend && c.writeMask && !extendedRange(c.format);
        out.push_back({kBlend + 4 * j, blends, writes[j] && !(c.blending && identityBlend(c))});
        // Metal's alpha factors are always their own; target 0's too, so no record takes it from what the pack leaves
        out.push_back({kBlendPerTarget + 0x20 * j, 1, true});
        const uint32_t values[6] = {kOps[c.factor[2]], kFactors[c.factor[0]], kFactors[c.factor[1]],
                                    kOps[c.factor[5]], kFactors[c.factor[3]], kFactors[c.factor[4]]};
        const uint32_t off[6] = {kOpAdd, kZero, kZero, kOpAdd, kZero, kZero};
        for (unsigned k = 0; k < 6; k++)
            out.push_back({kBlendPerTarget + 0x20 * j + 4 + 4 * k, blends ? values[k] : off[k], blends && writes[j]});
    }
    // NVK's write-mask macro: one control when every target's mask is the first's
    bool common = true;
    for (const auto &[j, w] : writes)
        common &= w == writes.begin()->second;
    out.push_back({kSingleCtWrite, common, true});
    const bool floatDepth = depthFormat == 252 || depthFormat == 260;  // Depth32Float, Depth32Float_Stencil8
    out.push_back({kViewportClip, kViewportClipBase | !floatDepth, true});
    out.push_back({kEarlyZ, fragment.earlyFragmentTests, true});
    out.push_back({0x1224, fragment.interlockControl, true});
    unsigned samples = 1;
    if (const size_t field = keyFields.find("samples="); field != std::string::npos)
        sscanf(keyFields.c_str() + field, "samples=%u", &samples);
    if (!nativeSamples(samples)) {
        why = "an unsupported raster sample count";
        return false;
    }
    out.push_back({0x0754, sampleShadingControl(fragment.usesSampleShading ? samples : 1), true});
    out.push_back({kRegisterCount[0], vertex.gprs, true});
    out.push_back({kRegisterCount[1], fragment.gprs, true});
    return true;
}

std::set<uint32_t> emittableMethods()
{
    constexpr uint32_t kTargets = 8, kStreams = 32;
    std::set<uint32_t> out = {kEarlyZ,           kSingleCtWrite, kViewportClip, kRegisterCount[0],
                              kRegisterCount[1], 0x1224,         0x0754};
    for (uint32_t j = 0; j < kTargets; j++) {
        out.insert({kCtWrite + 4 * j, kBlend + 4 * j});
        for (uint32_t k = 0; k < 7; k++)
            out.insert(kBlendPerTarget + 0x20 * j + 4 * k);
    }
    for (uint32_t location = 0; location < 32; location++)
        out.insert(kAttribute + 4 * location);
    for (uint32_t stream = 0; stream < kStreams; stream++)
        out.insert(kStreamFormat + 0x10 * stream);
    return out;
}

}  // namespace raw
