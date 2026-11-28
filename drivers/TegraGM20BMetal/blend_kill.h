// SPDX-License-Identifier: MIT
// SET_ITERATED_BLEND_OPTIMIZATION NOOP_SOURCE_RGBA_0000 (clb197.h): the ROP drops a pixel whose colour output is
// (0, 0, 0, 0) instead of blending it. A state turns it on only where that output leaves its target as it was under the
// state's own ROP blend (its blob's registers, not the Metal descriptor: m2v may blend in the program instead), for every
// value the target can hold:
//  - one target written (SET_CT_WRITE), target 0; the probe drew one target, and a pixel kill decided on one output would
//    drop the writes of the others. Only the targets the state's key names count: a pass's attachments are its state's,
//    and the pack's baseline keeps a target's SET_CT_WRITE where every state drawing that target writes it alike (24B119:
//    targets 6 and 7 at 0x1111), which a state without that target reads as written;
//  - its ROP blends (SET_BLEND), colour and alpha alike: op ADD or REVERSE_SUBTRACT (0 + D and D - 0 are D; SUBTRACT
//    gives -D, MIN and MAX ignore the coefficients); destination coefficient ONE, ONE_MINUS_SRC_COLOR or
//    ONE_MINUS_SRC_ALPHA (each 1 with that source); source coefficient one that is finite with that source, so 0 times it
//    is 0: ZERO, ONE, SRC_*, ONE_MINUS_SRC_*, and on a normalized target, whose texels are finite, DST_*,
//    ONE_MINUS_DST_* and SRC_ALPHA_SATURATE too. Never the blend constant (any float the encoder sets) or source 1 (a
//    second output the kill does not look at);
//  - no alpha to coverage or alpha to one (the source the ROP sees is not the program's), no target the program writes
//    as a storage image. A depth or stencil attachment does not keep it off: whether a dropped pixel still writes its
//    depth and stencil, as Metal's does, is HW Test gpu.gm20b.blend_kill's zeta arms (zeta_depth_wrong, zeta_stencil_wrong).
// HW Test gpu.x1.blend_kills (probe d13, PROBELOG): source-over of a zero source onto RGBA16F holding -0, NaN, infinities
// and denormals and onto BGRA8 changed no bit with it on.
#pragma once
#include <cstdint>
#include <cstring>

namespace gm20b {

constexpr uint32_t kIteratedBlendOptimization = 0x02f4;  // NVB197_SET_ITERATED_BLEND_OPTIMIZATION
constexpr uint32_t kNoopNever = 0, kNoopSourceRgba0000 = 1;

// MTLPixelFormats whose texels are finite: unorm, snorm and sRGB colour formats
inline bool normalizedFormat(uint32_t mtl)
{
    switch (mtl) {
    case 1:    // A8Unorm
    case 10:   // R8Unorm
    case 11:   // R8Unorm_sRGB
    case 12:   // R8Snorm
    case 20:   // R16Unorm
    case 22:   // R16Snorm
    case 30:   // RG8Unorm
    case 31:   // RG8Unorm_sRGB
    case 32:   // RG8Snorm
    case 40:   // B5G6R5Unorm
    case 41:   // A1BGR5Unorm
    case 42:   // ABGR4Unorm
    case 43:   // BGR5A1Unorm
    case 60:   // RG16Unorm
    case 62:   // RG16Snorm
    case 70:   // RGBA8Unorm
    case 71:   // RGBA8Unorm_sRGB
    case 72:   // RGBA8Snorm
    case 80:   // BGRA8Unorm
    case 81:   // BGRA8Unorm_sRGB
    case 90:   // RGB10A2Unorm
    case 94:   // BGR10A2Unorm
    case 110:  // RGBA16Unorm
    case 112:  // RGBA16Snorm
        return true;
    default:
        return false;
    }
}

inline bool noopBlendOp(uint32_t op)
{
    return op == 0x8006 || op == 0x800b;  // OGL_FUNC_ADD, OGL_FUNC_REVERSE_SUBTRACT
}

inline bool noopDestCoeff(uint32_t c)
{
    return c == 0x4001 || c == 0x4301 || c == 0x4303;  // ONE, ONE_MINUS_SRC_COLOR, ONE_MINUS_SRC_ALPHA
}

inline bool noopSourceCoeff(uint32_t c, bool normalized)
{
    if (c == 0x4000 || c == 0x4001 || (c >= 0x4300 && c <= 0x4303))  // ZERO, ONE, SRC_COLOR .. ONE_MINUS_SRC_ALPHA
        return true;
    return normalized && c >= 0x4304 && c <= 0x4308;  // DST_ALPHA .. ONE_MINUS_DST_COLOR, SRC_ALPHA_SATURATE
}

inline bool parseKeyUint(const char *&p, uint32_t &out)
{
    if (*p < '0' || *p > '9')
        return false;
    uint32_t v = 0;
    do {
        v = v * 10u + uint32_t(*p - '0');
        ++p;
    } while (*p >= '0' && *p <= '9');
    out = v;
    return true;
}

struct ParsedColorKey {
    uint32_t format = 0;
    uint32_t writeMask = 0;
    uint32_t blending = 0;
    uint32_t srcRGB = 0, dstRGB = 0, rgbOp = 0;
    uint32_t srcAlpha = 0, dstAlpha = 0, alphaOp = 0;
    uint8_t fieldCount = 0;
};

struct ParsedPipelineKey {
    ParsedColorKey colors[8] = {};
    uint32_t colorFormat[8] = {};
    uint8_t colorTargets = 0;
    uint32_t samples = 1;
    bool samples1Prefix = false;
    uint32_t depth = 0, stencil = 0;
    bool hasDs = false;
    bool hasZeroDsToken = false;
    bool rasterOff = false;
    bool hasA0 = false;
    uint32_t a0Format = 0, a0Offset = 0, a0Buffer = 0, a0Stride = 0, a0Step = 0;
};

// Single-pass zero-stdio parser over a pipeline state key string.
inline ParsedPipelineKey parsePipelineKey(const char *keyString)
{
    ParsedPipelineKey out;
    if (!keyString)
        return out;
    out.samples1Prefix = !strncmp(keyString, "samples=1 ", 10);
    bool seenSamples = false;
    uint8_t seenColorFmt = 0;
    for (const char *p = keyString; *p;) {
        const bool afterSpace = p > keyString && p[-1] == ' ';
        const char *tok = p;
        while (*p && *p != ' ')
            ++p;
        const size_t len = size_t(p - tok);
        if (afterSpace) {
            if (len >= 4 && tok[0] == 'c' && tok[1] >= '0' && tok[1] <= '9') {
                const char *q = tok + 1;
                uint32_t c = 0;
                if (parseKeyUint(q, c) && *q == '=' && c < 8) {
                    ++q;
                    uint32_t vals[9] = {};
                    uint8_t count = 0;
                    while (count < 9) {
                        if (!parseKeyUint(q, vals[count]))
                            break;
                        ++count;
                        if (*q != ':')
                            break;
                        ++q;
                    }
                    if (count >= 1 && !(seenColorFmt >> c & 1)) {
                        seenColorFmt |= uint8_t(1u << c);
                        out.colorFormat[c] = vals[0];
                        if (vals[0])
                            out.colorTargets |= uint8_t(1u << c);
                    }
                    if (count >= 3) {
                        ParsedColorKey &ck = out.colors[c];
                        ck.format = vals[0];
                        ck.writeMask = vals[1];
                        ck.blending = vals[2];
                        ck.srcRGB = vals[3];
                        ck.dstRGB = vals[4];
                        ck.rgbOp = vals[5];
                        ck.srcAlpha = vals[6];
                        ck.dstAlpha = vals[7];
                        ck.alphaOp = vals[8];
                        ck.fieldCount = count;
                    }
                }
            } else if (!out.hasA0 && len >= 3 && tok[0] == 'a' && tok[1] == '0' && tok[2] == '=') {
                const char *q = tok + 3;
                uint32_t v[5] = {};
                if (parseKeyUint(q, v[0]) && *q++ == ':' && parseKeyUint(q, v[1]) && *q++ == ':' &&
                    parseKeyUint(q, v[2]) && *q++ == ':' && parseKeyUint(q, v[3]) && *q++ == ':' &&
                    parseKeyUint(q, v[4])) {
                    out.hasA0 = true;
                    out.a0Format = v[0];
                    out.a0Offset = v[1];
                    out.a0Buffer = v[2];
                    out.a0Stride = v[3];
                    out.a0Step = v[4];
                }
            } else if (len >= 6 && !memcmp(tok, "rast=0", 6)) {
                out.rasterOff = true;
            }
            if (len == 6 && *p == ' ' && !memcmp(tok, "ds=0/0", 6))
                out.hasZeroDsToken = true;
        }
        if (!out.hasDs && len >= 3 && tok[0] == 'd' && tok[1] == 's' && tok[2] == '=') {
            out.hasDs = true;
            const char *q = tok + 3;
            uint32_t d = 0, s = 0;
            if (parseKeyUint(q, d)) {
                out.depth = d;
                if (*q == '/') {
                    ++q;
                    if (parseKeyUint(q, s))
                        out.stencil = s;
                }
            }
        } else if (!seenSamples && len >= 8 && !memcmp(tok, "samples=", 8)) {
            seenSamples = true;
            const char *q = tok + 8;
            uint32_t s = 0;
            if (parseKeyUint(q, s) && s > 1)
                out.samples = s;
        }
        if (*p == ' ')
            ++p;
    }
    return out;
}

// The colour format of target `j` in a state key ("... c<j>=<MTLPixelFormat>:..."), 0 without one
inline uint32_t keyColorFormat(const char *keyString, uint32_t j)
{
    if (!keyString || j >= 8)
        return 0;
    const char target = char('0' + j);
    for (const char *p = keyString; *p; ++p) {
        if (*p != ' ' || p[1] != 'c' || p[2] != target || p[3] != '=')
            continue;
        const char *q = p + 4;
        uint32_t format = 0;
        return parseKeyUint(q, format) ? format : 0;
    }
    return 0;
}

// The colour targets a state key names, bit j its c<j> (record_parts.h targets for a key the runtime holds as a string)
inline uint8_t keyColorTargets(const char *keyString)
{
    if (!keyString)
        return 0;
    uint8_t targets = 0, seen = 0;
    for (const char *p = keyString; *p; ++p) {
        if (*p != ' ' || p[1] != 'c' || p[2] < '0' || p[2] > '7' || p[3] != '=')
            continue;
        const uint32_t j = uint32_t(p[2] - '0');
        if (seen >> j & 1)
            continue;
        seen |= uint8_t(1u << j);
        const char *q = p + 4;
        uint32_t format = 0;
        if (parseKeyUint(q, format) && format)
            targets |= uint8_t(1u << j);
    }
    return targets;
}

// SET_ITERATED_BLEND_OPTIMIZATION's value for a state: its blob's (method, value) pairs, its targets' SET_CT_WRITE values
// after them, its key and the targets its program writes as storage images
inline uint32_t iteratedBlendNoop(const uint32_t *blob, uint32_t dwords, const uint32_t writes[8],
                                  const char *keyString, uint8_t storageBits,
                                  const uint32_t *defaultPerTarget = nullptr, uint8_t defaultSeen = 0,
                                  const ParsedPipelineKey *parsed = nullptr)
{
    if (!(writes[0] & 0x1111) || (storageBits & 1) || !keyString)
        return kNoopNever;
    const uint8_t targets = parsed ? parsed->colorTargets : keyColorTargets(keyString);
    for (uint32_t j = 1; j < 8; j++)
        if ((writes[j] & 0x1111) && (targets >> j & 1))
            return kNoopNever;
    bool blends = false;
    uint32_t perTarget[6] = {};
    bool seen[6] = {};
    if (defaultPerTarget) {
        for (uint32_t i = 0; i < 6; i++)
            if (defaultSeen >> i & 1) {
                perTarget[i] = defaultPerTarget[i];
                seen[i] = true;
            }
    }
    for (uint32_t k = 0; k + 1 < dwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
        if (m == 0x153c && (v & 0x11))  // SET_ANTI_ALIAS_ALPHA_CONTROL: ALPHA_TO_COVERAGE, ALPHA_TO_ONE
            return kNoopNever;
        if (m == 0x1360)  // SET_BLEND(0)
            blends = v & 1;
        if (m >= 0x1e04 && m <= 0x1e18) {  // SET_BLEND_PER_TARGET_COLOR_OP(0) .. ALPHA_DEST_COEFF(0)
            perTarget[(m - 0x1e04) / 4] = v;
            seen[(m - 0x1e04) / 4] = true;
        }
    }
    for (bool s : seen)
        if (!s)
            return kNoopNever;
    const uint32_t format = parsed ? parsed->colorFormat[0] : keyColorFormat(keyString, 0);
    const bool normalized = normalizedFormat(format);
    for (uint32_t half = 0; half < 2; half++) {
        const uint32_t *t = perTarget + 3 * half;  // op, source coefficient, destination coefficient
        if (!noopBlendOp(t[0]) || !noopSourceCoeff(t[1], normalized) || !noopDestCoeff(t[2]))
            return kNoopNever;
    }
    return blends ? kNoopSourceRgba0000 : kNoopNever;
}

}  // namespace gm20b
