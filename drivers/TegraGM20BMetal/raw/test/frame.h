// SPDX-License-Identifier: MIT
// raw GM20B path (r7-raw, r8-rawca): one Core Animation shaped frame, described once and run by two drivers over the same mock GPU and decoder:
// test/nvkframe.mm (the shipped frontend + m2v + NVK twin) and test/rawframe.cpp (the raw path). Same states (corpus R lines), same
// resources, passes, draws, bytes, textures, samplers and scissors, so the two decoded streams must describe the same GPU work.
//
// Shape (r4-encode's standard CA frame, cabench.mm): `blurPasses` 320x180 passes of one blur draw each, then the 1280x720 main pass
// (target = an IOSurface, second attachment memoryless and never written) of `draws` draws cycling through `mainStates` uber states,
// each with CA's bindings: vertices (setVertexBytes, the state's vertex slot), uniforms at vertex 2, 3 and fragment 1, 2, 5, 6,
// fragment textures 3, 4, 9, sampler 1, and a scissor per draw.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace frame {

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

// The state key of an R line, as capture/probe.mm and r6-precompile's precompile.mm make it: every descriptor field but the label,
// then " | v ... | f ..." with the functions and their constants.
inline std::string key(const std::string &line)
{
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "),
           bar3 = line.find(" | ", bar2 == std::string::npos ? 0 : bar2 + 5);
    if (bar1 == std::string::npos || bar2 == std::string::npos)
        return "";
    std::string k;
    for (const std::string &h : split(line.substr(0, bar1))) {
        const bool field = h.rfind("samples=", 0) == 0 || h.rfind("topo=", 0) == 0 || h.rfind("ds=", 0) == 0 ||
                           (h.size() > 1 && (h[0] == 'c' || h[0] == 'a') && isdigit(h[1]));
        if (field)
            k += h + " ";
    }
    k += line.substr(bar1, (bar3 == std::string::npos ? line.size() : bar3) - bar1);
    while (!k.empty() && k.back() == ' ')
        k.pop_back();  // the pack's keys carry no trailing blank (tools/probelog.py)
    return k;
}

inline bool a3()
{
    return getenv("FRAME_A3") != nullptr;
}

// FRAME_542: the framebuffers are widget snapshots, RGB8_444_2P (542) textures over two-plane 4:4:4 '_RB4' surfaces, which
// the main pass's states (those with c0=542 in the corpus) render through the frontend's stand-in; FRAME_PITCH's rows apply too.
// FRAME_542=load: the main pass loads the snapshot instead of clearing it
inline bool snapshot()
{
    return getenv("FRAME_542") != nullptr;
}

inline bool snapshotLoads()
{
    return snapshot() && !strcmp(getenv("FRAME_542"), "load");
}

// FRAME_DS: the main pass draws Depth32Float_Stencil8 states (ds=260/260) beside a memoryless attachment of that format, cleared,
// under the depth/stencil states of dsSpec and changing stencil references; its first color is memoryless too (the raw path
// renders no pitch color beside a depth/stencil attachment)
inline bool depthStencil()
{
    return getenv("FRAME_DS") != nullptr;
}

// FRAME_QC27: iOS 27's Core Animation states (caspec27.py), whose second attachment is RGBA16Float and whose blur passes
// draw into RGBA16Float
inline bool qc27()
{
    return getenv("FRAME_QC27") != nullptr;
}

// FRAME_XR: the blur passes draw Core Animation's extended-range blur states (RGB10A8_2P_XR10, format 550, both attachments) into
// targets of that format, each sampling the one before; every fourth main draw samples the last of them
inline bool extendedRange()
{
    return getenv("FRAME_XR") != nullptr;
}

// FRAME_MAIN_FORMAT=<MTLPixelFormat>: the framebuffers are private textures of that format (iOS 27's offscreen passes:
// BGRA10_XR 552, BGR10_XR 554, RGB10A8_2P_XR10 550, RGBA16Float 115) and the main pass's states those whose first
// attachment it is
inline uint32_t mainFormat()
{
    const char *v = getenv("FRAME_MAIN_FORMAT");
    return v ? uint32_t(atoi(v)) : 0;
}

struct State {
    std::string line, key;
    bool uber, blur, extra = false;
};

// The corpus states the frame uses: BGRA8 (format 80) two-attachment states (FRAME_XR: the blurs' 550 ones).
inline std::vector<State> states(const char *corpusPath)
{
    std::vector<State> out;
    FILE *f = fopen(corpusPath, "r");
    if (!f)
        return out;
    char buf[16384];
    std::vector<std::string> seen;
    while (fgets(buf, sizeof buf, f)) {
        std::string l(buf);
        if (!l.empty() && l.back() == '\n')
            l.pop_back();
        if (l.rfind("R ", 0) || l.find(" ok=1 ") == std::string::npos)
            continue;
        if (l.find(" label=com.apple.coreanimation.") == std::string::npos)
            continue;  // the render server's (the pack's)
        for (size_t at; (at = l.find(" lib=")) != std::string::npos;)
            l.erase(at, l.find(' ', at + 1) - at);  // not in the key
        // FRAME_A3: the three-attachment states instead (a third R32Float attachment, c2=55: Core Animation's "A3" states,
        // r7n's harvest), drawn in a main pass with that third attachment
        const bool blurLine = l.find("v downsample_blur_vert_lph") != std::string::npos,
                   xrBlur = extendedRange() && blurLine;
        const std::string mainC0 = " c0=" + std::to_string(mainFormat()) + ":";
        const char *c0 = xrBlur                      ? " c0=550:"
                         : qc27() && blurLine        ? " c0=115:"
                         : mainFormat() && !blurLine ? mainC0.c_str()
                         : snapshot() && !blurLine   ? " c0=542:"
                                                     : " c0=80:",
                   *c1 = xrBlur   ? " c1=550:"
                         : qc27() ? " c1=115:"
                                  : " c1=80:";
        if (l.find(c0) == std::string::npos || l.find(c1) == std::string::npos ||
            (a3() && !xrBlur ? l.find(" c2=55:") == std::string::npos || l.find(" c3=") != std::string::npos
                             : l.find(" c2=") != std::string::npos))
            continue;
        State s{l, key(l), l.find("v fixed_vert_lph_spc") != std::string::npos,
                l.find("v downsample_blur_vert_lph") != std::string::npos};
        // the blur passes' states have no depth/stencil formats, the main pass's those of FRAME_DS
        if (l.find(s.blur || !depthStencil() ? " ds=0/0 " : " ds=260/260 ") == std::string::npos)
            continue;
        // FRAME_ALL_STATES: every state that is not a blur goes in the main pass's rotation (else only the uber states)
        if (getenv("FRAME_ALL_STATES") && !s.blur)
            s.uber = true;
        // FRAME_KEYS=<file>: only the states whose key is a line of it (the pack's: test/eq.sh writes them)
        static std::vector<std::string> only = [] {
            std::vector<std::string> keys;
            if (const char *path = getenv("FRAME_KEYS"))
                if (FILE *k = fopen(path, "r")) {
                    char line[16384];
                    while (fgets(line, sizeof line, k)) {
                        std::string x(line);
                        if (!x.empty() && x.back() == '\n')
                            x.pop_back();
                        keys.push_back(x);
                    }
                    fclose(k);
                }
            return keys;
        }();
        // FRAME_EXTRA: the states FRAME_KEYS leaves out are kept as extras (neither uber nor blur): nvkframe draws one of them in
        // an extra pass of every odd frame (a command buffer the raw facade hands to m2v, between raw ones)
        if (!only.empty() && std::find(only.begin(), only.end(), s.key) == only.end()) {
            if (!getenv("FRAME_EXTRA") || s.blur)
                continue;
            s.extra = true;
            s.uber = false;
        }
        bool dup = false;
        for (auto &k : seen)
            dup |= k == s.key;
        if (dup || s.key.empty())
            continue;
        seen.push_back(s.key);
        out.push_back(s);
    }
    fclose(f);
    return out;
}

enum Tex : int {
    FB0,
    FB1,
    MEMORYLESS,
    BLUR0,
    BLUR1,
    BLUR_MEMORYLESS,
    IMAGE0,
    IMAGE1,
    IMAGE2,
    IMAGE3,
    A3TARGET,
    DS_COLOR,
    ZETA,
    TEX_COUNT
};

struct TexDesc {
    int w, h;
    bool iosurface, memoryless, renderTarget;
    uint32_t format =
        80;  // MTLPixelFormat: BGRA8Unorm, R32Float (55) for the A3 states' third attachment, 550 FRAME_XR's blurs
};

// FRAME_PITCH: the surfaces' rows as clients' and the render server's can lie since r12a (its needed alignment back at 16): the
// framebuffers 1272 wide (rows of 5088 bytes, not a multiple of 128: a pitch target the raw path renders through a pool image)
// and IMAGE2 132 wide (rows of 528, not a multiple of 32: sampled through a pool image the copy engine fills)
inline bool pitchShapes(const char *which = nullptr)
{
    const char *v = getenv("FRAME_PITCH");
    return v && (!which || !strcmp(v, "1") || !strcmp(v, which));
}

inline TexDesc texture(int t)
{
    switch (t) {
    case FB0:
    case FB1:
        if (mainFormat())
            return {1280, 720, false, false, true, mainFormat()};
        return {pitchShapes("fb") ? 1272 : 1280, 720, true, false, true, snapshot() ? 542u : 80u};
    case MEMORYLESS:
        return {1280, 720, false, true, true, qc27() ? 115u : 80u};
    case BLUR0:
    case BLUR1:
        return {320, 180, false, false, true, extendedRange() ? 550u : qc27() ? 115u : 80u};
    case BLUR_MEMORYLESS:
        return {320, 180, false, true, true, extendedRange() ? 550u : qc27() ? 115u : 80u};
    case IMAGE0:
        return {1280, 720, true, false, false};
    case A3TARGET:
        return {1280, 720, false, false, true, 55};
    case DS_COLOR:
        return {1280, 720, false, true, true};
    case ZETA:
        return {1280, 720, false, true, true, 260};
    default:
        return {t == IMAGE2 && pitchShapes("image") ? 132 : 128, 128, t == IMAGE2, false, false};
    }
}

struct SamplerSpec {
    int minmag, mip, address;
};  // MTLSamplerMinMagFilter, MTLSamplerMipFilter, MTLSamplerAddressMode

// FRAME_MIP: IMAGE1 a mipmapped texture (private, every level: level 0 copied in and generateMipmaps at each frame's start), sampler
// 0 trilinear (Core Animation's minification of icons and cards)
inline bool mipImage()
{
    return getenv("FRAME_MIP") != nullptr;
}

inline SamplerSpec sampler(int s)
{
    return s == 0 ? SamplerSpec{1, mipImage() ? 2 : 0, 0} : SamplerSpec{0, 0, 2};
}

struct Bytes {
    int stage, slot;
    uint32_t length;
};

// An MTLDepthStencilDescriptor, enums as Metal numbers them: the depth compare and write, then per face (front, back) the stencil
// compare, failure, depth failure and pass operations and the read and write masks
struct DsSpec {
    uint8_t depthCompare;
    bool depthWrite;
    uint8_t face[2][4];
    uint32_t readMask[2], writeMask[2];
};

constexpr unsigned kDsSpecs = 4;

inline DsSpec dsSpec(unsigned i)
{
    static const DsSpec specs[kDsSpecs] = {
        {7, false, {{7, 0, 0, 0}, {7, 0, 0, 0}}, {~0u, ~0u}, {~0u, ~0u}},     // Metal's default
        {1, true, {{7, 0, 0, 2}, {7, 0, 0, 2}}, {0xff, 0xff}, {0xff, 0xff}},  // depth Less, stencil writes ref
        {7,
         false,
         {{2, 0, 0, 3}, {5, 1, 5, 7}},
         {0x0f, 0x3c},
         {0x1f0, 0xf00f}},                                                 // stencil tests, masks wider than 8 bits
        {6, false, {{0, 2, 0, 0}, {7, 0, 0, 0}}, {~0u, ~0u}, {~0u, ~0u}},  // depth GreaterEqual, front stencil Never
    };
    return specs[i % kDsSpecs];
}

struct Draw {
    int state;  // index into the uber or blur list
    bool blur;
    uint32_t scissor[4];
    std::vector<Bytes> bytes;
    int textures[16];                                             // Tex or -1
    int samplers[16];                                             // sampler spec index or -1
    uint32_t primitive = 4, first = 0, count = 4, instances = 1;  // MTLPrimitiveType (4 triangle strip)
    uint32_t baseInstance = 0;
    int ds = -1;                  // dsSpec index set before it (-1: none set)
    uint32_t stencilRef[2] = {};  // set before it where ds >= 0
    // set before it where raster: MTLCullMode, MTLWinding, MTLTriangleFillMode, and setDepthBias's factors
    bool raster = false;
    uint8_t cull = 0, winding = 0, fill = 0;
    float bias[3] = {};
};

// FRAME_RASTER: the main pass's draws cull no face, the front or the back, wind clockwise or counter-clockwise, some fill
// with lines, some are biased in depth and some draw lines (Core Animation culls a layer that is not doubleSided)
inline bool rasterState()
{
    return getenv("FRAME_RASTER") != nullptr;
}

struct Pass {
    int color[3];  // Tex (-1: none)
    int load[3];   // 0 dontCare 1 load 2 clear
    float clear[4];
    std::vector<Draw> draws;
    int depth = -1;  // Tex of the depth/stencil attachment, both aspects cleared to clearDepth and clearStencil
    double clearDepth = 1;
    uint32_t clearStencil = 0;
};

struct Shape {
    unsigned blurPasses = 8, draws = 100,
             mainStates = getenv("FRAME_STATES") ? unsigned(atoi(getenv("FRAME_STATES"))) : 12;
    // FRAME_UNIFORM_BYTES: the uniform payloads' length (with FRAME_RING_ALIGN=4, 52 binds them at offsets 4, 8 and 12 past 16)
    uint32_t uniformBytes = getenv("FRAME_UNIFORM_BYTES") ? uint32_t(atoi(getenv("FRAME_UNIFORM_BYTES"))) : 256,
             vertexBytes = 192;
    // FRAME_RING: every payload goes into one shared buffer at 256-aligned offsets, bound with set*Buffer:offset: at a pass's first
    // draw and set*BufferOffset: after (Core Animation's per-frame ring, cabench mode 16), instead of set*Bytes
    bool ring = getenv("FRAME_RING") != nullptr;
};

constexpr uint32_t kRingBytes = 8 << 20;

// Every buffer slot (Metal's 31 a stage) a pass's draws leave empty is bound to kZeroBytes of zeros at the pass's start: a
// state that reads a slot the frame does not fill reads zeros on either driver (m2v binds its own zeros there; raw binds
// nothing, so a read would be the GPU's of an invalid buffer)
constexpr int kBufferSlots = 31;
constexpr uint32_t kZeroBytes = 65536;

inline bool slotFilled(const Pass &pass, int stage, int slot)
{
    for (const Draw &d : pass.draws)
        for (const Bytes &b : d.bytes)
            if (b.stage == stage && b.slot == slot)
                return true;
    return false;
}

// FRAME_FILL: each frame's command buffer first fills a buffer over [kFillFrom, kFillBytes) with a byte of the frame
// (fillBuffer:range:value:: whole words, and ends that are not), and every main-pass draw reads its first fragment uniform
// slot from that buffer, so what the fill wrote is in the draws' decode
inline bool fillUniforms()
{
    return getenv("FRAME_FILL") != nullptr;
}

constexpr uint32_t kFillBytes = 4096, kFillFrom = 3;

inline uint8_t fillValue(unsigned frame)
{
    return uint8_t(0x30 + frame % 64);
}

// The fragment slot a main-pass draw reads from the filled buffer (FRAME_FILL): its first fragment payload's; -1: none
template <typename Draw> int fillSlot(const Draw &d)
{
    if (d.blur)
        return -1;
    for (const auto &b : d.bytes)
        if (b.stage == 2)
            return int(b.slot);
    return -1;
}

// FRAME_RTSIZE: the main pass's renderTargetWidth/Height 37 and 23 pixels short of its attachments' (the device's passes, r7o:
// "a render target size other than the attachment's"), so its render area, default viewport and scissor, clears and the
// scissors past it are the target size's
inline bool rtSize()
{
    return getenv("FRAME_RTSIZE") != nullptr;
}

constexpr uint32_t kRtShortW = 37, kRtShortH = 23;

// The frame's passes. `uniformBytes`: the setBytes length of every uniform slot (CA's are 64 and 128; 256 = a whole m2v upload chunk,
// so the decoded payloads hold no bytes past the Metal length, which m2v leaves stale in its recycled chunks).
inline std::vector<Pass> passes(unsigned frameIndex, const Shape &s, size_t blurCount, size_t uberCount)
{
    std::vector<Pass> out;
    for (unsigned p = 0; p < s.blurPasses && blurCount; p++) {
        // FRAME_BLUR_LOAD: the blur targets' load action (0 DontCare, 1 Load, 2 Clear to a color past [0, 1] in two channels)
        const int blurLoad = getenv("FRAME_BLUR_LOAD") ? atoi(getenv("FRAME_BLUR_LOAD")) : 0;
        Pass bp{{p & 1 ? BLUR1 : BLUR0, BLUR_MEMORYLESS, -1}, {blurLoad, 0, 0}, {0.25f, -0.5f, 1.125f, 0.75f}, {}};
        Draw d{int(p % blurCount), true, {0, 0, 320, 180}, {}, {}, {}};
        for (int &t : d.textures)
            t = -1;
        for (int &t : d.samplers)
            t = -1;
        d.bytes = {{1, 1, 192},
                   {1, 2, s.uniformBytes},
                   {1, 3, s.uniformBytes},
                   {2, 1, s.uniformBytes},
                   {2, 6, s.uniformBytes}};
        d.textures[3] = p ? (p & 1 ? BLUR0 : BLUR1) : IMAGE0;
        d.samplers[0] = 0;
        bp.draws.push_back(d);
        out.push_back(bp);
    }
    Pass mp{{frameIndex & 1 ? FB1 : FB0, MEMORYLESS, a3() ? A3TARGET : -1},
            {snapshotLoads() ? 1 : 2, 0, 2},
            {0, 0, 0, 1},
            {}};
    if (depthStencil()) {
        mp.color[0] = DS_COLOR;
        mp.depth = ZETA;
        mp.clearDepth = 0.75 + 0.125 * (frameIndex & 1);
        mp.clearStencil = 0x1a5 + frameIndex;  // (Metal keeps the low 8 bits)
    }
    for (unsigned i = 0; i < s.draws && uberCount; i++) {
        Draw d{int((i / 3) % std::min<size_t>(s.mainStates, uberCount)),
               false,
               {(i * 13) % 1000, (i * 7) % 500, 280, 220},
               {},
               {},
               {}};
        for (int &t : d.textures)
            t = -1;
        for (int &t : d.samplers)
            t = -1;
        d.bytes = {{1, 1, s.vertexBytes},  {1, 2, s.uniformBytes}, {1, 3, s.uniformBytes}, {2, 1, s.uniformBytes},
                   {2, 2, s.uniformBytes}, {2, 5, s.uniformBytes}, {2, 6, s.uniformBytes}};
        if (a3()) {  // the A3 states' vertex uniforms at 5 and fragment uniforms at 3 and 4 too
            d.bytes.push_back({1, 5, s.uniformBytes});
            d.bytes.push_back({2, 3, s.uniformBytes});
            d.bytes.push_back({2, 4, s.uniformBytes});
        }
        // FRAME_GLASS: fragment uniforms at 9 too, where iOS 27's Liquid Glass states read their material
        if (getenv("FRAME_GLASS"))
            d.bytes.push_back({2, 9, s.uniformBytes});
        // FRAME_DRAW_FORMS: also triangle lists, a nonzero vertexStart and two instances (the draw paths CA's strips never take)
        if (getenv("FRAME_DRAW_FORMS")) {
            if (i % 4 == 1) {
                d.primitive = 3;
                d.count = 3;
            }
            if (i % 5 == 2)
                d.first = 1, d.count = 3;
            if (i % 7 == 3)
                d.instances = 2;
            if (i % 9 == 4)
                d.baseInstance = 3;
        }
        d.textures[3] = IMAGE1 + int(i % 3);
        d.textures[4] = extendedRange() && i % 4 == 0 && s.blurPasses ? (s.blurPasses & 1 ? BLUR0 : BLUR1) : IMAGE2;
        d.textures[9] = IMAGE3;
        d.samplers[1] = 1;
        if (rasterState()) {
            d.raster = true;
            d.cull = uint8_t(i % 3);
            d.winding = uint8_t((i / 3) % 2);
            d.fill = uint8_t(i % 7 == 3);
            if (i % 5 == 1) {
                d.bias[0] = 0.25f * float(i % 9) - 1.0f;
                d.bias[1] = i % 2 ? 2.0f : 0.0f;
                d.bias[2] = i % 3 ? 0.0f : 0.5f;
            }
            if (i % 11 == 5)
                d.primitive = 1;  // MTLPrimitiveTypeLine
        }
        if (depthStencil() && i % 3 != 2) {
            d.ds = int((i / 3) % kDsSpecs);
            d.stencilRef[0] = (i * 37 + frameIndex) % 300;
            d.stencilRef[1] = (i * 11) % 300;
        }
        mp.draws.push_back(d);
    }
    out.push_back(mp);
    return out;
}

// FRAME_INDEXED: every third main draw indexed as Core Animation's meshes are (CA::OGL::MetalContext::draw: 16-bit indices
// written into its buffer pool at 4-byte alignment, drawIndexedPrimitives on triangles), at `at` in the index buffer, which
// it advances; with FRAME_INDEXED_FORMS also several instances, a base vertex and a base instance
struct Indexed {
    uint32_t count, offset, instances = 1, baseInstance = 0;
    int32_t baseVertex = 0;
};

inline bool indexedDraw(bool blur, uint32_t drawIndex)
{
    return getenv("FRAME_INDEXED") && !blur && drawIndex % 3 == 1;
}

// FRAME_INDEX_PAST: every other indexed draw from the 64 KiB index buffer's tail, its last two positions past the buffer's end
// (Metal fetches index 0 there), every fourth of those from an offset one byte past the index size's multiple (rounded down)
inline Indexed indexed(uint32_t drawIndex, uint8_t *buffer, uint32_t &at)
{
    constexpr uint32_t kBytes = 65536;
    static const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
    if (getenv("FRAME_INDEX_PAST") && drawIndex % 2 == 0) {
        memcpy(buffer + kBytes - 8, quad, 8);
        return Indexed{6, kBytes - 8 + (drawIndex % 8 == 0 ? 1u : 0u)};
    }
    if (at + 64 > kBytes - 8)
        at = 0;
    memcpy(buffer + at, quad, sizeof quad);
    Indexed x{6, at};
    if (getenv("FRAME_INDEXED_FORMS")) {
        if (drawIndex % 4 == 0)
            x.instances = 2;
        if (drawIndex % 4 == 2)
            x.count = 3, x.baseVertex = 1;  // the quad's vertices 1 to 3
        if (drawIndex % 5 == 3)
            x.baseInstance = 2;
    }
    at += 12 + (drawIndex % 5) * 4;  // offsets 4-byte aligned, as the buffer pool hands them out
    return x;
}

// Deterministic contents for a setBytes call: vertices (a quad, stride 48, moved per draw) or uniforms.
inline void fill(std::vector<uint8_t> &out, const Bytes &b, unsigned frameIndex, unsigned drawIndex)
{
    out.assign(b.length, 0);
    float *f = reinterpret_cast<float *>(out.data());
    const size_t n = b.length / 4;
    if (b.stage == 1 && b.slot == 1) {
        for (int v = 0; v < 4 && size_t(v * 12 + 11) < n; v++) {
            f[v * 12 + 0] = (v & 1 ? 0.5f : -0.5f) + 0.001f * drawIndex;
            f[v * 12 + 1] = (v & 2 ? 0.5f : -0.5f) - 0.0007f * frameIndex;
            f[v * 12 + 3] = 1.0f;
            f[v * 12 + 4] = float(v & 1);
            f[v * 12 + 5] = float((v >> 1) & 1);
            for (int k = 8; k < 12; k++)
                f[v * 12 + k] = 0.25f * k;
        }
        return;
    }
    for (size_t i = 0; i < n; i++)
        f[i] =
            float((i * 2654435761u + drawIndex * 40503u + frameIndex * 977u + b.slot * 31u + b.stage) % 1000) / 1000.0f;
}

}  // namespace frame
