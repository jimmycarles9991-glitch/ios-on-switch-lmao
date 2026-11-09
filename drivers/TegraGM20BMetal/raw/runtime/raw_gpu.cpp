// SPDX-License-Identifier: MIT
// See raw_gpu.h. Method numbers: NVIDIA's class headers as Mesa ships them (src/nouveau/headers/nvidia/classes, MIT). The texture
// header fill follows Mesa NIL's nvb097_fill_image_view_desc (pitch), the sampler fill Mesa NVK's nvk_sampler_get_header, the
// copies NVK's nouveau_copy_rect and nvk_cmd_copy_memory_ce (all MIT), restricted to what Core Animation uses. r7-raw wrote the
// first version (render encoder, pack replay) and checked it against NVK's stream; r8-rawca added the lifetimes, the pool, the
// imports, the copies and the abortable command buffer.
#include "../fx/fx_image.h"
#include "raw_clear.h"
#include "raw_follow.h"
#include "raw_gpu.h"
#include "raw_owned.h"
#include "raw_residual.h"
#include "raw_samples.h"
#include "raw_slm.h"
#include "raw_srgb_table.h"
#include "raw_stamps.h"
#include "raw_target.h"
#include "raw_tiled_cache.h"
#include "TegraGM20BLayout.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <os/log.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace raw {
namespace {
enum : uint32_t {
    WAIT_FOR_IDLE = 0x0110,
    INVALIDATE_SHADER_CACHES = 0x021c,
    COLOR_TARGET = 0x0800,
    VIEWPORT_SCALE_X = 0x0a00,
    VIEWPORT_CLIP_H = 0x0c00,
    CLEAR_RECT_H = 0x0d6c,
    VERTEX_ARRAY_START = 0x0d74,
    COLOR_CLEAR_VALUE = 0x0d80,
    INVALIDATE_SHADER_CACHES_NO_WFI = 0x0da4,
    PIXEL_SHADER_BARRIER = 0x0de0,
    NO_OPERATION = 0x0100,
    SET_FRONT_POLYGON_MODE = 0x0dac,
    SET_BACK_POLYGON_MODE = 0x0db0,
    SET_POLY_OFFSET_POINT = 0x0dc0,
    SET_POLY_OFFSET_LINE = 0x0dc4,
    SET_POLY_OFFSET_FILL = 0x0dc8,
    SET_SLOPE_SCALE_DEPTH_BIAS = 0x156c,
    SET_DEPTH_BIAS = 0x15bc,
    SET_DEPTH_BIAS_CLAMP = 0x187c,
    OGL_SET_CULL = 0x1918,
    OGL_SET_FRONT_FACE = 0x191c,
    OGL_SET_CULL_FACE = 0x1920,
    PIXEL_SHADER_INTERLOCK_CONTROL = 0x1224,
    DEPTH_TEST = 0x12cc,
    STENCIL_TEST = 0x1380,
    ANTI_ALIAS_ALPHA_CONTROL = 0x153c,
    SET_ANTI_ALIAS = 0x15d0,
    ANTI_ALIAS_SAMPLE_POSITIONS = 0x11e0,
    HYBRID_ANTI_ALIAS_CONTROL = 0x0754,
    // Not in NVIDIA's published class headers: the texture data cache invalidate that NVN (deko3d's ORDER_FRAGMENTS
    // barrier) and NVIDIA's Linux driver put after PIXEL_SHADER_BARRIER, which orders it after the earlier pixels. The
    // _NO_WFI form acts as the front end reads it, while those pixels may still be shading or in the ROP.
    INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED = 0x0f74,
    SCISSOR_ENABLE = 0x0e00,
    SURFACE_CLIP_H = 0x0ff4,
    CLEAR_SURFACE_CONTROL = 0x10f8,
    CT_SELECT = 0x121c,
    INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI = 0x1288,
    INVALIDATE_SAMPLER_CACHE_NO_WFI = 0x1424,
    INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI = 0x1428,
    GLOBAL_BASE_INSTANCE_INDEX = 0x1438,
    GLOBAL_BASE_VERTEX_INDEX = 0x1434,
    VERTEX_ID_BASE = 0x1118,
    ZT_SELECT = 0x1538,
    END = 0x1614,
    BEGIN = 0x1618,
    DA_PRIMITIVE_RESTART = 0x1644,
    RENDER_ENABLE_OVERRIDE = 0x1944,
    PRIMITIVE_TOPOLOGY = 0x1970,
    CLEAR_SURFACE = 0x19d0,
    BLEND_CONST_RED = 0x131c,
    COLOR_COMPRESSION = 0x19e0,
    CT_WRITE = 0x1a00,
    VERTEX_STREAM_LOCATION = 0x1c04,
    VERTEX_STREAM_LIMIT = 0x1f00,
    CB_SELECTOR_A = 0x2380,
    LOAD_CB_OFFSET = 0x238c,
    LOAD_CB = 0x2390,
    BIND_GROUP_CB = 0x2410,
    INDEX_BUFFER_A = 0x17c8,
    INDEX_BUFFER_E = 0x17d8,
    INDEX_BUFFER_F = 0x17dc,
    DA_PRIMITIVE_RESTART_INDEX = 0x1648,
    // MAXWELL_DMA_COPY_A (NV90B5 layout)
    CE_LAUNCH_DMA = 0x0300,
    CE_OFFSET_IN_UPPER = 0x0400,
    CE_PITCH_IN = 0x0410,
    CE_LINE_LENGTH_IN = 0x0418,
    CE_SET_REMAP_CONST_A = 0x0700,
    CE_SET_REMAP_COMPONENTS = 0x0708,
    CE_SET_DST_BLOCK_SIZE = 0x070c,  // then DST_WIDTH, HEIGHT, DEPTH, LAYER, ORIGIN
    CE_SET_SRC_BLOCK_SIZE = 0x0728,  // then SRC_WIDTH, HEIGHT, DEPTH, LAYER, ORIGIN
};

constexpr uint32_t kRootBytes = 0x800, kSetBytes = 0x100, kUboChunk = 0x100;
constexpr uint32_t kMaxCbufBytes = 0x10000;        // NVK_MAX_CBUF_SIZE: the most of a constant buffer NVK binds
constexpr uint32_t kRootInterlockTickets = 0x260;  // offsetof(nvk_root_descriptor_table, interlock_ticket_buffer)
constexpr uint32_t kTicCount = 8192, kTscCount = 1024;
constexpr uint32_t kEngines =
    TegraGM20BEngine3D | TegraGM20BEngineCompute | TegraGM20BEngineCopy | TegraGM20BEngine2D | TegraGM20BEngineM2MF;
constexpr size_t kDedicatedSpares = 2;
// The shader heap: the programs of the states a process uses, placed at first use (Device::place); the pack holds tens of MiB
// of programs, of which a process uses a few hundred. Its GPU VA is the whole program window: SET_PIPELINE_PROGRAM is a 32-bit
// offset from SET_PROGRAM_REGION, so every program lies in the 4 GiB after it (NVK's contiguous shader arena before Volta is
// that window too). Only VA is held; parts of host memory are bound in it as programs are placed, with room after the last
// one for the GPU's instruction prefetch past a program's end. The render server binds kHeapAtOpen at its start: what Core
// Animation's states place in their first frames (17's journal: 1.13 MiB).
constexpr uint64_t kProgramWindow = 1ull << 32, kHeapStep = 256 << 10, kHeapSlack = 0x1000, kHeapAtOpen = 2 << 20;
// A client's first chunk of a kind, and its first heap part, as a share of the role's size: a widget's few surfaces and states
// fit, and jetsam limits of a few tens of MB leave nothing for a render server's steps.
constexpr uint64_t kClientFirstShare = 16, kClientHeapFirstShare = 4;
// INVALIDATE_SHADER_CACHES and its _NO_WFI form (cl9097.h, cla097.h): INSTRUCTION bit 0, FLUSH_DATA 2, DATA 4, CONSTANT 12
constexpr uint32_t kInvalidateInstruction = 1u << 0, kFlushData = 1u << 2, kInvalidateData = 1u << 4,
                   kInvalidateConstant = 1u << 12;
// Between passes: the shaders' data writes flushed, their data caches invalidated, and the constant caches too: bytes the copy
// engine, the 2D engine or a shader wrote since the submission's prologue may be bound as a constant buffer after, and the SMs'
// constant caches do not see such writes (NVK opens each of m2v's encoders, a command buffer of its own, with a constant
// invalidate, and its transfer-write to uniform-read barrier has one)
constexpr uint32_t kBetweenPasses = kFlushData | kInvalidateData | kInvalidateConstant;
constexpr uint32_t kMaxSegments = TegraGM20BMaxPushes;
// An instance of a draw: BEGIN with INSTANCE_ID SUBSEQUENT (two words), the first vertex or index and the count (three), END
constexpr uint32_t kInstanceWords = 6;
// NVK's copies of pitch memory (nouveau_copy_rect: LAUNCH_DMA PIPELINED, FLUSH_ENABLE, SRC/DST_MEMORY_LAYOUT PITCH, MULTI_LINE_ENABLE)
constexpr uint32_t kLaunchPitch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 9;

#ifdef SWITCH_RAW_HARNESS_MARKS
// The host harnesses' builds only (test/build.sh): a push that ran past its reservation stops them where it was closed
void checkRoom(const Push &p, const char *what)
{
    if (p.at > p.end) {
        fprintf(stderr, "raw: %s wrote %td words past its push reservation\n", what, p.at - p.end);
        abort();
    }
}
#else
void checkRoom(const Push &, const char *)
{}
#endif

uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// A pass's sample layout (raw_samples.h): its registers and the root table's sample words, the root table left selected
constexpr uint32_t kSampleWords = 20;

void sampleWords(Push &p, uint32_t samples, uint64_t root)
{
    SampleLayout l;
    sampleLayout(samples, l);
    p.m(SET_ANTI_ALIAS, l.antiAlias);
    p.incArray(ANTI_ALIAS_SAMPLE_POSITIONS, l.positions, 4);
    p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(root >> 32), uint32_t(root)});
    p.m(LOAD_CB_OFFSET, kRootSampleInfo);
    p.nonInc(LOAD_CB, l.root, kRootSampleInfoWords);
}

// Texture header word 0 per MTLPixelFormat as NVK writes it (pack/mkformats.py over the twin, 11-04), and the color target format
// NVK writes for a target of it (raw/test/targetcheck.py over the probe's PROBE_TARGETS run; 0 where m2v's NVK renders none).
const FormatInfo kFormats[] = {
    {1, 0xf7, 0x2002491d, 1, false},     // A8Unorm
    {10, 0xf3, 0x7012491d, 1, false},    // R8Unorm
    {11, 0, 0x7012491d, 1, true},        // R8Unorm_sRGB
    {20, 0xee, 0x7012491b, 2, false},    // R16Unorm
    {23, 0, 0x6014921b, 2, false},       // R16Uint (NIL's R16_UINT: UINT components, alpha ONE_INT; sampled only)
    {25, 0xf2, 0x7017ff9b, 2, false},    // R16Float
    {30, 0xea, 0x70d24918, 2, false},    // RG8Unorm
    {40, 0xe8, 0x74e24915, 2, false},    // B5G6R5Unorm
    {41, 0, 0x272a4913, 2, false},       // A1BGR5Unorm
    {42, 0, 0x272a4912, 2, false},       // ABGR4Unorm
    {43, 0xe9, 0x54e24914, 2, false},    // BGR5A1Unorm
    {55, 0xe5, 0x7017ff8f, 4, false},    // R32Float
    {60, 0xda, 0x70d2490c, 4, false},    // RG16Unorm
    {65, 0xde, 0x70d7ff8c, 4, false},    // RG16Float
    {70, 0xd5, 0x58d24908, 4, false},    // RGBA8Unorm (rendered: a 4:2:0 code texture's image)
    {71, 0xd6, 0x58d24908, 4, true},     // RGBA8Unorm_sRGB
    {80, 0xcf, 0x54e24908, 4, false},    // BGRA8Unorm
    {81, 0xd0, 0x54e24908, 4, true},     // BGRA8Unorm_sRGB
    {90, 0xd1, 0x58d24909, 4, false},    // RGB10A2Unorm (rendered: a 10-bit code texture's image)
    {92, 0xe0, 0x78d7ffa1, 4, false},    // RG11B10Float
    {93, 0, 0x78d7ffa0, 4, false},       // RGB9E5Float
    {94, 0xdf, 0x54e24909, 4, false},    // BGR10A2Unorm
    {105, 0xcb, 0x70d7ff84, 8, false},   // RG32Float
    {110, 0xc6, 0x58d24903, 8, false},   // RGBA16Unorm
    {115, 0xca, 0x58d7ff83, 8, false},   // RGBA16Float (rendered: a 4:2:0 texture's converted image)
    {125, 0xc0, 0x58d7ff81, 16, false},  // RGBA32Float (rendered: an sRGB 4:2:0 texture's image)
    // block-compressed (sampled only, private: Metal's copies move whole blocks), word 0 NIL's row of NVK's format
    {130, 0, 0x58d24924, 8, false, 4, 4},     // BC1_RGBA: DXT1_RGBA
    {131, 0, 0x58d24924, 8, true, 4, 4},      // BC1_RGBA_sRGB: DXT1_SRGBA
    {132, 0, 0x58d24925, 16, false, 4, 4},    // BC2_RGBA: DXT3_RGBA
    {133, 0, 0x58d24925, 16, true, 4, 4},     // BC2_RGBA_sRGB: DXT3_SRGBA
    {134, 0, 0x58d24926, 16, false, 4, 4},    // BC3_RGBA: DXT5_RGBA
    {135, 0, 0x58d24926, 16, true, 4, 4},     // BC3_RGBA_sRGB: DXT5_SRGBA
    {140, 0, 0x70124927, 8, false, 4, 4},     // BC4_RUnorm: RGTC1_UNORM
    {141, 0, 0x701124a7, 8, false, 4, 4},     // BC4_RSnorm: RGTC1_SNORM
    {142, 0, 0x70d24928, 16, false, 4, 4},    // BC5_RGUnorm: RGTC2_UNORM
    {143, 0, 0x70d124a8, 16, false, 4, 4},    // BC5_RGSnorm: RGTC2_SNORM
    {150, 0, 0x78d7ff90, 16, false, 4, 4},    // BC6H_RGBFloat: BPTC_RGB_FLOAT
    {151, 0, 0x78d7ff91, 16, false, 4, 4},    // BC6H_RGBUfloat: BPTC_RGB_UFLOAT
    {152, 0, 0x58d24917, 16, false, 4, 4},    // BC7_RGBAUnorm: BPTC_RGBA_UNORM
    {153, 0, 0x58d24917, 16, true, 4, 4},     // BC7_RGBAUnorm_sRGB: BPTC_SRGBA
    {186, 0, 0x58d24940, 16, true, 4, 4},     // ASTC_4x4_sRGB: ASTC_4x4_SRGB
    {187, 0, 0x58d24950, 16, true, 5, 4},     // ASTC_5x4_sRGB: ASTC_5x4_SRGB
    {188, 0, 0x58d24941, 16, true, 5, 5},     // ASTC_5x5_sRGB: ASTC_5x5_SRGB
    {189, 0, 0x58d24951, 16, true, 6, 5},     // ASTC_6x5_sRGB: ASTC_6x5_SRGB
    {190, 0, 0x58d24942, 16, true, 6, 6},     // ASTC_6x6_sRGB: ASTC_6x6_SRGB
    {192, 0, 0x58d24955, 16, true, 8, 5},     // ASTC_8x5_sRGB: ASTC_8x5_SRGB
    {193, 0, 0x58d24952, 16, true, 8, 6},     // ASTC_8x6_sRGB: ASTC_8x6_SRGB
    {194, 0, 0x58d24944, 16, true, 8, 8},     // ASTC_8x8_sRGB: ASTC_8x8_SRGB
    {195, 0, 0x58d24956, 16, true, 10, 5},    // ASTC_10x5_sRGB: ASTC_10x5_SRGB
    {196, 0, 0x58d24957, 16, true, 10, 6},    // ASTC_10x6_sRGB: ASTC_10x6_SRGB
    {197, 0, 0x58d24953, 16, true, 10, 8},    // ASTC_10x8_sRGB: ASTC_10x8_SRGB
    {198, 0, 0x58d24945, 16, true, 10, 10},   // ASTC_10x10_sRGB: ASTC_10x10_SRGB
    {199, 0, 0x58d24954, 16, true, 12, 10},   // ASTC_12x10_sRGB: ASTC_12x10_SRGB
    {200, 0, 0x58d24946, 16, true, 12, 12},   // ASTC_12x12_sRGB: ASTC_12x12_SRGB
    {204, 0, 0x58d24940, 16, false, 4, 4},    // ASTC_4x4_LDR: ASTC_4x4
    {205, 0, 0x58d24950, 16, false, 5, 4},    // ASTC_5x4_LDR: ASTC_5x4
    {206, 0, 0x58d24941, 16, false, 5, 5},    // ASTC_5x5_LDR: ASTC_5x5
    {207, 0, 0x58d24951, 16, false, 6, 5},    // ASTC_6x5_LDR: ASTC_6x5
    {208, 0, 0x58d24942, 16, false, 6, 6},    // ASTC_6x6_LDR: ASTC_6x6
    {210, 0, 0x58d24955, 16, false, 8, 5},    // ASTC_8x5_LDR: ASTC_8x5
    {211, 0, 0x58d24952, 16, false, 8, 6},    // ASTC_8x6_LDR: ASTC_8x6
    {212, 0, 0x58d24944, 16, false, 8, 8},    // ASTC_8x8_LDR: ASTC_8x8
    {213, 0, 0x58d24956, 16, false, 10, 5},   // ASTC_10x5_LDR: ASTC_10x5
    {214, 0, 0x58d24957, 16, false, 10, 6},   // ASTC_10x6_LDR: ASTC_10x6
    {215, 0, 0x58d24953, 16, false, 10, 8},   // ASTC_10x8_LDR: ASTC_10x8
    {216, 0, 0x58d24945, 16, false, 10, 10},  // ASTC_10x10_LDR: ASTC_10x10
    {217, 0, 0x58d24954, 16, false, 12, 10},  // ASTC_12x10_LDR: ASTC_12x10
    {218, 0, 0x58d24946, 16, false, 12, 12},  // ASTC_12x12_LDR: ASTC_12x12
    // the extended-range formats' codes (raw_xr.h), as m2v's NVK renders and fetches them: RG32_UINT, RGBA16_UINT, R32_UINT
    {550, 0xcd, 0x60d49204, 8, false},  // RGB10A8_2P_XR10
    {552, 0xc9, 0x58d49203, 8, false},  // BGRA10_XR
    {554, 0xe4, 0x6014920f, 4, false},  // BGR10_XR
};

// bits [lo, hi] of the 256-bit header
void setField(uint32_t *w, unsigned hi, unsigned lo, uint64_t v)
{
    for (unsigned b = lo; b <= hi; b++, v >>= 1) {
        uint32_t &word = w[b / 32];
        const uint32_t bit = 1u << (b % 32);
        word = (v & 1) ? (word | bit) : (word & ~bit);
    }
}

size_t pageSize()
{
    static const size_t page = size_t(getpagesize());
    return page;
}

// CPU writes to cached memory the GPU reads (an import), out to memory
void cleanToMemory(const void *at, uint64_t bytes)
{
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(at) & ~uintptr_t(63); a < uintptr_t(at) + bytes; a += 64)
        __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)at;
    (void)bytes;
#endif
}
}  // namespace

// The render server's: what one 720p screen's composition holds. A client's: the most it grows a chunk to (chunkSize).
const Device::Sizes Device::kSizes[2] = {
    {16 << 20, 16 << 20, 4 << 20, 2 << 20, 4 << 20},
    {4 << 20, 4 << 20, 1 << 20, 256 << 10, 512 << 10},
};

const FormatInfo *formatInfo(uint32_t mtl)
{
    for (const FormatInfo &f : kFormats)
        if (f.mtl == mtl)
            return &f;
    return nullptr;
}

uint32_t nvTopology(uint32_t mtl)
{
    static const uint32_t map[] = {1, 2, 3, 4,
                                   5};  // point, line, lineStrip, triangle, triangleStrip -> V_POINTLIST..TRIANGLESTRIP
    return mtl < 5 ? map[mtl] : 4;
}

// ---- device: rings ----

uint8_t *Device::alloc(Ring &r, uint32_t bytes, uint32_t align, uint64_t &va)
{
    uint64_t at = (r.head + align - 1) & ~uint64_t(align - 1);
    if (at + bytes > r.bo.bytes)
        at = 0;  // wrap: the tail [head, end) counts as used by the open command buffer
    // Below Ring::clear no span is in flight, the open command buffer's own included: no look at them there
    if (overflowing_ || ((at < r.head || at + bytes > r.clear) && ownWords(r, at, bytes) && !outgrow(r, bytes, at))) {
        overflowing_ = true;  // no larger ring: the command buffer is written to nowhere, and its commit refused
        overflow_.emplace_back(bytes);  // a block of its own: the encoder may still be writing an earlier one
        va = 0;
        return overflow_.back().data();
    }
    if (at < r.head || at + bytes > r.clear) {
        // [at, at + bytes) may still be read by an earlier submission: retire what the GPU finished (the fence page, no call)
        // and block in the kernel (WaitFence: the fence interrupt) for what overlaps. Sized so a Core Animation frame never
        // waits.
        for (auto it = r.inflight.begin(); it != r.inflight.end();) {
            if (it->seqno && done(it->seqno)) {
                it = r.inflight.erase(it);
                continue;
            }
            if (it->begin < at + bytes && at < it->end && it->seqno) {
                stats.ringWaits++;
                r.waited = true;
                wait(it->seqno);
                it = r.inflight.erase(it);
                continue;
            }
            ++it;
        }
        if (at < r.head && r.since < r.head) {  // wrapped inside one command buffer: keep the part before the wrap
            r.inflight.push_back({r.since, r.bo.bytes, 0});
            r.since = 0;
        }
        // spans are added behind head only (retire, the wrap above), so the nearest one ahead bounds every allocation until
        // the next wrap
        r.clear = r.bo.bytes;
        for (const Ring::Span &s : r.inflight)
            if (s.begin >= at + bytes)
                r.clear = std::min(r.clear, s.begin);
    }
    r.head = at + bytes;
    va = r.bo.va + at;
    return r.bo.cpu + at;
}

// Whether [at, at + bytes) holds words the open command buffer wrote: it has gone round the whole ring
bool Device::ownWords(const Ring &r, uint64_t at, uint64_t bytes) const
{
    if (at < r.head && r.since < at + bytes && r.since < r.head)
        return true;
    for (const Ring::Span &s : r.inflight)
        if (!s.seqno && s.begin < at + bytes && at < s.end)
            return true;
    return false;
}

// The open command buffer is larger than the ring: it goes on in one twice the size (or its allocation's), from its start; the
// old one is freed once the work that reads it, the open command buffer's included, completed. false: no memory for it
bool Device::outgrow(Ring &r, uint32_t bytes, uint64_t &at)
{
    Bo larger;
    if (!k_.alloc(std::max<uint64_t>(r.bo.bytes * 2, uint64_t(bytes) * 2), true, larger))
        return false;
    stats.ringsOutgrown++;
    r.outgrown.push_back(r.bo);
    r.bo = larger;
    r.head = r.since = r.clear = 0;
    r.inflight.clear();
    at = 0;
    return true;
}

void Device::retire(Ring &r, uint32_t seqno)
{
    for (const Bo &b : r.outgrown)
        grownOut_.push_back({b, seqno});
    r.outgrown.clear();
    while (!r.inflight.empty() && r.inflight.front().seqno && done(r.inflight.front().seqno))
        r.inflight.pop_front();
    for (auto &s : r.inflight)
        if (!s.seqno)
            s.seqno = seqno;
    if (r.head > r.since)
        r.inflight.push_back({r.since, r.head, seqno});
    r.since = r.head;
}

// ---- device: open ----

Device::~Device()
{
    // the render server keeps its device until it exits; a host test's device: the kernel frees what the connection held
    {
        std::lock_guard<std::mutex> l(lock_);
        stopping_ = true;
    }
    grow_.notify_all();
    if (grower_.joinable())
        grower_.join();
    compStop();
}

uint32_t Device::contextWords() const
{
    return uint32_t(pack_.bytes(PackInit) / sizeof(PackInitMethod)) * 2 + 4096 + kDepthStencilWords;
}

// The channel's context: NVK's own init methods with our addresses, then every register Core Animation's states leave the
// same. A channel the kernel kills takes its context with it (reopen).
void Device::emitContext(Push &p)
{
    const uint32_t initCount = uint32_t(pack_.bytes(PackInit) / sizeof(PackInitMethod));
    const PackInitMethod *im = pack_.section<PackInitMethod>(PackInit);
    const InitAddresses here = {heap_.va, zero_.va, root_.va, tic_.va, tsc_.va, kTicCount - 1, kTscCount - 1};
    for (uint32_t i = 0; i < initCount; i++) {
        const uint32_t d = relocated(im[i], here);
        p.m(im[i].mthd, d, im[i].subc);
    }
    const uint32_t *base = pack_.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack_.bytes(PackBaseline) / 8; i++)
        if (!ownedMethod(base[2 * i]))
            p.m(base[2 * i], base[2 * i + 1]);
    tiledCacheInit(p, k_.info());
    // the depth/stencil registers (the runtime's, as NVK leaves them for Metal's default state in a pass without the attachment)
    {
        DepthStencilRegs unknown;
        for (uint32_t &v : unknown.v)
            v = ~0u;
        emitDepthStencil(p, depthStencilRegs(DepthStencilDesc(), 0, 0, false, false), unknown);
        ds_ = unknown;
    }
    // root table selected for LOAD_CONSTANT_BUFFER from here on (draws reselect it after binding set 0 as a cbuf); NVK's init
    // above zeroed it, so one sample's layout goes in now, as every pass but a multisampled one leaves it
    sampleWords(p, 1, root_.va);
    // the pixel interlock's ticket buffer (nvk_root_descriptor_table.interlock_ticket_buffer; NAK's lowering waits on and
    // releases its words, 256 per GPC): zero, as this channel's ticket dispenser starts. NVK's init above may name the probe's.
    {
        const uint32_t a[2] = {uint32_t(tickets_.va), uint32_t(tickets_.va >> 32)};
        p.m(LOAD_CB_OFFSET, kRootInterlockTickets);
        p.nonInc(LOAD_CB, a, 2);
    }
}

bool Device::open(const char *packPath, uint64_t stack, std::string &err, Role role)
{
    role_ = role;
    sizes_ = kSizes[role == Role::RenderServer ? 0 : 1];
    if (!pack_.load(packPath, stack, err))
        return false;
    if (!k_.open(err))
        return false;
    if (k_.info().cls_eng3d != 0xb197 || k_.info().cls_copy != 0xb0b5) {
        err = "not a MAXWELL_B with MAXWELL_DMA_COPY_A";
        return false;
    }
    return init(err);
}

// Every process's: the header pools, the root table, the zero page, the interlock tickets and the shader heap's reservation,
// which textures, samplers and states need before any submission. The render server's channel, heap and first chunks too.
bool Device::init(std::string &err)
{
    if (!k_.alloc(kTicCount * 32, true, tic_) || !k_.alloc(kTscCount * 32, true, tsc_) ||
        !k_.alloc(0x1000, true, root_) || !k_.alloc(0x10000, true, zero_) || !k_.alloc(0x1000, true, tickets_) ||
        !k_.alloc(kColorTableBytes, true, colorTable_) || !k_.alloc(kStampSlots * sizeof(StampSlot), true, stamps_)) {
        err = k_.error();
        return false;
    }
    heap_.bytes = kProgramWindow;
    heap_.va = heap_.vaBase = k_.reserveVa(kProgramWindow);
    if (!heap_.va) {
        err = "no GPU VA for the shader heap's program window";
        return false;
    }
    memset(tic_.cpu, 0, tic_.bytes);
    memset(tsc_.cpu, 0, tsc_.bytes);
    memset(root_.cpu, 0, root_.bytes);
    memset(zero_.cpu, 0, zero_.bytes);
    memset(tickets_.cpu, 0, tickets_.bytes);
    colorTableWords(reinterpret_cast<uint32_t *>(colorTable_.cpu));
    memset(stamps_.cpu, 0, stamps_.bytes);
    // texture header 0: NIL's null descriptor (block linear over the zero page), what NVK keeps at index 0
    uint32_t *t0 = reinterpret_cast<uint32_t *>(tic_.cpu);
    t0[0] = 0x2491d;
    t0[1] = uint32_t(zero_.va);
    t0[2] = 0x600000 | uint32_t(zero_.va >> 32);
    t0[4] = 0xe2840000;
    t0[5] = 0x80000000;
    t0[7] = 0x80000001;
    // sampler header 0: NVK's texel-fetch sampler (nvk_txf_sampler_header: nearest, clamp to border, unnormalized), which NVK
    // reserves at index 0 on every GPU, so a handle naming no sampler names it
    {
        const uint32_t txf[8] = {0x260db, 0x2000291, 0xfff000, 0, 0, 0, 0, 0};
        memcpy(tsc_.cpu, txf, 32);
    }
    // m2v's neutral sampler, which it binds for a sampler slot the function reads and the encoder left unbound (Vulkan defaults:
    // nearest, clamp to edge, mip nearest, no LOD clamp), then the constexpr samplers the pack's functions use
    {
        const uint32_t neutral[8] = {0x26092, 0x291, 0xfff000, 0, 0, 0, 0, 0};
        memcpy(tsc_.cpu + 32 * nextTsc_, neutral, 32);
        neutralTsc_ = nextTsc_++;
    }
    const uint32_t *cs = pack_.section<uint32_t>(PackSamplers);
    if (pack_.header()->samplers > kConstexprSamplers) {
        err = "the pack's constexpr samplers do not fit its entries' sampler byte";
        return false;
    }
    for (uint32_t i = 0; i < pack_.header()->samplers; i++) {
        memcpy(tsc_.cpu + 32 * nextTsc_, cs + 8 * i, 32);
        constexprTsc_[constexprCount_++] = nextTsc_++;
    }
    // SET_CT_WRITE(i), as the baseline leaves it where a state's registers do not set it
    const uint32_t *base = pack_.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack_.bytes(PackBaseline) / 8; i++)
        if (base[2 * i] >= CT_WRITE && base[2 * i] < CT_WRITE + 32)
            ctWrite_[(base[2 * i] - CT_WRITE) / 4] = base[2 * i + 1];
    placed_.assign(pack_.header()->programs, ~0u);
    if (role_ == Role::Client)
        return true;
    if (!ensureChannel()) {
        err = channelError_;
        return false;
    }
    // at the render server's start, not under its first frames: the heap's first part, the mip chains' first chunk and the
    // first pool chunk
    {
        std::lock_guard<std::mutex> l(lock_);
        heapGrow(std::min<uint64_t>(pack_.bytes(PackHeap) + kHeapSlack, kHeapAtOpen));
        Span first;
        if (blAlloc(1, 0, first))
            blFree(first);
    }
    uint32_t chunk;
    newChunk(sizes_.chunk, false, nullptr, chunk);
    return true;
}

bool Device::ensureChannel()
{
    if (channelReady_.load(std::memory_order_acquire))
        return true;
    std::lock_guard<std::mutex> l(channelLock_);
    if (channelReady_.load(std::memory_order_relaxed))
        return true;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (channelFailed_ && now < channelRetryNs_)
        return false;
    if (!openChannel(channelError_)) {
        channelFailed_ = true;
        channelRetryNs_ = now + kChannelRetryNs;
        return false;
    }
    channelReady_.store(true, std::memory_order_release);
    return true;
}

// channelLock_ held: the rings, the channel and its context with the 2D engine's state (raw/fx). False: what it made is given
// back, so a later call starts over.
bool Device::openChannel(std::string &err)
{
    auto undo = [&](bool created) {
        err = k_.error();
        if (created)
            k_.closeChannel(channel_);
        k_.free(push_.bo);
        k_.free(upload_.bo);
        push_ = Ring();
        upload_ = Ring();
        channel_ = 0;
        fence_ = &kNoFence;
        return false;
    };
    if (!k_.alloc(sizes_.push, true, push_.bo) || !k_.alloc(sizes_.upload, true, upload_.bo) ||
        !k_.channel(kEngines, channel_, fence_))
        return undo(false);
    const uint32_t words = contextWords() + uint32_t(fx::twodInitWords());
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, words * 4, 256, va));
    Push p{start, start + words};
    emitContext(p);
    fx::emitTwodInit(p);
    TegraGM20BAbiPush seg = {va, uint32_t(p.at - start), 0};
    uint32_t seq = 0;
    if (!k_.exec(channel_, &seg, 1, 0, seq))
        return undo(true);
    lastSeqno_ = seq;
    retire(push_, seq);
    return true;
}

// lock_ held: program `prog`'s slot in the heap, copied out of the pack the first time a state uses it, its
// SET_PIPELINE_PROGRAM value and its shader data's GPU address (0: none). A slot is a few KiB copied once per process from the
// pack's pages, so the CPU copies it (the copy engine would want the pack's pages imported, wired, for the whole process); the
// prologue of the next submission committed after it invalidates the instruction caches too (CommandBuffer::commit), and every
// prologue the constant cache, before any draw reads it. False: the heap is full (that state stays m2v's).
bool Device::place(uint32_t prog, uint32_t &program, uint64_t &data)
{
    const PackProgram &p = pack_.section<PackProgram>(PackPrograms)[prog];
    if (placed_[prog] == ~0u) {
        uint64_t at = 0;
        if (!heapPlace(pack_.section<uint8_t>(PackHeap) + p.heapOffset, p.bytes, at))
            return false;
        placed_[prog] = uint32_t(at);
        stats.programsPlaced++;
    }
    program = placed_[prog] + (p.program - p.heapOffset);
    data = p.dataBytes ? heap_.va + placed_[prog] + p.dataOffset : 0;
    return true;
}

// lock_ held: an area whose warps get `perWarp` bytes of local memory or more; the next submission gives it to the channel
// (CommandBuffer::commit). False: no memory for it.
bool Device::slmNeed(uint64_t perWarp)
{
    if (perWarp <= slmPerWarp_)
        return true;
    const SlmArea a = slmArea(perWarp, k_.info().tpc_count);
    Bo area;
    if (!k_.alloc(a.bytes, false, area))
        return false;
    if (slm_.bytes)
        slmOutgrown_.push_back(slm_);
    slm_ = area;
    slmPerWarp_ = perWarp;
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
           "raw: shader local memory: %llu bytes a warp, %llu KiB", (unsigned long long)perWarp,
           (unsigned long long)(a.bytes >> 10));
    return true;
}

// lock_ held: the heap bound from its start through `end`, the range past what is bound as one more part of host memory imported
// and bound at the window's VAs. False: past the window, or no memory or the kernel refused the part.
bool Device::heapGrow(uint64_t end)
{
    if (end <= heapBound_)
        return true;
    if (end > heap_.bytes)
        return false;
    // a client's parts double from a quarter step, as its chunks do
    const uint64_t step =
        role_ == Role::Client ? std::clamp(heapBound_, kHeapStep / kClientHeapFirstShare, kHeapStep) : kHeapStep;
    HeapPart part;
    part.offset = heapBound_;
    part.bytes = std::min(heap_.bytes - heapBound_, (end - heapBound_ + step - 1) & ~(step - 1));
    vm_address_t cpu = 0;
    if (vm_allocate(mach_task_self(), &cpu, part.bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    part.cpu = reinterpret_cast<uint8_t *>(cpu);
    if (!k_.importPages(part.cpu, part.bytes, part.bo, 0, true) ||
        !k_.bindImportAt(part.cpu, part.bo, heap_.va + part.offset, kPteKindPitch)) {
        vm_deallocate(mach_task_self(), cpu, part.bytes);
        return false;
    }
    heapParts_.push_back(part);
    heapBound_ += part.bytes;
    return true;
}

// lock_ held: the bound part heap offset `at` lies in, null past every part
const Device::HeapPart *Device::heapPartAt(uint64_t at) const
{
    for (auto it = heapParts_.rbegin(); it != heapParts_.rend(); ++it)
        if (at >= it->offset)
            return at < it->offset + it->bytes ? &*it : nullptr;
    return nullptr;
}

// lock_ held: a program slot of `size` bytes at the heap's end, its offset in `offset`, with the prefetch slack after it bound.
// A slot starts 256-aligned as NVK's uploads do: its code at +0x80 and its shader data at a 256-aligned offset in it, while the
// slot before may end anywhere after its shader data. Parts are apart in host memory, so a slot lies in one part: one that
// would cross a part's end starts at the next part. The heap is cached host memory: what the CPU copies there is cleaned to
// memory before the GPU can read it. False: no room in the window.
bool Device::heapPlace(const uint8_t *bytes, uint32_t size, uint64_t &offset)
{
    uint64_t at = (heapPlaced_ + 0xFF) & ~uint64_t(0xFF);
    for (const HeapPart *p = heapPartAt(at); p && at + size > p->offset + p->bytes; p = heapPartAt(at))
        at = p->offset + p->bytes;
    if (!heapGrow(at + size + kHeapSlack))
        return false;
    const HeapPart *p = heapPartAt(at);
    uint8_t *cpu = p->cpu + (at - p->offset);
    memcpy(cpu, bytes, size);
    cleanToMemory(cpu, size);
    offset = at;
    heapPlaced_ = at + size;
    return true;
}

// The methods that bind program `prog`'s shader data at its slot of constant buffer group `group` (0 vertex, 4 pixel), leaving
// the root table selected; and the slot it reads set 0 at (0: none)
static uint8_t bindProgramCbufs(Push &w, const PackProgram &prog, uint32_t group, uint64_t data, uint64_t root)
{
    uint8_t setSlot = 0;
    for (uint8_t c = 1; c < prog.cbufs; c++) {
        if (prog.cbufType[c] == CbufSet)
            setSlot = c;
        if (prog.cbufType[c] != CbufShaderData)
            continue;
        w.inc(CB_SELECTOR_A, {std::min(prog.dataBytes, kMaxCbufBytes), uint32_t(data >> 32), uint32_t(data)});
        w.m(BIND_GROUP_CB + 0x20 * group, 1 | uint32_t(c) << 4);
        w.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(root >> 32), uint32_t(root)});
    }
    return setSlot;
}

// A state of the pack, made the first time it is looked up (most of the pack's states never are in a process): its registers
// as push words with its programs placed in the heap. Else one the runtime made (addRecord).
Pipeline *Device::pipeline(uint64_t keyHash, PackForm form, bool caTransform)
{
    const PackPipeline *rec = pack_.find(keyHash, form);
    std::lock_guard<std::mutex> l(lock_);
    if (!rec) {
        const auto made = completed_.find(keyHash);
        return form == PackForm::Default && made != completed_.end() ? made->second : nullptr;
    }
    Pipeline &pl = pipelines_[rec - pack_.section<PackPipeline>(PackPipelines)];
    if (pl.rec)
        return &pl;
    const PackPipeline &pp = *rec;
    const PackProgram *progs = pack_.section<PackProgram>(PackPrograms);
    PlacedProgram vs, fs;
    if (!place(pp.vs, vs.program, vs.data) || !place(pp.fs, fs.program, fs.data)) {
        stats.heapFull++;
        return nullptr;
    }
    vs.record = &progs[pp.vs];
    fs.record = &progs[pp.fs];
    vs.header = pack_.section<uint8_t>(PackHeap) + progs[pp.vs].program;
    fs.header = pack_.section<uint8_t>(PackHeap) + progs[pp.fs].program;
    if (!slmNeed(std::max(slmPerWarpOfHeader(vs.header), slmPerWarpOfHeader(fs.header)))) {
        stats.heapFull++;
        return nullptr;
    }
    build(pl, pp, vs, fs, pack_.section<uint32_t>(PackBlob) + pp.blob, pack_.string(pp.keyString), caTransform);
    return &pl;
}

// lock_ held: `pl` of record `pp`, whose programs are placed at `vs` and `fs`, whose registers are `blob` (method, value pairs)
// and whose key's attachment and vertex fields are `keyString`
void Device::build(Pipeline &pl, const PackPipeline &pp, const PlacedProgram &vs, const PlacedProgram &fs,
                   const uint32_t *blob, const char *keyString, bool caTransform)
{
    std::vector<uint32_t> words(64 + 16 + pp.blobDwords * 2);
    Push w{words.data(), words.data() + words.size()};
    uint32_t writes[8];
    memcpy(writes, ctWrite_, sizeof writes);
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
        if (ownedMethod(m))
            continue;  // a pack made before the method was the runtime's: the probe process's value
        if (m >= CT_WRITE && m < CT_WRITE + 32)
            writes[(m - CT_WRITE) / 4] = v;
        w.m(m, v);
    }
    for (uint32_t c = 0; c < 8; c++)
        if (writes[c] & 0x1111)  // R_ENABLE, G_ENABLE, B_ENABLE, A_ENABLE
            pl.writeBits |= uint8_t(1u << c);
    // TILE_COALESCER_MODE: conflicts by sample or pixel
    bool interlocked = false;
    for (uint32_t k = 0; k < pp.blobDwords; k += 2)
        if (blob[k] == PIXEL_SHADER_INTERLOCK_CONTROL)
            interlocked = blob[k + 1] & 3;
    w.m(0x2044, vs.program);  // SET_PIPELINE_PROGRAM(VERTEX)
    w.m(0x2144, fs.program);  // SET_PIPELINE_PROGRAM(PIXEL)
    pl.setSlot[0] = bindProgramCbufs(w, *vs.record, 0, vs.data, root_.va);
    pl.setSlot[1] = bindProgramCbufs(w, *fs.record, 4, fs.data, root_.va);
    for (const auto &[prog, group] : {std::pair{vs.record, uint8_t(0)}, std::pair{fs.record, uint8_t(4)}})
        for (uint8_t c = 1; c < prog->cbufs; c++) {
            if (cbufKind(prog->cbufType[c]) != CbufUbo)
                continue;
            uint8_t entry = 0;
            while (entry < pp.entries && !(pp.entry[entry].kind == EntryUbo && !bufferStorage(pp.entry[entry]) &&
                                           pp.entry[entry].offset == cbufUboOffset(prog->cbufType[c])))
                entry++;
            pl.ubosNamed &= entry < pp.entries;
            if (entry < pp.entries)
                pl.ubos[pl.uboCount++] = {group, c, entry};
        }
    pl.methods.assign(words.data(), w.at);
    memcpy(pl.push, pp.pushWords, sizeof pl.push);
    pl.pushMask = pp.pushMask;
    // the color formats of its key ("c<i>=<format>:<writeMask>:<blending>:..."), and which colors its draws read as they
    // were (m2v linear_attachment::writes: a written color blended into, or written through a mask that keeps channels)
    for (const char *at = keyString; (at = strstr(at, " c")); at += 2) {
        unsigned c = 0, f = 0, mask = 0, blend = 0, factor[6] = {};
        const int n = sscanf(at, " c%u=%u:%u:%u:%u:%u:%*u:%u:%u", &c, &f, &mask, &blend, &factor[0], &factor[1],
                             &factor[2], &factor[3]);
        if (n >= 4 && c < 8) {
            constexpr unsigned kRGB8_444_2P = 542,
                               kRGBA16Float = 115;  // rendered through its stand-in (Texture::planarStandIn)
            pl.colorFormats[c] = f == kRGB8_444_2P ? kRGBA16Float : f;
            if (mask && (blend || mask != 15))
                pl.readBits |= uint8_t(1u << c);
            // MTLBlendFactorBlendColor, OneMinusBlendColor, BlendAlpha, OneMinusBlendAlpha
            for (int k = 0; blend && k + 4 < n; k++)
                pl.constantBlend |= factor[k] >= 11 && factor[k] <= 14;
        }
    }
    // attribute 0 ("a0=<MTLVertexFormat>:<offset>:<buffer>:<stride>:<step function>:..."): MTLVertexFormatFloat2, Float3
    // or Float4, per vertex, from the stream's buffer
    unsigned format = 0, offset = 0, buffer = 0, stride = 0, step = 0;
    if (const char *a0 = strstr(keyString, " a0=");
        a0 && sscanf(a0, " a0=%u:%u:%u:%u:%u", &format, &offset, &buffer, &stride, &step) == 5 && format >= 29 &&
        format <= 31 && buffer == pp.vertexSlot && stride == pp.vertexStride && step == 1) {
        pl.positionComponents = uint8_t(format - 27);
        pl.positionOffset = uint16_t(offset);
    }
    pl.caTransform = caTransform;
    if (const char *ds = strstr(keyString, "ds=")) {
        uint32_t depth = 0, stencil = 0;
        sscanf(ds, "ds=%u/%u", &depth, &stencil);
        pl.depthFormat = depth ? depth : stencil;
    }
    if (const char *n = strstr(keyString, "samples="))
        pl.samples = uint8_t(std::max(1, atoi(n + 8)));
    // SPH word 0 bit 15 KILLS_PIXELS, bit 608 OMAP_SAMPLE_MASK (cla097sph.h); SET_DEPTH_TEST, SET_STENCIL_TEST and
    // SET_ANTI_ALIAS_ALPHA_CONTROL's alpha to coverage in bit 0
    uint32_t sph[20];
    memcpy(sph, fs.header, sizeof sph);
    bool tests = false;
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
        tests |= (m == DEPTH_TEST || m == STENCIL_TEST || m == ANTI_ALIAS_ALPHA_CONTROL) && (v & 1);
        if (m == HYBRID_ANTI_ALIAS_CONTROL)
            pl.samplePasses = uint8_t(std::max(1u, v & 0xf));
    }
    pl.dropsPixels = (sph[0] >> 15 & 1) || (sph[19] & 1) || tests || strncmp(keyString, "samples=1 ", 10) ||
                     !strstr(keyString, " ds=0/0 ");
    for (uint32_t k = 0; k < pp.entries; k++)
        if (pp.entry[k].kind == EntryFetch && pp.entry[k].slot < 8)
            pl.fetchBits |= uint8_t(1u << pp.entry[k].slot);
    if (interlocked)
        pl.storageBits = pl.fetchBits;
    pl.rec = &pp;
}

// ---- device: memory ----

// A span of `bytes` (page multiple) from the pool: first fit in a chunk, else a new chunk (one BoImport of fresh anonymous
// memory; its pages are wired and their zeros cleaned to memory by the kext: ~10 ms for 16 MiB on the device, r7l). A span of more
// than half a chunk gets a chunk of its own, given back whole when the span is freed. The shared chunks grow ahead of need: once their
// free bytes fall below a chunk's worth the grower thread makes the next one, so this (the render thread's resource creation) finds room;
// one that does not waits for a chunk the grower is making, or makes one itself (Stats::poolStalls: a burst outran the mark).

// The size of a new shared chunk of a kind that must hold `need` bytes: the role's (`cap`) for the render server; for a client,
// what it already holds of that kind, from a sixteenth of the role's to all of it, so its chunks double as it draws more
uint64_t Device::chunkSize(const std::vector<Chunk> &chunks, uint64_t cap, uint64_t need, uint64_t align) const
{
    uint64_t size = cap;
    if (role_ == Role::Client) {
        uint64_t held = 0;
        for (const Chunk &ch : chunks)
            if (!ch.dedicated)
                held += ch.bytes;
        size = std::clamp(held, cap / kClientFirstShare, cap);
    }
    size = std::max(size, need);
    return (size + align - 1) & ~(align - 1);
}

// lock_ held (`unlocked`: the caller's lock, let go for the kernel's wiring and taken again); the chunk's index in chunks_
bool Device::newChunk(uint64_t size, bool dedicated, std::unique_lock<std::mutex> *unlocked, uint32_t &index)
{
    vm_address_t memory = 0;
    if (vm_allocate(mach_task_self(), &memory, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    Chunk ch;
    ch.memory = reinterpret_cast<void *>(memory);
    ch.bytes = size;
    ch.dedicated = dedicated;
    if (unlocked)
        unlocked->unlock();
    const bool wired = k_.importPages(ch.memory, size, ch.bo);
    if (unlocked)
        unlocked->lock();
    if (!wired || !k_.bindImport(ch.memory, ch.bo, kPteKindPitch)) {
        vm_deallocate(mach_task_self(), memory, size);
        return false;
    }
    uint32_t c = 0;
    while (c < chunks_.size() && chunks_[c].memory)
        c++;
    if (c == chunks_.size())
        chunks_.emplace_back();
    if (!dedicated) {
        ch.free[0] = size;
        poolFree_ += size;
    }
    chunks_[c] = std::move(ch);
    stats.poolBytes += size;
    stats.poolChunks++;
    index = c;
    return true;
}

void Device::growLoop()
{
    std::unique_lock<std::mutex> l(lock_);
    for (;;) {
        grow_.wait(l, [&] {
            return stopping_ || growing_ || !tiledWanted_.empty();
        });
        if (stopping_)
            return;
        uint32_t c;
        if (growing_) {
            newChunk(sizes_.chunk, false, &l, c);  // failing, the render thread's next allocation tries itself
            growing_ = false;
        } else {
            // the want stays listed while it is made, so an allocation that finds no room waits for it
            const TiledWant w = tiledWanted_.front();
            newTiledChunk(*w.chunks, w.kind, chunkSize(*w.chunks, sizes_.blChunk, 0, kBlAlign), false, &l, c);
            tiledWanted_.erase(tiledWanted_.begin());
        }
        grown_.notify_all();
    }
}

bool Device::poolAlloc(uint64_t bytes, Span &s)
{
    bytes = (bytes + pageSize() - 1) & ~uint64_t(pageSize() - 1);
    s = {};
    // over half a chunk: its own chunk, made now (in the render server rare: no Core Animation texture of the 720p screen is
    // that large)
    if (bytes > sizes_.chunk / 2) {
        uint32_t c = ~0u;
        for (auto it = spares_.begin(); it != spares_.end(); ++it)
            if (chunks_[it->chunk].bytes == bytes) {
                c = it->chunk;
                spares_.erase(it);
                break;
            }
        if (c == ~0u && !newChunk(bytes, true, nullptr, c))
            return false;
        chunks_[c].used = bytes;
        s = {c, 0, bytes, chunks_[c].bo.va, static_cast<uint8_t *>(chunks_[c].memory)};
        return true;
    }
    for (int pass = 0; pass < 3; pass++) {
        for (uint32_t c = 0; c < chunks_.size(); c++) {
            Chunk &ch = chunks_[c];
            if (ch.dedicated || !ch.memory)
                continue;
            for (auto it = ch.free.begin(); it != ch.free.end(); ++it) {
                if (it->second < bytes)
                    continue;
                const uint64_t offset = it->first, left = it->second - bytes;
                ch.free.erase(it);
                if (left)
                    ch.free[offset + bytes] = left;
                ch.used += bytes;
                poolFree_ -= bytes;
                s = {c, offset, bytes, ch.bo.va + offset, static_cast<uint8_t *>(ch.memory) + offset};
                if (poolFree_ < sizes_.chunk && !growing_ && grower_.joinable()) {  // the next chunk, off this thread
                    growing_ = true;
                    grow_.notify_one();
                }
                return true;
            }
        }
        if (pass == 2)
            break;
        stats.poolStalls++;
        if (growing_) {  // the grower is making one: wait for it rather than wire a second
            std::unique_lock<std::mutex> l(lock_, std::adopt_lock);
            // bounded, as every wait of the plugin's: a chunk not wired in 20 s is a stuck kernel import, said once and
            // stopped for the stack (a wait with no bound froze r7p8's compositor with nothing in the log)
            if (!grown_.wait_for(l, std::chrono::seconds(20), [&] {
                    return !growing_;
                })) {
                os_log_fault(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                             "raw: pool chunk not wired after 20 s (the grower's import): abort() for the stack");
                abort();
            }
            l.release();
            continue;
        }
        uint32_t c;
        if (!newChunk(chunkSize(chunks_, sizes_.chunk, bytes, pageSize()), false, nullptr, c))
            return false;
    }
    return false;
}

void Device::warm()
{
    {
        std::lock_guard<std::mutex> l(lock_);
        uint32_t c;
        while (poolFree_ <= sizes_.chunk && newChunk(sizes_.chunk, false, nullptr, c)) {
        }  // the first and the one ahead of it
        while (tiledFreeBytes(blChunks_) <= sizes_.blChunk &&
               newTiledChunk(blChunks_, fx::kBlockLinearKind, sizes_.blChunk, false, nullptr, c)) {
        }  // and of block-linear images: the first frames' memoryless backings find room before the grower runs
    }
    if (!grower_.joinable())
        grower_ = std::thread([this] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            pthread_setname_np("com.brunostjohn.switch.raw.pool");
            followers().join();
            growLoop();
        });
}

void Device::poolFree(Span &s)
{
    if (!s.valid() || s.chunk >= chunks_.size())
        return;
    Chunk &ch = chunks_[s.chunk];
    ch.used -= s.bytes;
    if (ch.dedicated) {  // nothing else is in it
        if (spares_.size() < kDedicatedSpares) {
            spares_.push_back({s.chunk, commitCount_});
            s = {};
            return;
        }
        freeChunk(ch);
        s = {};
        return;
    }
    uint64_t offset = s.offset, bytes = s.bytes;
    poolFree_ += bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    s = {};
    // an empty chunk goes back while the others keep a chunk's worth free (the grower's own mark), so the pool
    // shrinks after a burst instead of holding its peak for good (r8a: 320 MB in 20 chunks from 15:18 on, the screen idle)
    if (!ch.used && poolFree_ >= ch.bytes + sizes_.chunk) {
        poolFree_ -= ch.bytes;
        freeChunk(ch);
    }
}

// lock_ held: an imported chunk's BO and pages go back (the kernel keeps the pages until the work submitted before has completed)
void Device::freeChunk(Chunk &ch)
{
    stats.poolBytes -= ch.bytes;
    stats.poolChunks--;
    k_.free(ch.bo);
    vm_deallocate(mach_task_self(), vm_address_t(ch.memory), ch.bytes);
    ch = Chunk();
}

void Device::trim()
{
    std::lock_guard<std::mutex> l(lock_);
    for (const Spare &sp : spares_)
        freeChunk(chunks_[sp.chunk]);
    spares_.clear();
    for (Chunk &ch : chunks_)
        if (ch.memory && !ch.dedicated && !ch.used) {
            poolFree_ -= ch.bytes;
            freeChunk(ch);
        }
    auto freeUnused = [&](std::vector<Chunk> &tiled) {
        for (Chunk &ch : tiled)
            if (ch.memory && !ch.used)
                freeChunk(ch);
    };
    freeUnused(blChunks_);
    for (std::vector<Chunk> &z : zChunks_)
        freeUnused(z);
    for (Chunk &ch : wcChunks_)
        if (ch.bo.handle && !ch.used) {
            k_.free(ch.bo);
            ch = Chunk();
        }
}

// A surface's one import in this task. A new one asks the kernel its layout (`ask`: the first texture's shape and whether it
// writes; none: a pitch decision, as for m2v's imports and buffers over a surface) before binding it under that layout's
// kind; one already made is shared whatever its layout.
Import *Device::importSurface(uint64_t id, void *base, uint64_t bytes, const void *owner, const LayoutAsk *ask)
{
    const auto key = std::make_tuple(id, uint64_t(uintptr_t(base)), bytes);
    if (id) {
        auto it = surfaces_.find(key);
        if (it != surfaces_.end()) {
            it->second->refs++;
            stats.importHits++;
            return it->second;
        }
    }
    Import *i = new Import;
    if (!k_.importPages(base, bytes, i->bo, uint32_t(id))) {
        delete i;
        return nullptr;
    }
    const LayoutAsk none = {};
    const LayoutAsk &a = ask ? *ask : none;
    if (id && k_.surfaceLayout(i->bo, a.width, a.height, a.bpp, a.bytesPerRow, a.writes) == TegraGM20BLayoutTiled &&
        tgm_layout_tiled(a.width, a.height, a.bpp, &i->layout))
        i->kind = TGM_LAYOUT_KIND;
    if (!k_.bindImport(base, i->bo, i->kind)) {
        delete i;
        return nullptr;
    }
    i->refs = 1;
    i->id = id;
    i->base = uintptr_t(base);
    i->bytes = bytes;
    if (owner)
        i->owner = CFRetain(owner);
    if (id)
        surfaces_[key] = i;
    stats.imports++;
    stats.importsMade++;
    stats.importBytes += i->bo.bytes;
    return i;
}

void Device::release(Import *i)
{
    if (!i || --i->refs)
        return;
    if (i->id)
        surfaces_.erase(std::make_tuple(i->id, i->base, i->bytes));
    stats.imports--;
    stats.importBytes -= i->bo.bytes;
    k_.free(i->bo);
    if (i->owner)
        CFRelease(i->owner);
    delete i;
}

uint32_t Device::allocTic()
{
    if (!freeTic_.empty()) {
        const uint32_t t = freeTic_.back();
        freeTic_.pop_back();
        return t;
    }
    return nextTic_ < kTicCount ? nextTic_++ : 0;
}

// A freed header holds header 0's null descriptor until it is reused: zeroes would read as a buffer texture at VA 0, which a
// draw still in flight, or one binding a stale index, would sample (a fault).
void Device::freeTic(uint32_t tic)
{
    memcpy(tic_.cpu + 32 * tic, tic_.cpu, 32);
    freeTic_.push_back(tic);
}

// Texture header of a pitch-linear 2D texture (Mesa NIL nvb097_fill_image_view_desc for a linear image: HEADER_VERSION PITCH,
// TEXTURE_TYPE TWO_D_NO_MIPMAP), word 0 NVK's for the format
void Device::writeTic(const Texture &t)
{
    fx::MipChain one;
    uint32_t w[8] = {};
    if (t.zeta) {  // NVK's image as laid out (createZetaOver): one level, its block height
        one.width = t.width;
        one.height = t.height;
        one.levels = 1;
        one.bpp = t.bpp;
        one.yLog2 = t.blockHeight;
        fx::mipChainTic(one, t.va, kZetaFormatInfo[zetaFormatIndex(t.format)].tic0, false, 0, 1, w, fx::kTexTwoD);
    } else if (const FormatInfo *f = formatInfo(t.format);
               (t.layers > 1 && fx::arrayLayout(t.width, t.height, t.layers, 1, t.bpp, one)) ||
               (t.type == fx::kTexThreeD &&
                fx::volumeLayout(t.width, t.height, t.depth, 1, t.bpp, !t.blockDepth, one)) ||
               (t.blockLinear && fx::mipChainLayout(t.width, t.height, 1, t.bpp, one))) {  // NIL's header of the image
        if (t.blockW > 1 || t.blockH > 1)
            one.yLog2 = t.blockHeight;  // (blockLayout's)
        fx::mipChainTic(one, t.va, f->tic0, f->srgb, 0, 1, w, t.type);
    } else {
        w[0] = f->tic0;
        setField(w, 87, 85, 2);                        // HEADER_VERSION PITCH
        setField(w, 63, 37, (t.va >> 5) & 0x7ffffff);  // ADDRESS_BITS31TO5
        setField(w, 79, 64, (t.va >> 32) & 0xffff);
        setField(w, 111, 96, (t.pitch >> 5) & 0xffff);
        setField(w, 154, 151, 7);  // TWO_D_NO_MIPMAP
        setField(w, 112, 112, 1);  // LOD_ANISO_QUALITY2
        setField(w, 113, 113, 1);  // LOD_ANISO_QUALITY HIGH
        setField(w, 114, 114, 1);  // LOD_ISO_QUALITY HIGH
        setField(w, 143, 128, t.width - 1);
        setField(w, 175, 160, t.height - 1);
        setField(w, 150, 150, f->srgb);
        setField(w, 159, 157, 7);  // BORDER_SIZE BORDER_SAMPLER_COLOR
        setField(w, 191, 191, 1);  // NORMALIZED_COORDS
        setField(w, 216, 215, 2);  // ANISO_FINE_SPREAD_FUNC TWO
        setField(w, 218, 217, 1);  // ANISO_COARSE_SPREAD_FUNC ONE
    }
    if (t.blockW > 1 || t.blockH > 1) {
        setField(w, 143, 128, t.texelWidth - 1);  // WIDTH_MINUS_ONE in texels (NIL's extent_px)
        setField(w, 175, 160, t.texelHeight - 1);
    }
    w[0] = swizzledWord0(w[0], t.swizzle);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // write-combined: visible at the next submission's header invalidate
}

void Device::swizzle(Texture &t, const uint8_t channels[4])
{
    std::lock_guard<std::mutex> l(lock_);
    memcpy(t.swizzle, channels, 4);
    if (!t.tic)
        return;
    uint32_t *w = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic);
    w[0] = swizzledWord0(w[0], t.swizzle);
}

uint32_t swizzledWord0(uint32_t word0, const uint8_t channels[4])
{
    constexpr uint32_t kZero = 0, kOneInt = 6, kOneFloat = 7, kSint = 3, kUint = 4;
    auto source = [&](uint32_t c) {
        return word0 >> (19 + 3 * c) & 7;
    };
    const uint32_t redType = word0 >> 7 & 7;
    uint32_t one = redType == kSint || redType == kUint ? kOneInt : kOneFloat;
    for (uint32_t c = 0; c < 4; c++)
        if (source(c) == kOneInt || source(c) == kOneFloat)
            one = source(c);
    uint32_t out = word0 & ~(0xfffu << 19);
    for (uint32_t c = 0; c < 4; c++) {
        const uint8_t s = channels[c];  // MTLTextureSwizzle: Zero, One, Red, Green, Blue, Alpha
        const uint32_t from = s == 0 ? kZero : s == 1 ? one : s <= 5 ? source(s - 2u) : source(c);
        out |= from << (19 + 3 * c);
    }
    return out;
}

// A pitch-linear color target: its address and pitch multiples of 128 (NVK renders a linear image only so, nvk_rendering_linear,
// "the render hardware gets grumpy" otherwise; m2v's NVK renders any other through a tiled shadow, which the raw path leaves to
// m2v)
static bool linearTarget(const Texture &t)
{
    return !((t.va | t.pitch) & 127);
}

bool shape(uint32_t width, uint32_t height, uint32_t format, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    if (!f || !width || !height || width > 16384 || height > 16384)
        return false;
    t = {};
    t.blockW = f->blockW;
    t.blockH = f->blockH;
    t.texelWidth = width;
    t.texelHeight = height;
    t.width = (width + f->blockW - 1) / f->blockW;
    t.height = (height + f->blockH - 1) / f->blockH;
    t.format = format;
    t.bpp = f->bpp;
    t.renderable = f->rt != 0;
    return true;
}

// NIL's layout of a block-compressed image: its blocks tiled, the block height chosen by its height in texels (Tiling::choose
// reads extent_px) and clamped to its rows of blocks
static void blockLayout(const Texture &t, tgm_layout &bl)
{
    const uint8_t y =
        uint8_t(std::min<uint32_t>(tgm_layout_y_log2(t.texelHeight), tgm_layout_log2_ceil((t.height + 7) / 8)));
    const uint64_t block = 512ull << y;
    bl.y_log2 = y;
    bl.rows = (t.height + (8u << y) - 1) / (8u << y) * (8u << y);
    bl.align = std::max<uint64_t>(block, 4096);
    bl.bytes = (uint64_t(bl.row_bytes) * bl.rows + bl.align - 1) / bl.align * bl.align;
}

bool Device::createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc, bool tiled)
{
    if (!shape(width, height, format, t))
        return false;
    const bool blocks = t.blockW > 1 || t.blockH > 1;
    t.pitch = (t.width * t.bpp + 255) & ~255u;
    std::lock_guard<std::mutex> l(lock_);
    const uint64_t bytes = uint64_t(t.pitch) * t.height;
    tgm_layout bl;
    // (a colour format: the chunks are bound under colour kind 0xFE, which a depth/stencil format's zeta is not; a
    // block-compressed one is NVK's optimal image, never pitch)
    const bool laid = tiled && !wc && (t.renderable || blocks) && tgm_layout_tiled(t.width, t.height, t.bpp, &bl);
    if (laid && blocks)
        blockLayout(t, bl);
    if (laid && blAlloc(bl.bytes, bl.align, t.span)) {
        t.pitch = bl.row_bytes;
        t.blockLinear = true;
        t.blockHeight = bl.y_log2;
        t.layerBytes = bl.bytes;
    } else if (blocks) {
        return false;
    } else if (wc && wcAlloc(bytes, t.span)) {
        t.wc = true;
        stats.wcBytes += t.span.bytes;
    } else {
        if (wc)
            stats.wcFallbacks++;
        if (!poolAlloc(bytes, t.span))
            return false;
    }
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, memoryOf(t), none);
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::createLine(uint32_t width, uint32_t format, Texture &t)
{
    tgm_layout bl;
    if (!shape(width, 1, format, t) || xrForm(format) != XrForm::None || !tgm_layout_tiled(width, 1, t.bpp, &bl))
        return false;
    std::lock_guard<std::mutex> l(lock_);
    if (!blAlloc(bl.bytes, bl.align, t.span))
        return false;
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
    t.type = fx::kTexOneD;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, Memory::Bl, none);
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::createVolume(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, bool view2d, Texture &t)
{
    fx::MipChain c;
    if (!shape(width, height, format, t) || xrForm(format) != XrForm::None ||
        !fx::volumeLayout(width, height, depth, 1, t.bpp, view2d, c))
        return false;
    std::lock_guard<std::mutex> l(lock_);
    if (!blAlloc(c.bytes, c.align, t.span))
        return false;
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = c.bytes;
    t.type = fx::kTexThreeD;
    t.depth = depth;
    t.blockDepth = c.zLog2;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, Memory::Bl, none);
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::createArray(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t type, Texture &t)
{
    fx::MipChain c;
    if (!shape(width, height, format, t) || xrForm(format) != XrForm::None ||
        !fx::arrayLayout(width, height, layers, 1, t.bpp, c))
        return false;
    std::lock_guard<std::mutex> l(lock_);
    if (!blAlloc(c.bytes, c.align, t.span))
        return false;
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = c.arrayStride;
    t.type = uint8_t(type);
    t.layers = layers;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, Memory::Bl, none);
        return false;
    }
    writeTic(t);
    return true;
}

// A texture over a block-linear surface's import: TegraGM20BLayout.h's image from the surface's first byte, at the shape the
// kernel decided (a view of another format of the same texel size reads it as its own), under the import's
// TGM_LAYOUT_KIND binding, which the 3D and 2D engines, the copy engine and the texture unit all address so
bool Device::tiledSurfaceTexture(uint64_t offset, uint32_t width, uint32_t height, Texture &t)
{
    const tgm_layout &l = t.import->layout;
    if (offset || width != l.width || height != l.height || t.bpp != l.bpp) {
        release(t.import);
        t.import = nullptr;
        return false;
    }
    t.va = t.import->bo.va;
    t.cpu = reinterpret_cast<uint8_t *>(t.import->base);
    t.pitch = l.row_bytes;
    t.blockLinear = true;
    t.blockHeight = l.y_log2;
    t.layerBytes = l.bytes;
    if (!(t.tic = allocTic())) {
        release(t.import);
        t.import = nullptr;
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::createSurfaceTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                  uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format,
                                  Texture &t, bool writes)
{
    // rows and offset a multiple of 16, as m2v takes a surface (linear_plan.rs); the texture header's 32 through `sampled`
    if (!shape(width, height, format, t) || (pitch & 15) || (offset & 15) || pitch < width * t.bpp ||
        offset + uint64_t(pitch) * height > allocBytes || (uintptr_t(base) & (pageSize() - 1)))
        return false;
    const bool unaligned = (pitch | offset) & 31;
    Texture *sampled = nullptr;
    if (unaligned) {
        sampled = new Texture;
        if (!createTexture(width, height, format, *sampled, false, true)) {
            delete sampled;
            return false;
        }
    }
    std::unique_lock<std::mutex> l(lock_);
    t.pitch = pitch;
    const LayoutAsk ask = {width, height, t.bpp, pitch, writes && !offset && !sampled};
    if (!(t.import = importSurface(surfaceId, base, (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1), surface,
                                   &ask)) ||
        (t.import->kind == TGM_LAYOUT_KIND && !tiledSurfaceTexture(offset, width, height, t))) {
        l.unlock();
        if (sampled) {
            destroyTexture(*sampled);
            delete sampled;
        }
        return false;
    }
    if (t.blockLinear) {
        l.unlock();
        if (sampled) {
            destroyTexture(*sampled);
            delete sampled;
        }
        return true;
    }
    t.va = t.import->bo.va + offset;
    t.cpu = static_cast<uint8_t *>(base) + offset;
    t.shadowTarget = t.renderable && !linearTarget(t);
    t.renderable = t.renderable && linearTarget(t);
    if (sampled) {  // sampled through its pool image's header
        t.sampled = sampled;
        t.tic = sampled->tic;
        return true;
    }
    if (!(t.tic = allocTic())) {
        release(t.import);
        t.import = nullptr;
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::createPlanarTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                 uint64_t luma, uint32_t lumaPitch, uint64_t chroma, uint32_t chromaPitch,
                                 uint32_t width, uint32_t height, Texture &t)
{
    constexpr uint32_t kRGBA8Unorm = 70;
    if (!width || !height || lumaPitch < width || chromaPitch < 2 * width ||
        luma + uint64_t(lumaPitch) * height > allocBytes || chroma + uint64_t(chromaPitch) * height > allocBytes ||
        (uintptr_t(base) & (pageSize() - 1)))
        return false;
    Import *planes;
    {
        std::lock_guard<std::mutex> l(lock_);
        planes = importSurface(surfaceId, base, (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1), surface);
    }
    if (!planes)
        return false;
    if (!createTexture(width, height, kRGBA8Unorm, t)) {  // the image: pool memory, its texture header
        std::lock_guard<std::mutex> l(lock_);
        release(planes);
        return false;
    }
    t.import = planes;
    t.planar = true;
    t.addressMap = 2;
    t.planes = {t.import->bo.va + luma, t.import->bo.va + chroma, lumaPitch, chromaPitch};
    return true;
}

Pipeline *Device::standInPipeline(bool store)
{
    static const char load[] = "raw.two_plane.two_plane_load", stored[] = "raw.two_plane.two_plane_store";
    return store ? pipeline(Pack::fnv(stored, sizeof stored - 1)) : pipeline(Pack::fnv(load, sizeof load - 1));
}

bool Device::createPlanarTarget(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, Texture &t)
{
    constexpr uint32_t kR8Unorm = 10, kRG8Unorm = 30, kRGBA16Float = 115;
    if (!t.planar || !t.import || !standInPipeline(false) || !standInPipeline(true))
        return false;
    const uint64_t at = t.import->bo.va;
    Texture *luma = new Texture, *chroma = new Texture, *standIn = new Texture;
    const bool made = createSurfaceTexture(surfaceId, base, allocBytes, surface, t.planes.luma - at, t.width, t.height,
                                           t.planes.lumaPitch, kR8Unorm, *luma) &&
                      createSurfaceTexture(surfaceId, base, allocBytes, surface, t.planes.chroma - at, t.width,
                                           t.height, t.planes.chromaPitch, kRG8Unorm, *chroma) &&
                      createTexture(t.width, t.height, kRGBA16Float, *standIn, false, true) && standIn->renderable &&
                      (luma->renderable || luma->shadowTarget) && (chroma->renderable || chroma->shadowTarget);
    if (!made) {
        for (Texture *x : {luma, chroma, standIn}) {
            destroyTexture(*x);
            delete x;
        }
        return false;
    }
    t.luma = luma;
    t.chroma = chroma;
    t.planarStandIn = standIn;
    return true;
}

bool Device::createBufferTexture(const Buffer &b, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t format, Texture &t)
{
    if (!shape(width, height, format, t) || blockCompressed(formatInfo(format)) || (pitch & 31) ||
        pitch < width * t.bpp || ((b.va + offset) & 31) || offset + uint64_t(pitch) * height > b.length)
        return false;
    std::lock_guard<std::mutex> l(lock_);
    t.pitch = pitch;
    t.va = b.va + offset;
    t.shadowTarget = t.renderable && !linearTarget(t);
    t.renderable = t.renderable && linearTarget(t);
    t.cpu = b.cpu + offset;
    if (!(t.tic = allocTic()))
        return false;
    writeTic(t);
    return true;
}

bool Device::createView(const Texture &of, uint32_t format, Texture &t)
{
    // Metal views a converted image (planar, YCbCr) only in its own format or that format's sRGB twin (m2v view_plan.rs),
    // neither of which has a header here.
    if (of.converted())
        return false;
    const FormatInfo *f = formatInfo(format);
    if (!f || f->bpp != of.bpp || f->blockW != of.blockW || f->blockH != of.blockH || of.memoryless || of.sampled ||
        xrForm(format) != XrForm::None || xrForm(of.format) != XrForm::None)
        return false;
    std::lock_guard<std::mutex> l(lock_);
    t = of;
    memcpy(t.swizzle, kIdentitySwizzle, 4);
    t.view = true;
    t.format = format;
    t.renderable = f->rt != 0 && (t.blockLinear || linearTarget(t));
    t.shadowTarget = f->rt != 0 && !t.blockLinear && !linearTarget(t);
    t.import = nullptr;  // the viewed texture owns the memory (RawMetal.mm keeps it alive while a view lives)
    t.span = {};
    t.lease = nullptr;
    t.chain = nullptr;  // a mipmapped texture's view: every level sampled, level 0 rendered and copied
    if (!(t.tic = allocTic()))
        return false;
    if (of.chain) {
        uint32_t w[8];
        fx::mipChainTic(of.chain->layout, of.va, f->tic0, f->srgb, 0, of.chain->layout.levels, w, of.type);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
        return true;
    }
    writeTic(t);
    return true;
}

bool Device::createPartView(const Texture &of, uint32_t format, uint32_t type, uint32_t baseLevel, uint32_t levels,
                            uint32_t baseLayer, uint32_t layers, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    const bool cube = type == fx::kTexCube || type == fx::kTexCubeArray;
    if (of.converted() || !of.blockLinear || of.zeta || of.type == fx::kTexOneD || of.type == fx::kTexThreeD || !f ||
        f->bpp != of.bpp || blockCompressed(f) || of.blockW > 1 || of.blockH > 1 || of.memoryless || of.sampled ||
        xrForm(format) != XrForm::None || xrForm(of.format) != XrForm::None || !levels || !layers ||
        (type == fx::kTexTwoD && layers != 1) || (cube && (layers % 6 || of.width != of.height)))
        return false;
    fx::MipChain c;
    uint64_t va = of.va;
    Texture image;
    if (of.chain) {
        if (baseLayer || layers != 1 || baseLevel + levels > of.chain->layout.levels ||
            (baseLevel && !chainLevelImage(of, baseLevel, image)))
            return false;
        c = of.chain->layout;
        va = of.chain->va;
        if (!baseLevel)
            image = of;
        // (a later level's block height is the chain's clamp, not its own image's: the CPU goes by m2v)
        image.cpu = nullptr;
        image.layerBytes = c.arrayStride;
    } else {
        if (baseLevel || levels != 1 || baseLayer + layers > of.layers ||
            !fx::arrayLayout(of.width, of.height, of.layers, 1, of.bpp, c))
            return false;
        va += baseLayer * c.arrayStride;
        image = of;
        image.va = va;
        image.cpu = of.cpu ? of.cpu + baseLayer * c.arrayStride : nullptr;
        image.layers = layers;
        image.layerBytes = c.arrayStride;
    }
    c.layers = layers;
    std::lock_guard<std::mutex> l(lock_);
    t = image;
    memcpy(t.swizzle, kIdentitySwizzle, 4);
    t.view = true;
    t.format = format;
    t.type = uint8_t(type);
    t.renderable = f->rt != 0 && type == fx::kTexTwoD && levels == 1 && layers == 1;
    t.shadowTarget = false;
    t.import = nullptr;
    t.span = {};
    t.lease = nullptr;
    t.chain = nullptr;
    t.wc = false;
    if (!(t.tic = allocTic()))
        return false;
    uint32_t w[8];
    fx::mipChainTic(c, va, f->tic0, f->srgb, baseLevel, levels, w, type);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);
    return true;
}

bool Device::createZetaOver(const ZetaMemory &m, uint32_t width, uint32_t height, uint32_t format, bool sampled,
                            Texture &t, const char **why)
{
    uint64_t bytes, align;
    *why = !zetaShape(width, height, format, 1, t, bytes, align) ? "a size or format the raw path renders no zeta of"
           : !m.va                                               ? "its image is not bound"
           : !m.tiled                                            ? "its image is pitch-linear"
           : m.blockDepth                                        ? "its image's blocks are deeper than one plane"
           : m.bpp != t.bpp                                      ? "its texel size is not the format's"
           : m.pitch < width * t.bpp || m.pitch % 64             ? "its row stride is not whole GOBs of its texels"
           : m.bytes < m.layerBytes || uint64_t(m.pitch) * height > m.bytes ? "its image is smaller than its rows"
           : sampled && m.pitch != (width * t.bpp + 63) / 64 * 64
               ? "its row stride is not the one a texture header implies"
               : nullptr;
    if (*why)
        return false;
    t.va = m.va;
    t.pitch = m.pitch;
    t.blockHeight = m.blockHeight;
    t.layerBytes = m.layerBytes;
    t.comptags = m.compressed;
    if (!sampled)
        return true;
    if (!(t.tic = allocTic())) {
        *why = "no texture header is left";
        return false;
    }
    writeTic(t);
    return true;
}

bool Device::stencilTemp(Texture &t)
{
    constexpr uint32_t kRG8Unorm = 30;
    if (t.stencilTemp || !t.zeta || t.format != kZetaFormat)
        return t.stencilTemp != nullptr;
    Texture *temp = new Texture;
    if (!createTexture(t.width, t.height, kRG8Unorm, *temp)) {
        delete temp;
        return false;
    }
    t.stencilTemp = temp;
    return true;
}

bool Device::createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t, uint32_t samples)
{
    uint64_t bytes, align;
    const bool zeta = zetaFormatIndex(format) < kZetaFormats;
    if (zeta ? !zetaShape(width, height, format, samples, t, bytes, align) : !shape(width, height, format, t))
        return false;
    fx::MipChain c;
    if (samples > 1 && !zeta &&
        (!t.renderable || xrForm(format) != XrForm::None || !fx::msLayout(width, height, samples, t.bpp, c)))
        return false;
    t.memoryless = true;
    t.samples = uint8_t(samples);
    return true;
}

// ---- block-linear memory (raw/fx mip chains: kind 0xFE; zeta: kZetaPteKind), made on demand ----

uint64_t Device::tiledFreeBytes(const std::vector<Chunk> &chunks)
{
    uint64_t free = 0;
    for (const Chunk &ch : chunks)
        if (ch.memory && !ch.dedicated)
            free += ch.bytes - ch.used;
    return free;
}

bool Device::tiledWanted(const std::vector<Chunk> &chunks) const
{
    return std::any_of(tiledWanted_.begin(), tiledWanted_.end(), [&](const TiledWant &w) {
        return w.chunks == &chunks;
    });
}

bool Device::newTiledChunk(std::vector<Chunk> &chunks, uint32_t kind, uint64_t size, bool dedicated,
                           std::unique_lock<std::mutex> *unlocked, uint32_t &index)
{
    vm_address_t memory = 0;
    if (vm_allocate(mach_task_self(), &memory, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    Chunk ch;
    ch.memory = reinterpret_cast<void *>(memory);
    ch.bytes = size;
    ch.dedicated = dedicated;
    if (unlocked)
        unlocked->unlock();
    const bool wired = k_.importPages(ch.memory, size, ch.bo);
    if (unlocked)
        unlocked->lock();
    if (!wired || !k_.bindImport(ch.memory, ch.bo, kind) || (ch.bo.va & (kBlAlign - 1))) {
        if (ch.bo.handle)
            k_.free(ch.bo);
        vm_deallocate(mach_task_self(), memory, size);
        return false;
    }
    uint32_t c = 0;
    while (c < chunks.size() && chunks[c].memory)
        c++;
    if (c == chunks.size())
        chunks.emplace_back();
    if (!dedicated)
        ch.free[0] = size;
    chunks[c] = std::move(ch);
    stats.poolBytes += size;
    stats.poolChunks++;
    index = c;
    return true;
}

// A span's first fit in a shared chunk, else a chunk made now: of its own over half a chunk, else a shared one. Where the grower
// thread runs (the render server, warm()), the shared chunks grow ahead of need as the pool's do, so a pass's memoryless
// backings, made at commit, find room rather than wiring a chunk on the commit path (r115: boot 15's keyboard took 64 such
// imports of 16 MiB at ~8.5 ms each as ~10 ms encodes).
bool Device::tiledAlloc(std::vector<Chunk> &chunks, uint32_t kind, uint64_t bytes, uint64_t align, Span &s)
{
    align = std::max(align, kBlAlign);
    bytes = (bytes + kBlAlign - 1) & ~(kBlAlign - 1);
    s = {};
    const bool dedicated = bytes > sizes_.blChunk / 2;
    for (int pass = 0; pass < 3; pass++) {
        for (uint32_t c = 0; c < chunks.size(); c++) {
            Chunk &ch = chunks[c];
            if (!ch.memory || ch.dedicated)
                continue;
            for (auto it = ch.free.begin(); it != ch.free.end(); ++it) {
                const uint64_t start = it->first, len = it->second, at = (start + align - 1) & ~(align - 1);
                if (at + bytes > start + len)
                    continue;
                ch.free.erase(it);
                if (at > start)
                    ch.free[start] = at - start;
                if (at + bytes < start + len)
                    ch.free[at + bytes] = start + len - at - bytes;
                ch.used += bytes;
                s = {c, at, bytes, ch.bo.va + at, static_cast<uint8_t *>(ch.memory) + at};
                if (grower_.joinable() && !tiledWanted(chunks) && tiledFreeBytes(chunks) < sizes_.blChunk) {
                    tiledWanted_.push_back({&chunks, kind});  // the next chunk, off this thread
                    grow_.notify_one();
                }
                return true;
            }
        }
        if (pass == 2)
            break;
        if (dedicated) {
            uint32_t c;
            if (!newTiledChunk(chunks, kind, bytes, true, nullptr, c))
                return false;
            chunks[c].used = bytes;
            s = {c, 0, bytes, chunks[c].bo.va, static_cast<uint8_t *>(chunks[c].memory)};
            return true;
        }
        stats.poolStalls++;
        if (tiledWanted(chunks)) {  // the grower is making one: wait for it rather than wire a second
            std::unique_lock<std::mutex> l(lock_, std::adopt_lock);
            // bounded, as the pool's wait (a chunk not wired in 20 s is a stuck kernel import)
            if (!grown_.wait_for(l, std::chrono::seconds(20), [&] {
                    return !tiledWanted(chunks);
                })) {
                os_log_fault(
                    os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                    "raw: block-linear chunk not wired after 20 s (the grower's import): abort() for the stack");
                abort();
            }
            l.release();
            continue;
        }
        uint32_t c;
        if (!newTiledChunk(chunks, kind, chunkSize(chunks, sizes_.blChunk, bytes, kBlAlign), false, nullptr, c))
            return false;
    }
    return false;
}

void Device::tiledFree(std::vector<Chunk> &chunks, Span &s)
{
    if (!s.valid() || s.chunk >= chunks.size())
        return;
    Chunk &ch = chunks[s.chunk];
    ch.used -= s.bytes;
    if (ch.dedicated) {
        freeChunk(ch);
        s = {};
        return;
    }
    uint64_t offset = s.offset, bytes = s.bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    s = {};
    // an empty chunk goes back while the others keep a chunk's worth free, as the pool's do: a burst's peak is not held for
    // good (boot 15: backboardd's chunks stayed at 912 MB after its keyboard's burst)
    if (!ch.used && tiledFreeBytes(chunks) >= ch.bytes + sizes_.blChunk)
        freeChunk(ch);
}

bool Device::createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    const uint32_t fmt2d = fx::twodFormat(format);
    if (!f || levels < 2 || !f->rt || xrForm(format) != XrForm::None)
        return false;
    Texture::Chain *c = new Texture::Chain;
    if (!fx::mipChainLayout(width, height, levels, f->bpp, c->layout) || !shape(width, height, format, t)) {
        delete c;
        return false;
    }
    {
        std::lock_guard<std::mutex> l(lock_);
        if (!blAlloc(c->layout.bytes, c->layout.align, t.span)) {
            delete c;
            return false;
        }
        if (!(t.tic = allocTic())) {
            Lease *none = nullptr;
            freeSpan(t.span, Memory::Bl, none);
            delete c;
            return false;
        }
    }
    const fx::MipLevel &l0 = c->layout.level[0];
    t.va = c->va = t.span.va;
    t.cpu = t.span.cpu;
    t.pitch = l0.rowBytes;
    t.blockLinear = true;
    t.blockHeight = l0.yLog2;
    t.layerBytes = c->layout.bytes;
    c->fmt2d = fmt2d;
    uint32_t w[8];
    fx::mipChainTic(c->layout, c->va, f->tic0, f->srgb, 0, levels, w);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // the texture's header: the chain's (visible at the next header invalidate)
    t.chain = c;
    stats.chains++;
    stats.chainBytes += c->layout.bytes;
    return true;
}

void Device::destroyTexture(Texture &t)
{
    if (t.view) {
        std::lock_guard<std::mutex> l(lock_);
        if (t.tic) {
            freeTic(t.tic);
        }
        t = {};
        return;
    }
    for (Texture *plane : {t.luma, t.chroma, t.planarStandIn, t.stencilTemp})
        if (plane) {
            destroyTexture(*plane);
            delete plane;
        }
    t.luma = t.chroma = t.planarStandIn = t.stencilTemp = nullptr;
    if (t.standIn) {
        destroyTexture(*t.standIn);
        delete t.standIn;
        t.standIn = nullptr;
    }
    if (t.sampled) {  // its header is the pool image's
        destroyTexture(*t.sampled);
        delete t.sampled;
        t.sampled = nullptr;
        t.tic = 0;
    }
    std::lock_guard<std::mutex> l(lock_);
    if (t.tic) {
        freeTic(t.tic);
    }
    if (t.chain) {
        for (const uint32_t level : t.chain->levelTic)
            if (level) {
                freeTic(level);
            }
        delete t.chain;
        t.chain = nullptr;
    }
    if (t.import)
        release(t.import);
    freeSpan(t.span, memoryOf(t), t.lease);
    t = {};
}

bool Device::createBuffer(uint64_t length, Buffer &b, bool wc)
{
    b = {};
    if (!length)
        length = 16;
    std::lock_guard<std::mutex> l(lock_);
    if (wc) {
        if (wcAlloc(length, b.span)) {
            b.wc = true;
            b.va = b.span.va;
            b.cpu = b.span.cpu;
            b.length = length;
            stats.wcBytes += b.span.bytes;
            return true;
        }
        stats.wcFallbacks++;
    }
    if (!poolAlloc(length, b.span))
        return false;
    b.va = b.span.va;
    b.cpu = b.span.cpu;
    b.length = length;
    return true;
}

// ---- write-combined buffers: the GPU pool (BoAlloc coherent), mapped write-combined for the CPU ----

// lock_ held. First fit by page; a new chunk when none has room (a BoAlloc: tens of microseconds, the GPU pool's own)
bool Device::wcAlloc(uint64_t bytes, Span &s)
{
    bytes = (bytes + pageSize() - 1) & ~uint64_t(pageSize() - 1);
    s = {};
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t c = 0; c < wcChunks_.size(); c++) {
            Chunk &ch = wcChunks_[c];
            if (!ch.bo.handle || ch.dedicated)
                continue;
            for (auto it = ch.free.begin(); it != ch.free.end(); ++it) {
                if (it->second < bytes)
                    continue;
                const uint64_t at = it->first, left = it->second - bytes;
                ch.free.erase(it);
                if (left)
                    ch.free[at + bytes] = left;
                ch.used += bytes;
                s = {c, at, bytes, ch.bo.va + at, ch.bo.cpu + at};
                return true;
            }
        }
        if (pass)
            break;
        const bool dedicated = bytes > sizes_.wcChunk / 2;
        const uint64_t size = dedicated ? bytes : chunkSize(wcChunks_, sizes_.wcChunk, bytes, pageSize());
        Chunk ch;
        if (!k_.alloc(size, true, ch.bo))
            return false;  // the GPU pool is full: pool memory instead (createBuffer)
        ch.memory = ch.bo.cpu;
        ch.bytes = size;
        ch.dedicated = dedicated;
        uint32_t c = 0;
        while (c < wcChunks_.size() && wcChunks_[c].bo.handle)
            c++;
        if (c == wcChunks_.size())
            wcChunks_.emplace_back();
        if (dedicated) {
            ch.used = bytes;
            s = {c, 0, bytes, ch.bo.va, ch.bo.cpu};
            wcChunks_[c] = std::move(ch);
            return true;
        }
        ch.free[0] = size;
        wcChunks_[c] = std::move(ch);
    }
    return false;
}

// lock_ held
void Device::wcFree(Span &s)
{
    if (!s.valid() || s.chunk >= wcChunks_.size())
        return;
    Chunk &ch = wcChunks_[s.chunk];
    ch.used -= s.bytes;
    stats.wcBytes -= s.bytes;
    if (ch.dedicated) {
        k_.free(ch.bo);
        ch = Chunk();
        s = {};
        return;
    }
    uint64_t offset = s.offset, bytes = s.bytes;
    auto next = ch.free.lower_bound(offset);
    if (next != ch.free.end() && offset + bytes == next->first) {
        bytes += next->second;
        next = ch.free.erase(next);
    }
    bool merged = false;
    if (next != ch.free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            prev->second += bytes;
            merged = true;
        }
    }
    if (!merged)
        ch.free[offset] = bytes;
    s = {};
    // an empty chunk goes back to the GPU pool while another empty one stays for the next burst: the compositor's pool
    // share follows what Core Animation holds, not its peak (r8a: the compositor 107 MiB of the pool at 15:16, 63 after,
    // and Maps refused a 4 MiB BO with 33 MiB free)
    if (!ch.used) {
        for (const Chunk &other : wcChunks_)
            if (&other != &ch && other.bo.handle && !other.dedicated && !other.used) {
                k_.free(ch.bo);  // (the kernel keeps the pages until the work submitted before has completed)
                ch = Chunk();
                break;
            }
    }
}

uint32_t Device::wcChunksHeld()
{
    std::lock_guard<std::mutex> l(lock_);
    uint32_t n = 0;
    for (const Chunk &ch : wcChunks_)
        n += ch.bo.handle && !ch.dedicated;
    return n;
}

uint64_t Device::wcChunkBytesHeld()
{
    std::lock_guard<std::mutex> l(lock_);
    uint64_t n = 0;
    for (const Chunk &ch : wcChunks_)
        n += ch.bo.handle && !ch.dedicated ? ch.bytes : 0;
    return n;
}

uint64_t Device::blChunkFreeBytes()
{
    std::lock_guard<std::mutex> l(lock_);
    return tiledFreeBytes(blChunks_);
}

// The caller's pages around [bytes, bytes + length): the frontend answers minBufferNoCopyAlignmentBytes with the kernel's 4 KiB
// page, not getpagesize()'s 16 KiB, and Apple's own callers (iconservicesagent on 27) pass pointers inside a page; the import
// binds whole kernel pages and the buffer's address is its first byte's inside them
bool Device::createBufferNoCopy(void *bytes, uint64_t length, Buffer &b)
{
    b = {};
    if (!bytes || !length)
        return false;
    std::lock_guard<std::mutex> l(lock_);
    if (!(b.import = importSurface(0, bytes, length, nullptr)))
        return false;
    b.va = b.import->bo.va;
    b.cpu = static_cast<uint8_t *>(bytes);
    b.length = length;
    return true;
}

void Device::destroyBuffer(Buffer &b)
{
    std::lock_guard<std::mutex> l(lock_);
    if (b.import)
        release(b.import);
    freeSpan(b.span, b.arena ? Memory::Arena : b.wc ? Memory::Wc : Memory::Pool, b.lease);
    b = {};
}

// Metal's level clamp bounds as m2v hands them to Vulkan (metal2vulkan metal/sampler.rs vk_sampler: Metal's measured grid of
// sixty-fourths, a nearest mip filter's bounds the levels it reads at them, NotMipmapped as [0, 0.25])
namespace {
float levelOnGrid(float c)
{
    c = std::max(c, 0.0f);
    return c >= 64.0f ? c : std::floor(c * 64.0f) / 64.0f;
}

float nearestLevel(float c)
{
    c = levelOnGrid(c);
    if (c >= 15.0f)
        return 15.0f;
    const float whole = std::floor(c);
    return std::min(15.0f, whole + (c - whole > 0.5f ? 1.0f : 0.0f));
}

// Mesa's __set_ufixed for the 4.8 LOD fields: clamped to what 12 bits hold, truncated
uint32_t lodField(float v)
{
    return uint32_t(std::clamp(v, 0.0f, 4095.0f / 256.0f) * 256.0f) & 0xfff;
}
}  // namespace

// Sampler header (Mesa NVK nvk_sampler_get_header) of the Vulkan sampler m2v makes of the Metal descriptor (vk_sampler): the
// address modes, anisotropy (rounded up to a power of two: MAX_ANISOTROPY, TRILIN_OPT, MIN_ANISO for a linear minification), and
// the LOD clamps on Metal's grid; clampToZero is BORDER with the transparent black border.
void samplerHeader(const SamplerDesc &d, uint32_t w[8])
{
    memset(w, 0, 32);
    auto address = [](uint8_t m) -> uint32_t {  // MTLSamplerAddressMode -> NV9097_TEXSAMP0_ADDRESS_U_*
        switch (m) {
        case 0:
            return 2;  // clampToEdge -> CLAMP_TO_EDGE
        case 1:
            return 5;  // mirrorClampToEdge -> MIRROR_ONCE_CLAMP_TO_EDGE
        case 2:
            return 0;  // repeat -> WRAP
        case 3:
            return 1;  // mirrorRepeat -> MIRROR
        default:
            return 3;  // clampToZero -> BORDER (transparent black)
        }
    };
    // m2v hands anisotropy to a device that samples Metal's way (its profile's device_samples, NVK's) whatever the filters
    uint32_t aniso = 0;  // 0: off
    if (d.maxAnisotropy > 1) {
        aniso = 1;
        while (aniso < d.maxAnisotropy)
            aniso <<= 1;
        aniso = std::min(aniso, 16u);
    }
    setField(w, 2, 0, address(d.sAddress));
    setField(w, 5, 3, address(d.tAddress));
    setField(w, 8, 6, address(d.rAddress));
    setField(w, 13, 13, 1);  // S_R_G_B_CONVERSION
    setField(w, 16, 14, 1);  // FONT_FILTER_WIDTH SIZE_2
    setField(w, 19, 17, 1);  // FONT_FILTER_HEIGHT SIZE_2
    if (aniso)
        setField(w, 22, 20, aniso >= 16 ? 7 : aniso >= 12 ? 6 : aniso >> 1);  // MAX_ANISOTROPY
    setField(w + 1, 2, 0, d.magFilter ? 2 : 1);                               // MAG_LINEAR / MAG_POINT
    setField(w + 1, 5, 4, d.minFilter ? (aniso ? 3 : 2) : 1);                 // MIN_ANISO / MIN_LINEAR / MIN_POINT
    setField(w + 1, 7, 6, d.mipFilter == 2 ? 3 : 2);                          // MIP_LINEAR / MIP_POINT
    setField(w + 1, 9, 9, 1);  // CUBEMAP_INTERFACE_FILTERING AUTO_SPAN_SEAM
    w[1] |= samplerCoordinateBits(d);
    // TRILIN_OPT from the descriptor's maxAnisotropy as Vulkan's (vk_to_9097_trilin_opt: 1 when anisotropy is off)
    const float va = aniso ? float(aniso) : 1.0f;
    setField(w + 1, 30, 26, va >= 12 ? 0 : va >= 4 ? 6 : va >= 2 ? 4 : 0);
    const float lo = d.lodMin, hi = d.lodMax;
    const bool grid = d.mipFilter != 0 && d.minFilter == d.magFilter && d.maxAnisotropy <= 1;
    auto onGrid = [&](float bound, float level) {
        return (!grid && bound > 0.0f && level <= 0.0f) ? bound : level;
    };
    float minLod, maxLod;
    if (d.mipFilter == 0) {
        if (hi > 0.0f) {
            minLod = std::clamp(lo, 0.0f, 0.25f);
            maxLod = 0.25f;
        } else {
            minLod = maxLod = 0.0f;
        }
    } else if (d.mipFilter == 1) {
        minLod = onGrid(lo, nearestLevel(lo));
        maxLod = onGrid(hi, nearestLevel(hi));
    } else {
        minLod = onGrid(lo, levelOnGrid(lo));
        maxLod = onGrid(hi, levelOnGrid(hi));
    }
    maxLod = std::max(maxLod, minLod);
    setField(w + 2, 11, 0, lodField(minLod));
    setField(w + 2, 23, 12, lodField(maxLod));
}

// lock_ held: a free entry of the sampler header pool
bool Device::takeTsc(uint32_t &tsc)
{
    if (!freeTsc_.empty()) {
        tsc = freeTsc_.back();
        freeTsc_.pop_back();
        return true;
    }
    if (nextTsc_ >= kTscCount)
        return false;
    tsc = nextTsc_++;
    return true;
}

bool Device::createSampler(const SamplerDesc &d, Sampler &s)
{
    uint32_t w[8];
    samplerHeader(d, w);
    s.desc = d;
    std::lock_guard<std::mutex> l(lock_);
    if (!takeTsc(s.tsc))
        return false;
    memcpy(tsc_.cpu + 32 * s.tsc, w, 32);
    return true;
}

void Device::destroySampler(Sampler &s)
{
    std::lock_guard<std::mutex> l(lock_);
    if (s.tsc)
        freeTsc_.push_back(s.tsc);
    for (uint32_t m : s.mapped)
        if (m && m != s.tsc && std::find(freeTsc_.begin(), freeTsc_.end(), m) == freeTsc_.end())
            freeTsc_.push_back(m);
    s = {};
}

// m2v's address map of a planar texture (metal2vulkan texture/ycbcr_image.rs `ycbcr_modes`, Metal's measured reads): a 4:2:0
// texture samples the wrapping modes (mirror-clamp-to-edge 1, repeat 2, mirrored repeat 3) as clamp to the edge; one read as
// codes clamp to zero (4) and clamp to the border color (5) too, which a converted one reads as the border (alpha 1: its header's
// W_SOURCE); a 4:4:4 texture read as codes clamp to zero and to the border as clamp to the edge, the others as they are. (r16 sent
// every mode of a converted 4:2:0 texture to the edge: its clamps to zero read the edge texels, not the border m2v reads.)
bool Device::samplerVariant(Sampler &s, uint8_t map)
{
    if (!map || map > 3)
        return false;
    if (s.mapped[map])
        return true;
    SamplerDesc d = s.desc;
    const bool wrapped = map == 1 || map == 3, clamps = map == 2 || map == 3;
    for (uint8_t *m : {&d.sAddress, &d.tAddress, &d.rAddress})
        if ((wrapped && *m >= 1 && *m <= 3) || (clamps && (*m == 4 || *m == 5)))
            *m = 0;
    if (d.sAddress == s.desc.sAddress && d.tAddress == s.desc.tAddress && d.rAddress == s.desc.rAddress) {
        s.mapped[map] = s.tsc;
        return true;
    }
    uint32_t w[8];
    samplerHeader(d, w);
    std::lock_guard<std::mutex> l(lock_);
    uint32_t tsc = 0;
    if (!takeTsc(tsc))
        return false;
    memcpy(tsc_.cpu + 32 * tsc, w, 32);
    s.mapped[map] = tsc;
    return true;
}

Pipeline *Device::ycbcrPipeline(uint8_t output)
{
    static const char *const names[] = {"", "raw.ycbcr.ycbcr_converted_frag", "raw.ycbcr.ycbcr_srgb_frag",
                                        "raw.ycbcr.ycbcr_codes_frag", "raw.ycbcr.ycbcr_codes10_frag"};
    if (!output || output > 4)
        return nullptr;
    return pipeline(Pack::fnv(names[output], strlen(names[output])));
}

Pipeline *Device::xrPipeline(XrForm form)
{
    const char *name = xrConversion(form);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

Pipeline *Device::resolvePipeline(uint32_t format)
{
    const char *name = resolveState(format);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

bool Device::createStandIn(Texture &t)
{
    if (t.standIn || !xrPipeline(xrForm(t.format)))
        return t.standIn != nullptr;
    Texture *s = new Texture;
    if (!createTexture(t.width, t.height, kXrStandInFormat, *s, false, true)) {
        delete s;
        return false;
    }
    t.standIn = s;
    return true;
}

bool Device::createYcbcrTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t)
{
    if (!ycbcrPipeline(plan.output))
        return false;
    Texture *y = new Texture, *c = plan.chromaWidth ? new Texture : nullptr;
    auto undo = [&] {
        for (Texture *x : {y, c})
            if (x) {
                destroyTexture(*x);
                delete x;
            }
        return false;
    };
    if (!createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.luma, width, height, plan.lumaPitch,
                              plan.lumaFormat, *y))
        return undo();
    if (c && !createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.chroma, plan.chromaWidth,
                                   plan.chromaHeight, plan.chromaPitch, plan.chromaFormat, *c))
        return undo();
    if (!createTexture(width, height, plan.image, t, false, true))  // block-linear, as NVK makes m2v's converted image
        return undo();
    t.luma = y;
    t.chroma = c;
    memcpy(t.shape, plan.shape, sizeof t.shape);
    t.ycbcr = plan.output;
    t.addressMap = plan.addressMap;
    // its alpha read as one, as m2v's view of its own converted image reads it (W_SOURCE ONE_FLOAT; the conversion writes 1 there
    // too: the same texels, now the same header)
    {
        std::lock_guard<std::mutex> l(lock_);
        uint32_t *w = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic);
        w[0] = (w[0] & ~(7u << 28)) | 7u << 28;
    }
    return true;
}

// A memoryless attachment's memory: one per size, format, tag and layout, kept while passes use it (m2v 0105 gives a memoryless
// attachment linear memory in a pass that renders to linear memory; NVK renders a pass with a block-linear colour target or a
// depth/stencil attachment into block-linear colour). Called with the device lock held.
Device::PlainMem *Device::largestPlain(uint32_t tag, uint32_t memory) const
{
    PlainMem *best = nullptr;
    for (const auto &m : plainMems_)
        if (m->tag == tag && m->memory == memory && (!best || m->span.bytes > best->span.bytes))
            best = m.get();
    return best;
}

Device::Backing *Device::plainBacking(const Texture &m, uint32_t tag, bool blockLinear)
{
    Texture t;
    fx::MipChain c;
    uint64_t bytes = 0, align = kBlAlign;
    uint32_t memory = kPlainPool;
    const FormatInfo *f = formatInfo(m.format);
    if (m.zeta) {
        memory = kPlainZeta + zetaFormatIndex(m.format);
        uint64_t zetaAlign = 0;
        if (!zetaShape(m.width, m.height, m.format, m.samples, t, bytes, zetaAlign))
            return nullptr;
        align = std::max(align, zetaAlign);
    } else if (blockLinear) {
        memory = kPlainBl;
        if (!f || !shape(m.width, m.height, m.format, t) || !fx::msLayout(m.width, m.height, m.samples, t.bpp, c))
            return nullptr;
        bytes = c.bytes;
        align = std::max<uint64_t>(align, c.align);
        t.pitch = c.level[0].rowBytes;
        t.blockLinear = true;
        t.blockHeight = c.level[0].yLog2;
        t.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
        t.samples = m.samples;
    } else {
        if (!f || !shape(m.width, m.height, m.format, t))
            return nullptr;
        t.pitch = (m.width * t.bpp + 255) & ~255u;
        bytes = uint64_t(t.pitch) * t.height;
    }
    PlainMem *mem = largestPlain(tag, memory);
    if (!mem || mem->span.bytes < bytes || (mem->span.va & (align - 1))) {
        auto made = std::make_unique<PlainMem>();
        made->tag = tag;
        made->memory = memory;
        made->align = align;
        const uint64_t want = sizeClassBytes(bytes, kBlAlign);
        const bool ok = memory == kPlainPool ? poolAlloc(want, made->span)
                        : memory == kPlainBl
                            ? blAlloc(want, align, made->span)
                            : tiledAlloc(zChunks_[memory - kPlainZeta], kZetaFormatInfo[memory - kPlainZeta].kind, want,
                                         align, made->span);
        if (!ok || (made->span.va & (align - 1))) {
            if (ok)
                freePlainSpan(*made);
            return nullptr;
        }
        stats.backingBytes += made->span.bytes;
        plainMems_.push_back(std::move(made));
        mem = plainMems_.back().get();
        // the views of its tag's smaller memory: no pass takes them again (an animated layer seldom asks a size twice), so it
        // goes back once their last uses complete rather than 10 s on
        for (Backing *b : backings_)
            if (b->plain && b->plain != mem && b->plain->tag == tag && b->plain->memory == memory)
                b->retired = true;
        compRearm_ = true;  // an idle process gives it back when it turns stale
        compStart();
    }
    t.va = mem->span.va;
    t.cpu = memory == kPlainPool ? mem->span.cpu : nullptr;
    if (!m.zeta) {
        if (!(t.tic = allocTic())) {
            if (!mem->views)
                dropPlain(mem);
            return nullptr;
        }
        if (blockLinear) {
            uint32_t w[8];
            fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, 1, w);
            memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
        } else {
            t.renderable = t.renderable && linearTarget(t);
            writeTic(t);
        }
    }
    auto *b = new Backing;
    b->texture = t;
    b->tag = tag;
    b->plain = mem;
    b->usedNs = compClock();
    mem->views++;
    stats.backings++;
    return b;
}

void Device::freePlainSpan(PlainMem &mem)
{
    if (mem.memory == kPlainPool)
        poolFree(mem.span);
    else if (mem.memory == kPlainBl)
        blFree(mem.span);
    else
        tiledFree(zChunks_[mem.memory - kPlainZeta], mem.span);
}

void Device::dropPlain(PlainMem *mem)
{
    stats.backingBytes -= mem->span.bytes;
    freePlainSpan(*mem);
    plainMems_.erase(std::find_if(plainMems_.begin(), plainMems_.end(), [&](const std::unique_ptr<PlainMem> &p) {
        return p.get() == mem;
    }));
}

void Device::plainFree(Backing &b)
{
    if (b.texture.tic)
        freeTic(b.texture.tic);
    stats.backings--;
    PlainMem *mem = b.plain;
    b.plain = nullptr;
    if (!--mem->views)
        dropPlain(mem);
}

Texture *Device::backing(const Texture &m, uint32_t tag, bool blockLinear)
{
    blockLinear |= m.zeta;
    const uint32_t kind = compKind(m, tag, blockLinear);
    const CompMem *mem = kind ? comp_.largest(tag, kind) : nullptr;
    Backing *plain = nullptr;
    blockLinear |= m.samples > 1;
    const auto asked = [&](Backing *b) {
        b->lastCommit = commitCount_;
        b->lastClosed = closedCount_;
        return &b->texture;
    };
    for (Backing *b : backings_)
        if (!b->retired && b->texture.width == m.width && b->texture.height == m.height &&
            b->texture.format == m.format && b->tag == tag && b->texture.blockLinear == blockLinear &&
            b->texture.samples == m.samples) {
            if (b->plain && b->plain != largestPlain(tag, b->plain->memory)) {
                b->retired = true;  // larger plain memory of its tag took its place: a view of that replaces it
                continue;
            }
            if (!kind || (b->comp && b->comp == mem))
                return asked(b);
            if (b->comp)
                b->retired = true;  // larger memory of its tag and kind took its place: a view of that replaces it
            else
                plain = b;
        }
    if (Backing *c = kind ? compBacking(m, tag, kind) : nullptr) {
        if (plain)
            plain->retired = true;
        backings_.push_back(c);
        return asked(c);
    }
    if (plain)
        return asked(plain);
    // a memoryless attachment's (tag 0: a zeta's): plain memory shared by its tag's backings, in size classes; a shadow
    // target's (tag 1 + j) is its own
    if (tag == 0 || tag >= kMemorylessTag) {
        Backing *v = plainBacking(m, tag, blockLinear);
        if (!v)
            return nullptr;
        backings_.push_back(v);
        return asked(v);
    }
    Backing *b = new Backing;
    Texture &t = b->texture;
    if (m.zeta) {
        uint64_t bytes, align;
        const uint32_t z = zetaFormatIndex(m.format);
        if (!zetaShape(m.width, m.height, m.format, m.samples, t, bytes, align) ||
            !tiledAlloc(zChunks_[z], kZetaFormatInfo[z].kind, bytes, align, t.span)) {
            delete b;
            return nullptr;
        }
        t.va = t.span.va;
    } else if (blockLinear) {
        fx::MipChain c;
        const FormatInfo *f = formatInfo(m.format);
        if (!f || !shape(m.width, m.height, m.format, t) || !fx::msLayout(m.width, m.height, m.samples, t.bpp, c) ||
            !blAlloc(c.bytes, c.align, t.span) || !(t.tic = allocTic())) {
            blFree(t.span);
            delete b;
            return nullptr;
        }
        t.va = t.span.va;
        t.pitch = c.level[0].rowBytes;
        t.blockLinear = true;
        t.blockHeight = c.level[0].yLog2;
        t.layerBytes = uint64_t(c.level[0].rowBytes) * c.level[0].rows;
        t.samples = m.samples;
        uint32_t w[8];
        fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, 1, w);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
    } else {
        if (!shape(m.width, m.height, m.format, t)) {
            delete b;
            return nullptr;
        }
        t.pitch = (m.width * t.bpp + 255) & ~255u;
        if (!poolAlloc(uint64_t(t.pitch) * t.height, t.span)) {
            delete b;
            return nullptr;
        }
        t.va = t.span.va;
        t.cpu = t.span.cpu;
        if (!(t.tic = allocTic())) {
            poolFree(t.span);
            delete b;
            return nullptr;
        }
        t.renderable = t.renderable && linearTarget(t);
        writeTic(t);
    }
    b->tag = tag;
    b->usedNs = compClock();
    backings_.push_back(b);
    stats.backings++;
    stats.backingBytes += t.span.bytes;
    return asked(b);
}

// lock_ held
void Device::freeBacking(Texture &t)
{
    if (t.tic) {
        freeTic(t.tic);
    }
    if (t.zeta)
        tiledFree(zChunks_[zetaFormatIndex(t.format)], t.span);
    else if (t.blockLinear)
        blFree(t.span);
    else
        poolFree(t.span);
}

// Backings no pass used for 600 command buffers (~10 s of frames) and whose last use completed go back to the pool; a retired
// one (a compressed backing, or one on larger memory of its tag, replaced it) as soon as its last use completed, as no pass
// takes it again; one on compressed or shared plain memory after 10 s unused too, also while the process commits nothing
// (raw_comp.h backingStale; the compressed-memory thread trims then).
void Device::trimBackings()
{
    const uint64_t now = compClock();
    compTrim(now);
    for (auto it = spares_.begin(); it != spares_.end();) {
        if (commitCount_ - it->freedAt > 600) {
            freeChunk(chunks_[it->chunk]);
            it = spares_.erase(it);
            continue;
        }
        ++it;
    }
    uint64_t retired = 0;
    for (auto it = backings_.begin(); it != backings_.end();) {
        Backing *b = *it;
        if (backingStale(commitCount_, b->lastCommit, closedCount_, b->lastClosed, now, b->usedNs, b->retired,
                         b->comp || b->plain) &&
            (!b->lastUse || done(b->lastUse))) {
            if (b->comp) {
                compFree(*b);
            } else if (b->plain) {
                plainFree(*b);
            } else {
                stats.backings--;
                stats.backingBytes -= b->texture.span.bytes;
                freeBacking(b->texture);
            }
            delete b;
            it = backings_.erase(it);
            continue;
        }
        retired += !b->comp && b->retired;
        ++it;
    }
    stats.retiredBackings = retired;
    uint64_t tiledFree = tiledFreeBytes(blChunks_);
    for (const std::vector<Chunk> &z : zChunks_)
        tiledFree += tiledFreeBytes(z);
    stats.tiledFreeBytes = tiledFree;
}

// ---- command buffer ----

// A wait the kernel refuses with no kill (the call failed, or the submit thread's reopen destroyed the channel meanwhile) is
// no completion: the channel's status decides. A healthy channel is waited on again; one that keeps refusing is taken as
// killed, with everything it has not completed, so nothing it may still run is released or written over as done.
bool Device::waitFence(uint32_t seqno)
{
    constexpr unsigned kRefusals = 3;
    const uint32_t ch = channel_, opened = k_.opened();
    for (unsigned refused = 0; !done(seqno);) {
        if (k_.wait(ch, seqno))
            break;
        TegraGM20BAbiStatus st;
        if (k_.lost(ch) || !k_.status(ch, st)) {  // killed, or destroyed by a reopen that kept its kill record
            noteLost();
            break;
        }
        if (st.error) {
            k_.markLost(ch, opened);
            noteLost();
            break;
        }
        if (int32_t(st.completed - seqno) >= 0)
            return !lost(seqno);
        if (++refused == kRefusals) {
            os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                         "raw: the kernel refused %u waits on channel %u's seqno %u (%s): taken as killed", refused, ch,
                         seqno, k_.error().c_str());
            {
                std::lock_guard<std::mutex> l(killedLock_);
                killedKept_[killedNext_++ % killedKept_.size()] = {
                    st.completed + 1, __atomic_load_n(&lastSeqno_, __ATOMIC_RELAXED), kWaitRefused};
            }
            k_.markLost(ch, opened);  // begin() makes it again
            noteLost();
            return false;
        }
    }
    return !lost(seqno);
}

void Device::noteLost()
{
    lostThrough_.store(__atomic_load_n(&lastSeqno_, __ATOMIC_RELAXED), std::memory_order_relaxed);
    lostAny_.store(true, std::memory_order_release);
}

// The barriers order the record after the caller's read of the fence word, and its range after its error: the kernel
// writes the range, the error, then the fence word.
bool Device::liveKill(Killed &k) const
{
    const volatile uint32_t *fence = fence_;
    if (!k_.killRecords() || fence == &kNoFence)
        return false;
    const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
    std::atomic_thread_fence(std::memory_order_acquire);
    k.error = record->error;
    if (!k.error)
        return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    k.first = record->killed_first;
    k.last = record->killed_last;
    return true;
}

bool Device::killed(uint32_t seqno) const
{
    const auto covers = [seqno](const Killed &k) {
        return k.error && int32_t(seqno - k.first) >= 0 && int32_t(k.last - seqno) >= 0;
    };
    Killed live = {};
    if (liveKill(live) && covers(live)) {
        killedError_.store(live.error, std::memory_order_relaxed);
        return true;
    }
    std::lock_guard<std::mutex> l(killedLock_);
    for (const Killed &kept : killedKept_)
        if (covers(kept)) {
            killedError_.store(kept.error, std::memory_order_relaxed);
            return true;
        }
    return false;
}

// The kernel killed the channel (a GPU fault or hang, its own or one it was stuck behind; as a GPU restart on iOS): what it
// had in flight failed, and a new channel, named by ChannelCreate's reply, gets the context the old one took with it.
// Between command buffers: no span of either ring is open, and every one the old channel had is done (the kernel released
// its fence), whatever word the new channel's fence is.
std::string Device::vaOwner(uint64_t va)
{
    char line[96];
    auto in = [&](const Bo &bo, uint64_t bytes, const char *what) {
        const uint64_t base = bo.vaBase ? bo.vaBase : bo.va;
        if (!base || va < base || va - base >= bytes)
            return false;
        snprintf(line, sizeof line, "%s +0x%llx", what, (unsigned long long)(va - base));
        return true;
    };
    auto chunk = [&](const std::vector<Chunk> &chunks, const char *what) {
        for (size_t i = 0; i < chunks.size(); i++)
            if (chunks[i].bo.handle && in(chunks[i].bo, chunks[i].bo.bytes, what))
                return true;
        return false;
    };
    std::lock_guard<std::mutex> l(lock_);
    if (in(heap_, heapBound_, "the shader heap") || in(heap_, heap_.bytes, "the shader heap's unbound reservation") ||
        in(push_.bo, push_.bo.bytes, "the push ring") || in(upload_.bo, upload_.bo.bytes, "the upload ring") ||
        in(root_, root_.bytes, "the root table") || in(tic_, tic_.bytes, "the texture headers") ||
        in(tsc_, tsc_.bytes, "the sampler headers") || in(zero_, zero_.bytes, "the zero page") ||
        in(tickets_, tickets_.bytes, "the interlock tickets") ||
        in(colorTable_, colorTable_.bytes, "the colour table") || in(stamps_, stamps_.bytes, "the GPU time stamps") ||
        chunk(chunks_, "a pool chunk") || chunk(blChunks_, "a block-linear chunk") ||
        std::any_of(std::begin(zChunks_), std::end(zChunks_),
                    [&](const std::vector<Chunk> &z) {
                        return chunk(z, "a zeta chunk");
                    }) ||
        chunk(wcChunks_, "a write-combined chunk"))
        return line;
    for (const auto &[key, i] : surfaces_)
        if (in(i->bo, i->bo.bytes, i->kind == TGM_LAYOUT_KIND ? "a block-linear IOSurface" : "a pitch IOSurface"))
            return std::string(line) + " (IOSurface " + std::to_string(i->id) + ")";
    return "none of the raw device's memory";
}

bool Device::reopen()
{
    if (!contextless_) {  // the first try for this death: what the channel had failed, its kill record is kept
        noteLost();
        Killed live = {};
        if (liveKill(live)) {
            k_.markLost(channel_, k_.opened());
            std::lock_guard<std::mutex> l(killedLock_);
            killedKept_[killedNext_++ % killedKept_.size()] = live;
        }
        contextless_ = true;
    }
    // Closed once: the connection is NVK's, whose next channel may take the slot number
    if (channel_)
        k_.closeChannel(channel_);
    channel_ = 0;
    fence_ = &kNoFence;
    auto refused = [&] {
        std::lock_guard<std::mutex> l(channelLock_);
        channelError_ = k_.error();
        return false;
    };
    if (!k_.channel(kEngines, channel_, fence_))
        return refused();
    stats.reopens++;
    push_.inflight.clear();
    upload_.inflight.clear();
    {
        std::lock_guard<std::mutex> l(lock_);
        for (Backing *b : backings_)
            b->lastUse = 0;
        heapInvalidated_ = ~0ull;
    }
    slmChannel_ = 0;
    memset(rootPush_, 0, sizeof rootPush_);
    restart_ = 0;
    const uint32_t words = contextWords() + uint32_t(fx::twodInitWords());
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, words * 4, 256, va));
    Push p{start, start + words};
    emitContext(p);
    // The interlock's tickets start at zero with the new context's dispensers (a fresh context: its dispenser values are
    // zero already). Warps of the dead channel may have held tickets, and their lines may sit dirty in the GPU's L2, so the
    // copy engine zeroes the buffer in the new channel's own order, done before the 3D engine runs again.
    p.inc(CE_OFFSET_IN_UPPER,
          {uint32_t(zero_.va >> 32), uint32_t(zero_.va), uint32_t(tickets_.va >> 32), uint32_t(tickets_.va)},
          SUBC_COPY);
    p.inc(CE_LINE_LENGTH_IN, {uint32_t(tickets_.bytes), 1}, SUBC_COPY);
    p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);
    p.m(WAIT_FOR_IDLE, 0);
    fx::emitTwodInit(p);
    TegraGM20BAbiPush seg = {va, uint32_t(p.at - start), 0};
    uint32_t seq = 0;
    if (!k_.exec(channel_, &seg, 1, 0, seq))
        return refused();
    lastSeqno_ = seq;
    retire(push_, seq);
    contextless_ = false;
    return true;
}

// Between command buffers: a ring that made an allocation wait for the GPU doubles (to `most`), the old one freed once the work
// that used it completed, so a client's rings follow what its command buffers use.
void Device::growRing(Ring &r, uint64_t most)
{
    const bool grow = r.waited && r.bo.bytes < most;
    r.waited = false;
    Bo larger;
    if (!grow || !k_.alloc(std::min(r.bo.bytes * 2, most), true, larger))
        return;
    grownOut_.push_back({r.bo, lastSeqno_});
    r.bo = larger;
    r.head = r.since = r.clear = 0;
    r.inflight.clear();
}

CommandBuffer *Device::begin()
{
    if (!ensureChannel())
        return nullptr;
    Killed live = {};
    if ((contextless_ || k_.lost(channel_) || liveKill(live)) && !reopen())
        return nullptr;
    for (auto it = grownOut_.begin(); it != grownOut_.end();)
        if (done(it->second)) {
            k_.free(it->first);
            it = grownOut_.erase(it);
        } else {
            ++it;
        }
    growRing(push_, kSizes[0].push);
    growRing(upload_, kSizes[0].upload);
    CommandBuffer &cb = cb_;
    cb.dev_ = this;
    cb.segments_.clear();
    cb.clean_.clear();
    cb.flush_.clear();
    cb.backings_.clear();
    cb.surfaces_.clear();
    cb.draws = cb.passes = cb.blits = 0;
    cb.refusal = {};
    cb.partial = false;
    overflowing_ = false;
    overflow_.clear();
    cb.pushStart_ = push_.head;
    cb.uploadStart_ = upload_.head;
    memcpy(cb.rootPush_, rootPush_, sizeof rootPush_);
    cb.restart_ = restart_;
    cb.ds_ = ds_;
    cb.extrasLive_ = extrasLive_;
    cb.drawn_ = false;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (commitCount_ % 64 == 0)
            trimBackings();
        cb.stamp = stampOf(commitCount_);
    }
    // submission prologue, as NVK opens each submission: header/sampler caches (headers written since), constant, shader data and
    // texture caches, and the instruction caches where commit finds the heap grew
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, 64 * 4, 256, va));
    Push p{start, start + 64};
    emitStamp(p, stamps_.va + stampOffset(cb.stamp), cb.stamp);
    p.m(INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
    p.m(INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
    p.inc(INVALIDATE_SHADER_CACHES_NO_WFI, {kInvalidateConstant});
    cb.shaderInvalidate_ = p.at - 1;
    p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
    p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    p.m(INVALIDATE_SHADER_CACHES_NO_WFI, kInvalidateData);
    checkRoom(p, "a submission prologue");
    cb.segment(start, p.at);
    return &cb;
}

void CommandBuffer::segment(uint32_t *from, uint32_t *to)
{
    if (to == from)
        return;
    const uint64_t va = dev_->push_.bo.va + (reinterpret_cast<uint8_t *>(from) - dev_->push_.bo.cpu);
    // contiguous with the last segment: one GPFIFO entry
    if (!segments_.empty() && segments_.back().va + uint64_t(segments_.back().dwords) * 4 == va &&
        segments_.back().dwords + uint64_t(to - from) < (1u << 21) - 1) {
        segments_.back().dwords += uint32_t(to - from);
        return;
    }
    segments_.push_back({va, uint32_t(to - from), 0});
}

void CommandBuffer::clean(const void *cpu, uint64_t bytes, bool invalidate)
{
    if (!cpu || !bytes)
        return;
    auto &list = invalidate ? flush_ : clean_;
    const uintptr_t a = uintptr_t(cpu);
    if (!list.empty() && a >= list.back().first &&
        a <= list.back().first + list.back().second) {  // CA's rings: adjacent
        list.back().second = std::max<uint64_t>(list.back().second, a + bytes - list.back().first);
        return;
    }
    list.push_back({a, bytes});
}

RenderEncoder *CommandBuffer::render(const PassDesc &pass)
{
    RenderEncoder &e = render_;
    // a pass's lists keep the last one's capacity: no allocation a steady frame
    std::vector<std::pair<uint32_t *, uint32_t *>> bodies = std::move(e.bodies_);
    std::vector<std::array<uint32_t, 4>> rects = std::move(e.rects_);
    bodies.clear();
    rects.clear();
    e = RenderEncoder();
    e.bodies_ = std::move(bodies);
    e.rects_ = std::move(rects);
    e.cb_ = this;
    e.dev_ = dev_;
    e.pass_ = pass;
    // the render hardware takes one memory layout for all of a pass's color targets (nvk_rendering_linear): block-linear when
    // one of them is, or is a pitch target the hardware cannot take (rendered through a shadow either way), or beside a zeta,
    // its pitch ones then rendered through block-linear shadows as NVK does; memoryless ones get a backing of the pass's layout
    const Texture *zeta = pass.depthStencil.texture;
    if (zeta && !zeta->zeta)
        return nullptr;
    // every attachment of one sample count (Metal's rule)
    uint8_t samples = zeta ? zeta->samples : 0;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture) {
            if (samples && t->samples != samples)
                return nullptr;
            samples = t->samples;
        }
    e.samples_ = std::max<uint8_t>(samples, 1);
    bool blockLinear = zeta != nullptr;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture; t && !t->memoryless && (t->blockLinear || t->shadowTarget))
            blockLinear = true;
    if (zeta && zeta->memoryless) {
        std::lock_guard<std::mutex> l(dev_->lock_);
        Texture *b = dev_->backing(*zeta);
        if (!b)
            return nullptr;
        e.pass_.depthStencil.texture = b;
        backings_.push_back(b);
    }
    for (uint32_t j = 0; j < pass.count && j < 8; j++) {
        const Texture *t = pass.color[j].texture;
        if (!t)
            continue;
        if (!t->renderable && !t->shadowTarget)
            return nullptr;
        const bool mixed = blockLinear && !t->blockLinear && !t->memoryless;
        if (t->shadowTarget || mixed) {  // rendered through a pool image (end(): copied in before, back after)
            if (mixed) {
                dev_->stats.rtMixed++;
            } else {
                dev_->stats.rtUnaligned++;
                dev_->stats.rtUnalignedPitch = t->pitch;
                dev_->stats.rtUnalignedVa = uint32_t(t->va & 255);
            }
            std::lock_guard<std::mutex> l(dev_->lock_);
            Texture *b = dev_->backing(*t, 1 + j, blockLinear);
            if (!b || !b->renderable)
                return nullptr;
            e.pass_.color[j].texture = b;
            e.shadowOf_[j] = t;
            backings_.push_back(b);
            continue;
        }
        if (t->memoryless) {  // back it for this pass
            std::lock_guard<std::mutex> l(dev_->lock_);
            Texture *b = dev_->backing(*t, Device::kMemorylessTag + j, blockLinear);
            if (!b)
                return nullptr;
            e.pass_.color[j].texture = b;
            e.memoryless_[j] = true;
            backings_.push_back(b);
        }
    }
    // the render area: from the origin, the smallest attachment's extent, each axis within the pass's target size where it
    // names one (m2v render_pass.rs `area`); Metal's default viewport and scissor cover it (render_state.rs `metal_defaults`)
    const Texture *extents[9] = {zeta};
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        extents[1 + j] = e.pass_.color[j].texture;
    for (const Texture *t : extents)
        if (t) {
            e.areaW_ = e.areaW_ ? std::min(e.areaW_, t->width) : t->width;
            e.areaH_ = e.areaH_ ? std::min(e.areaH_, t->height) : t->height;
        }
    if (pass.width)
        e.areaW_ = std::min(e.areaW_, pass.width);
    if (pass.height)
        e.areaH_ = std::min(e.areaH_, pass.height);
    // a Clear load is a color write the prologue's CLEAR_SURFACE makes ahead of the draws: a draw that fetches the color
    // takes the fetch barrier after it, as NVK's first fetch after its clears does, or it reads what was there before
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (e.pass_.color[j].texture && e.pass_.color[j].load == ColorAttachment::Clear)
            e.writtenBits_ |= uint8_t(1u << j);
    if (e.writtenBits_) {
        DrawGeometry cleared;
        cleared.limit = {0, 0, e.areaW_, e.areaH_};
        e.written_.add(cleared);
    }
    // the body goes straight into the push ring; the pass prologue (targets, clears) is written at end() and runs first
    e.reserve();
    e.setViewport(0, 0, e.areaW_, e.areaH_, 0, 1);
    e.setScissor(0, 0, e.areaW_, e.areaH_);
    e.extrasAtBegin_ = extrasLive_;
    extrasLive_ = nullptr;
    passes++;
    return &e;
}

BlitEncoder *CommandBuffer::blit()
{
    BlitEncoder &e = blit_;
    e = BlitEncoder();
    e.cb_ = this;
    e.dev_ = dev_;
    uint64_t va;
    e.start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    e.p_ = Push{e.start_, e.start_ + (1 << 12)};
    // what the render work before wrote is complete before the copy engine reads it (NVK: a WFI on the 3D subchannel; the
    // switch to the copy subchannel then waits for the 3D engine anyway)
    e.p_.m(WAIT_FOR_IDLE, 0);
    blits++;
    return &e;
}

bool CommandBuffer::commit(uint32_t &seqno)
{
    Device &d = *dev_;
    if (d.overflowing_) {
        os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                     "raw: a command buffer larger than its ring, with no memory for a larger one: not submitted");
        return false;
    }
    bool invalidates = false;
    uint64_t invalidatedBefore = 0;
    uint64_t slmGiven = 0, slmVa = 0;
    {
        std::lock_guard<std::mutex> l(d.lock_);
        if (shaderInvalidate_ && d.heapPlaced_ != d.heapInvalidated_) {
            *shaderInvalidate_ = kInvalidateConstant | kInvalidateInstruction;
            invalidates = true;
            invalidatedBefore = d.heapInvalidated_;
            d.heapInvalidated_ = d.heapPlaced_;
        }
        if (d.slmPerWarp_ != d.slmChannel_) {
            slmGiven = d.slmPerWarp_;
            slmVa = d.slm_.va;
        }
    }
    // ahead of everything in it: a program placed after begin() may ask for more than the channel has
    if (slmGiven && !segments_.empty()) {
        uint64_t va;
        auto *head = reinterpret_cast<uint32_t *>(d.alloc(d.push_, kSlmWords * 4, 4, va));
        Push p{head, head + kSlmWords};
        emitSlm(p, slmVa, slmArea(slmGiven, d.k_.info().tpc_count));
        segments_.insert(segments_.begin(), {va, uint32_t(p.at - head), 0});
    }
    if (!segments_.empty()) {
        uint64_t va;
        auto *tail = reinterpret_cast<uint32_t *>(d.alloc(d.push_, (kTiledCacheFlushWords + kStampWords) * 4, 4, va));
        Push p{tail, tail + kTiledCacheFlushWords + kStampWords};
        if (tiledCache(d.k_.info()))
            p.at = tiledCacheFlush(p.at);
        emitStamp(p, d.stamps_.va + stampOffset(stamp), stamp);
        segment(tail, p.at);
    }
    // CPU writes the GPU reads: to memory, by address (no set/way operation, nothing global)
    uint64_t cleaned = 0;
#if defined(__aarch64__)
    for (const auto &r : clean_) {
        for (uintptr_t a = r.first & ~uintptr_t(63); a < r.first + r.second; a += 64)
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
        cleaned += r.second;
    }
    for (const auto &r : flush_) {
        for (uintptr_t a = r.first & ~uintptr_t(63); a < r.first + r.second; a += 64)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        cleaned += r.second;
    }
    // the cleans, and the write-combined ring writes, complete before the kernel rings GP_PUT
    __asm__ volatile("dsb sy" ::: "memory");
#else
    for (const auto &r : clean_)
        cleaned += r.second;
#endif
    d.stats.cleanBytes += cleaned;
    uint32_t seq = 0;
    for (size_t at = 0; at < segments_.size(); at += kMaxSegments) {
        const uint32_t n = uint32_t(std::min<size_t>(kMaxSegments, segments_.size() - at));
        if (!d.k_.exec(d.channel_, segments_.data() + at, n, d.lastSeqno_, seq, &surfaces_)) {
            if (!at && invalidates) {  // its prologue never reached the GPU: the next submission's invalidates
                std::lock_guard<std::mutex> l(d.lock_);
                d.heapInvalidated_ = invalidatedBefore;
            }
            if (at) {  // its first batches run on: abandon() may give their ring words back only once they completed
                partial = true;
                d.wait(d.lastSeqno_);
                d.contextless_ = true;
            }
            return false;
        }
        d.lastSeqno_ = seq;
    }
    seqno = seq;
    if (slmGiven)
        d.slmChannel_ = slmGiven;
    d.retire(d.push_, seq);
    d.retire(d.upload_, seq);
    memcpy(d.rootPush_, rootPush_, sizeof rootPush_);
    d.restart_ = restart_;
    d.ds_ = ds_;
    d.extrasLive_ = extrasLive_;
    {
        std::lock_guard<std::mutex> l(d.lock_);
        const uint64_t now = compClock();
        for (Texture *b : backings_)
            for (Device::Backing *k : d.backings_)
                if (&k->texture == b) {
                    k->lastUse = seq;
                    k->usedNs = now;
                }
        d.commitCount_++;
        d.closedCount_++;
    }
    d.stats.commits++;
    return true;
}

bool Device::gpuNanoseconds(uint32_t stamp, uint64_t &ns) const
{
    if (!stamp || !stamps_.cpu)
        return false;
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    return stampedNanoseconds(*slot, stamp, ns);
}

void CommandBuffer::abandon()
{
    Device &d = *dev_;
    {
        std::lock_guard<std::mutex> l(d.lock_);
        d.closedCount_++;
    }
    d.overflowing_ = false;
    d.overflow_.clear();
    for (Device::Ring *r : {&d.push_, &d.upload_}) {
        // drop the open command buffer's spans and give back what it took (nothing else was allocated since: one encodes)
        for (auto it = r->inflight.begin(); it != r->inflight.end();)
            it = it->seqno ? std::next(it) : r->inflight.erase(it);
        r->head = r->since = (r == &d.push_ ? pushStart_ : uploadStart_);
        if (!r->outgrown.empty()) {  // it began in a ring it grew out of: the new one is empty
            for (const Bo &b : r->outgrown)
                d.grownOut_.push_back({b, d.lastSeqno_});
            r->outgrown.clear();
            r->head = r->since = 0;
        }
    }
    segments_.clear();
    clean_.clear();
    flush_.clear();
    surfaces_.clear();
}

// ---- render encoder ----

// Room for the pass body: 16K dwords at a time (a CA pass of 100 draws is ~6K); a full one becomes a segment and the body goes on
// in a new one, so a pass never drops a draw.
void RenderEncoder::reserve()
{
    checkRoom(p_, "a pass body");
    if (bodyStart_ && p_.at > bodyStart_)
        bodies_.push_back({bodyStart_, p_.at});
    if (bodyStart_)
        dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    uint64_t va;
    const uint32_t room = 1 << 14;
    bodyStart_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, room * 4, 256, va));
    p_ = Push{bodyStart_, bodyStart_ + room};
}

void RenderEncoder::setBytes(int stage, uint32_t slot, const void *bytes, uint32_t length)
{
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    uint64_t va;
    const uint32_t chunk = (length + kUboChunk - 1) & ~(kUboChunk - 1);
    uint8_t *m = dev_->alloc(dev_->upload_, chunk ? chunk : kUboChunk, kUboChunk, va);
    memcpy(m, bytes, length);
    memset(m + length, 0, (chunk ? chunk : kUboChunk) - length);
    bufs_[stage][slot] = {va, length, nullptr, 0,
                          m};  // m2v binds the Metal length (its upload chunks are 256-byte aligned)
    dev_->stats.uploadBytes += chunk;
}

void RenderEncoder::setBytesAt(int stage, uint32_t slot, uint64_t va, uint32_t length, const uint8_t *cpu)
{
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    bufs_[stage][slot] = {va, length, nullptr, 0, cpu};
}

void RenderEncoder::setBuffer(int stage, uint32_t slot, const Buffer *b, uint64_t offset)
{
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    if (!b) {
        bufs_[stage][slot] = {};
        return;
    }
    bufs_[stage][slot] = {b->va + offset,
                          uint32_t(std::min<uint64_t>(b->length - std::min(offset, b->length), 0xffffffffu)), b, offset,
                          b->cpu && offset < b->length ? b->cpu + offset : nullptr};
}

void RenderEncoder::setBufferOffset(int stage, uint32_t slot, uint64_t offset)
{
    if (slot >= 31 || stage < 1 || stage > 2 || !bufs_[stage][slot].buffer)
        return;
    setBuffer(stage, slot, bufs_[stage][slot].buffer, offset);
}

void RenderEncoder::setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    // a rectangle past the render area is clipped to it, as plain Metal clips it (m2v render_state.rs `set_scissors`)
    const uint32_t x0 = std::min(x, areaW_), y0 = std::min(y, areaH_);
    w = uint32_t(std::min<uint64_t>(uint64_t(x) + w, areaW_)) - x0;
    h = uint32_t(std::min<uint64_t>(uint64_t(y) + h, areaH_)) - y0;
    x = x0;
    y = y0;
    sc_[0] = x;
    sc_[1] = y;
    sc_[2] = w;
    sc_[3] = h;
    scissor_[0] = (x + w) << 16 | x;
    scissor_[1] = (y + h) << 16 | y;
    scissorDirty_ = true;
}

// Metal's viewport as m2v gives it to Vulkan (y flipped: y + h, height -h) and NVK writes it (nvk_emit_viewport)
void RenderEncoder::setViewport(double x, double y, double w, double h, double znear, double zfar)
{
    vp_[0] = x;
    vp_[1] = y;
    vp_[2] = w;
    vp_[3] = h;
    const float vy = float(y + h), vh = float(-h);
    viewport_[0] = fbits(0.5f * float(w));
    viewport_[1] = fbits(0.5f * vh);
    viewport_[2] = fbits(float(zfar - znear));
    viewport_[3] = fbits(float(x) + 0.5f * float(w));
    viewport_[4] = fbits(vy + 0.5f * vh);
    viewport_[5] = fbits(float(znear));
    const float xmin = std::clamp(float(x), 0.0f, 65535.0f), xmax = std::clamp(float(x + w), 0.0f, 65535.0f);
    const float ymin = std::clamp(std::min(vy, vy + vh), 0.0f, 65535.0f),
                ymax = std::clamp(std::max(vy, vy + vh), 0.0f, 65535.0f);
    viewport_[6] = uint32_t(xmax - xmin) << 16 | uint32_t(xmin);
    viewport_[7] = uint32_t(ymax - ymin) << 16 | uint32_t(ymin);
    viewportDirty_ = true;
}

void RenderEncoder::setBlendColor(const float rgba[4])
{
    uint32_t v[4];
    memcpy(v, rgba, sizeof v);
    if (memcmp(v, blendColor_, sizeof v)) {
        memcpy(blendColor_, v, sizeof v);
        blendColorDirty_ = true;
    }
}

void RenderEncoder::setDepthStencil(const DepthStencilDesc *d)
{
    ds_ = d ? *d : DepthStencilDesc();
    dsDirty_ = true;
}

void RenderEncoder::setStencilReference(uint32_t front, uint32_t back)
{
    stencilRef_[0] = front;
    stencilRef_[1] = back;
    dsDirty_ = true;
}

// The full barrier m2v puts between passes: the shaders' data writes flushed (storage buffers and textures; ROP writes are
// behind the wait for idle) and their data and texture caches invalidated
void RenderEncoder::barrier()
{
    if (p_.room() < 64)
        reserve();
    tiledCacheDrain(p_, dev_->k_.info());
    p_.m(WAIT_FOR_IDLE, 0);
    p_.m(INVALIDATE_SHADER_CACHES, kBetweenPasses);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    writtenBits_ = storageWrittenBits_ = 0;
    written_.clear();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    anyWritten_ = false;
    ropWritten_ = storageWritten_ = 0;
#endif
}

bool RenderEncoder::draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
                         const char **why)
{
    Pipeline *pl = pipeline_;
    if (!pl)
        return true;  // m2v drops a draw without a pipeline (parity 0006) the same way
    called_ = true;
    readBits_ |= pl->fetchBits;
    if (!count || !instances)
        return true;
    const Runs &runs = selfRuns(*pl, primitive, first, count, instances);
    if (!runs.empty()) {
        for (const auto &[begin, end] : runs) {
#ifdef SWITCH_RAW_HARNESS_MARKS
            // the host frame harness's builds only: nvdec names this mark SPLIT, and eqcheck folds the run into the one before
            if (begin) {
                if (p_.room() < 1024)
                    reserve();
                p_.m(NO_OPERATION, 0x5b1175);
            }
#endif
            if (!prepare(pl, primitive, first + begin, first + begin, end - begin, baseInstance, false, why))
                return false;
            p_.m(BEGIN, 0);
            p_.inc(VERTEX_ARRAY_START, {first + begin, end - begin});
            p_.m(END, 0);
        }
        cb_->draws++;
        return true;
    }
    if (!prepare(pl, primitive, first, first, count, baseInstance, false, why))
        return false;
    for (uint32_t i = 0; i < instances; i++) {
        if (p_.room() < kInstanceWords)
            reserve();
        p_.m(BEGIN, i ? 1u << 26 : 0);  // INSTANCE_ID FIRST, then SUBSEQUENT (Maxwell has no instance count)
        p_.inc(VERTEX_ARRAY_START, {first, count});
        p_.m(END, 0);
    }
    cb_->draws++;
    return true;
}

// The runs a draw that fetches a color it writes is drawn as, where its own triangles may cover a pixel twice
// (raw_overlap.h); empty: it is drawn whole. The indexed draw being prepared sets drawIndices_ first
const RenderEncoder::Runs &RenderEncoder::selfRuns(const Pipeline &pl, uint32_t primitive, int32_t first,
                                                   uint32_t count, uint32_t instances)
{
    static thread_local Runs runs;
    static thread_local TriangleRuns triangles;
    runs.clear();
    if (!(pl.fetchBits & pl.writeBits) || (primitive != 3 && primitive != 4) || count <= 3)
        return runs;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const auto start = std::chrono::steady_clock::now();
#endif
    const bool told = instances == 1 &&
                      triangles.split(geometry(pl, primitive, first, count, {0, 0, ~0u, ~0u}), primitive == 4, runs);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    dev_->stats.selfOverlapNs += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    if (!told)
        dev_->stats.selfOverlapUnbounded++;
    else if (!runs.empty()) {
        dev_->stats.selfOverlapSplit++;
        dev_->stats.selfOverlapRuns += runs.size();
    }
#else
    (void)told;
#endif
    return runs;
}

// The largest index of `count` indices of type T at `at` (any alignment), restarts (all ones) left out; 0 for none.
template <typename T> static uint32_t largestIndex(const uint8_t *at, uint32_t count)
{
    T most = 0;
    for (uint32_t i = 0; i < count; i++) {
        T v;
        memcpy(&v, at + size_t(i) * sizeof(T), sizeof(T));
        v = v == T(~T(0)) ? T(0) : v;
        most = v > most ? v : most;
    }
    return most;
}

// NVK's indexed draw on MAXWELL_B: the index buffer as NVK_MME_BIND_IB sets it (start, last byte, the restart index of the type,
// the index size) where it changed, the draw parameters as NVK_MME_DRAW_INDEXED's (the base vertex in the root table and the
// base registers, the base instance), then per instance BEGIN, SET_INDEX_BUFFER_F (first index) and DRAW_INDEX_BUFFER (count),
// END. m2v binds the indices the draw reads.
bool RenderEncoder::drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib,
                                uint64_t offset, uint32_t instances, int32_t baseVertex, uint32_t baseInstance,
                                const char **why)
{
    Pipeline *pl = pipeline_;
    if (!pl)
        return true;
    called_ = true;
    readBits_ |= pl->fetchBits;
    if (!count || !instances)
        return true;
    if (!ib || (indexSize != 2 && indexSize != 4) || !ib->cpu) {
        *why = "an indexed draw the raw path does not take";
        cb_->refusal = {RefIndexedRange, uint32_t(offset), indexSize,
                        ib ? uint32_t(std::min<uint64_t>(ib->length, ~0u)) : 0};
        return false;
    }
    // Metal's fetch (m2v render.rs draw_indexed_primitives, measured): the offset's low 32 bits rounded down to the index size,
    // indices read up to the length rounded up to 4 bytes, index 0 at every position past that: drawn from a zero-padded copy
    offset = uint32_t(offset);
    offset -= offset % indexSize;
    const uint64_t fetchEnd = offset < ib->length ? (ib->length + 3) & ~uint64_t(3) : ib->length,
                   inside = fetchEnd > offset ? (fetchEnd - offset) / indexSize : 0;
    Buffer padded;
    if (count > inside) {
        const uint64_t bytes = uint64_t(count) * indexSize;
        uint8_t *m =
            dev_->alloc(dev_->upload_, (bytes + kUboChunk - 1) & ~uint64_t(kUboChunk - 1), kUboChunk, padded.va);
        memcpy(m, ib->cpu + offset, inside * indexSize);
        memset(m + inside * indexSize, 0, bytes - inside * indexSize);
        padded.cpu = m;
        padded.length = bytes;
        padded.wc = true;
        dev_->stats.uploadBytes += bytes;
        ib = &padded;
        offset = 0;
    }
    // the vertices it reads: up to its largest index (the indices are CPU-written, as the vertices). A restart (the all-ones
    // index) reads as 0, which never raises the largest: branch-free per index size, so clang vectorizes it (UMAXV; r9-simd)
    // (write-combined vertices are cleaned by no one: no largest index wanted; write-combined indices neither cleaned nor read)
    const uint8_t *at = ib->cpu + offset;
    const uint8_t slot = pl->rec->vertexSlot;
    const Buffer *vb = slot < 31 ? bufs_[1][slot].buffer : nullptr;
    const uint32_t most = !vb || vb->wc || !pl->rec->vertexStride ? 0
                          : indexSize == 2                        ? largestIndex<uint16_t>(at, count)
                                                                  : largestIndex<uint32_t>(at, count);
    if (!ib->wc)
        cb_->clean(at, uint64_t(count) * indexSize);
    drawIndices_ = at;
    drawIndexSize_ = indexSize;
    drawIndexCount_ = count;
    // the vertices the indices reach from the base vertex, the ones a negative base puts before the stream left out
    const int64_t vFirst = std::max<int64_t>(0, baseVertex),
                  vEnd = std::max<int64_t>(0, int64_t(baseVertex) + most + 1);
    const Runs &runs = selfRuns(*pl, primitive, baseVertex, count, instances);
    const uint32_t parts = runs.empty() ? 1 : uint32_t(runs.size());
    for (uint32_t r = 0; r < parts; r++) {
        const uint32_t begin = runs.empty() ? 0 : runs[r].first, end = runs.empty() ? count : runs[r].second;
        drawIndices_ = at + uint64_t(begin) * indexSize;
        drawIndexCount_ = end - begin;
#ifdef SWITCH_RAW_HARNESS_MARKS
        if (r) {
            if (p_.room() < 1024)
                reserve();
            p_.m(NO_OPERATION, 0x5b1175);
        }
#endif
        const bool prepared = prepare(pl, primitive, baseVertex, uint32_t(vFirst),
                                      uint32_t(std::max<int64_t>(0, vEnd - vFirst)), baseInstance, true, why);
        drawIndices_ = nullptr;
        if (!prepared)
            return false;
        const uint64_t va = ib->va + offset, last = va + uint64_t(count) * indexSize - 1;
        if (va != ibVa_ || last != ibEnd_ || indexSize != ibSize_) {
            p_.inc(INDEX_BUFFER_A, {uint32_t(va >> 32), uint32_t(va), uint32_t(last >> 32), uint32_t(last)});
            p_.m(DA_PRIMITIVE_RESTART_INDEX, indexSize == 2 ? 0xffffu : 0xffffffffu);
            cb_->restart_ = indexSize == 2 ? 0xffffu : 0xffffffffu;
            p_.m(INDEX_BUFFER_E, indexSize == 2 ? 1 : 2);  // INDEX_SIZE TWO_BYTES / FOUR_BYTES
            ibVa_ = va;
            ibEnd_ = last;
            ibSize_ = indexSize;
        }
        // each instance: INSTANCE_ID FIRST, then SUBSEQUENT (nvk_mme_build_draw_indexed_loop); SET_INDEX_BUFFER_F (first
        // index) and DRAW_INDEX_BUFFER (count)
        for (uint32_t i = 0; i < instances; i++) {
            if (p_.room() < kInstanceWords)
                reserve();
            p_.m(BEGIN, i ? 1u << 26 : 0);
            p_.inc(INDEX_BUFFER_F, {begin, end - begin});
            p_.m(END, 0);
        }
    }
    cb_->draws++;
    return true;
}

bool RenderEncoder::prepare(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t vFirst, uint32_t vCount,
                            uint32_t baseInstance, bool indexed, const char **why)
{
    const uint32_t count = vCount;
    const bool firstDraw = !drew_;
    drew_ = true;
    readBits_ |= pl->readBits;
    // the pixels it may write: its scissor within its viewport's pixels (m2v vk_scissors, viewport_pixels)
    auto axis = [](double origin, double size, uint32_t limit, uint32_t lo, uint32_t hi, uint32_t out[2]) {
        const double a = size < 0 ? origin + size : origin, b = size < 0 ? origin : origin + size;
        const int64_t first = int64_t(std::clamp(std::floor(a), 0.0, double(limit)));
        const int64_t end =
            size == 0 ? first : std::max<int64_t>(first, int64_t(std::clamp(std::ceil(b), 0.0, double(limit))));
        out[0] = uint32_t(std::max<int64_t>(lo, first));
        out[1] = std::max(out[0], uint32_t(std::min<int64_t>(hi, end)));
    };
    uint32_t x[2], y[2];
    const bool empty = vp_[2] == 0 || vp_[3] == 0;
    axis(vp_[0], empty ? 0 : vp_[2], areaW_, sc_[0], sc_[0] + sc_[2], x);
    axis(vp_[1], empty ? 0 : vp_[3], areaH_, sc_[1], sc_[1] + sc_[3], y);
    if (!whole_) {
        if (x[1] > x[0] && y[1] > y[0]) {
            const std::array<uint32_t, 4> r = {x[0], y[0], x[1], y[1]};
            if (std::find(rects_.begin(), rects_.end(), r) == rects_.end())
                rects_.push_back(r);
            if (rects_.size() > 1024) {
                whole_ = true;
                rects_.clear();
            }
        }
    }
    for (uint32_t j = 0; j < 8; j++) {  // the pass's attachments are the formats the state was made for
        const Texture *t = j < pass_.count ? pass_.color[j].texture : nullptr;
        if ((t ? t->format : 0) != pl->colorFormats[j] && !(t && pl->colorFormats[j] == 0)) {
            *why = "a state whose color formats differ from the pass's";
            cb_->refusal = {RefColorFormats, j, t ? t->format : 0, pl->colorFormats[j]};
            return false;
        }
    }
    const Texture *zeta = pass_.depthStencil.texture;
    // Metal tests a state's depth and stencil against the pass's hidden attachments where it has none (metal2vulkan
    // depth_stencil.rs depth_without_attachment), which NVK's gating does not give
    if (pl->depthFormat != (zeta ? zeta->format : 0) || (!zeta && ds_.active())) {
        *why = "a depth/stencil state or format the pass's depth/stencil attachment does not take";
        cb_->refusal = {RefDepthStencil, pl->depthFormat, zeta ? zeta->format : 0, ds_.active()};
        return false;
    }
    if (pl->samples != samples_ || pl->samplePasses > 1) {
        *why = "a state whose samples a pixel or per-sample shading the pass's attachments do not take";
        cb_->refusal = {RefSamples, pl->samples, samples_, pl->samplePasses};
        return false;
    }
    if (p_.room() < 1024 + pl->methods.size() + (cb_->extrasLive_ ? cb_->extrasLive_->restore.size() : 0))
        reserve();
    Device &d = *dev_;
    Push &p = p_;
    // NVK's dynamic state is dirty at a command buffer's first draw: its primitive restart index (0 then; an index buffer bind
    // has set its own before an indexed one) is written there
    if (!cb_->drawn_) {
        cb_->drawn_ = true;
        const uint32_t restart = indexed ? cb_->restart_ : 0;
        if (!indexed && cb_->restart_ != restart) {
            p.m(DA_PRIMITIVE_RESTART_INDEX, restart);
            cb_->restart_ = restart;
        }
    }
    if (dsDirty_) {
        emitDepthStencil(p,
                         depthStencilRegs(ds_, stencilRef_[0], stencilRef_[1], zeta && zetaHasDepth(zeta->format),
                                          zeta && zetaHasStencil(zeta->format)),
                         cb_->ds_);
        dsDirty_ = false;
    }
    uint32_t *root = cb_->rootPush_;
    const PackPipeline &r = *pl->rec;
    // set 0 first (it may refuse the draw before anything is written): the layout's entries only
    if (!pl->ubosNamed) {
        *why = "a program reads a set-0 uniform buffer its record does not name";
        cb_->refusal = {};
        return false;
    }
    uint64_t setVa;
    uint32_t *set = reinterpret_cast<uint32_t *>(d.alloc(d.upload_, kSetBytes, 256, setVa));
    uint32_t residuals[32], residualMask = 0;  // the draw's SlotBindOffset residuals by root push word
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackEntry &e = r.entry[i];
        uint32_t *at = set + e.offset / 4;
        if (e.kind == EntryColorTable) {
            at[0] = uint32_t(d.colorTable_.va);
            at[1] = uint32_t(d.colorTable_.va >> 32);
            at[2] = kColorTableBytes;
            at[3] = 0;
            continue;
        }
        if (e.kind == EntryUbo) {
            const Bound &b = bufs_[e.stage][e.slot];
            BufferDescriptor desc;
            if (!bufferDescriptor(e, b.va, b.size, desc)) {
                *why = bufferStorage(e) ? "a storage buffer bound at an offset that is not a multiple of 16, with no "
                                          "residual push word"
                                        : "a uniform buffer bound at an offset that is not a multiple of 256, with no "
                                          "residual push word";
                cb_->refusal = {RefUboOffset, uint32_t(e.stage) << 8 | e.slot, desc.residual,
                                uint32_t(std::min<uint64_t>(b.offset, ~0u))};
                return false;
            }
            if (desc.word >= 0) {
                residuals[desc.word] = desc.residual;
                residualMask |= 1u << desc.word;
            }
            if (b.buffer && !b.buffer->wc)
                cb_->clean(b.buffer->cpu + b.offset, std::min<uint64_t>(desc.range - desc.residual, b.size));
            at[0] = uint32_t(desc.base);
            at[1] = uint32_t(desc.base >> 32);
            at[2] = b.va ? desc.range : 0;
            at[3] = 0;
            continue;
        }
        uint32_t tic = 0, tsc = 0;
        if (e.kind == EntryTexture) {
            const Texture *t = textures_[e.slot];
            if (t && !t->tic) {
                *why = "a texture the raw path cannot sample";
                cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
                return false;
            }
            tic = t ? (t->standIn ? t->standIn->tic : t->tic) : 0;
        } else if (e.kind == EntryFetch) {
            const Texture *t = pass_.color[e.slot].texture;
            if (t && !t->tic) {
                *why = "framebuffer fetch from a target the raw path cannot sample";
                cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
                return false;
            }
            // metal2vulkan's storage view of an 8-bit sRGB color is its texel size's unsigned-integer format, not the
            // texture's header
            const FormatInfo *f = t && (pl->storageBits >> e.slot & 1) ? formatInfo(t->format) : nullptr;
            if (f && f->srgb) {
                *why = "an sRGB color read and written in the pixel interlock";
                cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
                return false;
            }
            tic = t ? t->tic : 0;
        } else if (e.kind == EntryTexture3d) {
            // a texture of another type there is no binding of the argument: m2v writes its neutral, raw an empty handle
            const Texture *t = e.slot < 16 && textures_[e.slot] && textures_[e.slot]->type == fx::kTexThreeD
                                   ? textures_[e.slot]
                                   : nullptr;
            if (t && !t->tic) {
                *why = "a texture the raw path cannot sample";
                cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
                return false;
            }
            tic = t ? t->tic : 0;
        }
        const uint8_t map = e.kind == EntryTexture && textures_[e.slot] ? textures_[e.slot]->addressMap : 0;
        if (e.samplerKind == SamplerSlot) {
            const Sampler *s = samplers_[e.sampler];
            tsc = s ? (map ? s->mapped[map] : s->tsc) : d.neutralTsc_;
            if (s && map && !tsc) {
                *why = "a planar texture's sampler without its address map";
                return false;
            }
        } else if (e.samplerKind == SamplerConstexpr) {
            if (map) {  // m2v compiles a variant of the function for it (RuntimeFact::ConstexprSampled)
                *why = "a planar texture sampled through a constexpr sampler";
                return false;
            }
            tsc = d.constexprTsc_[e.sampler];
        }
        at[0] = tic | tsc << 20;
    }
    writeRaster(p, rasterFor(primitive));
    if (pl != emitted_ || residualMask || residualsPushed_) {
        if (pl != emitted_) {
            blendColorDirty_ |= pl->constantBlend;
            if (cb_->extrasLive_ && cb_->extrasLive_ != pl) {
                p.raw(cb_->extrasLive_->restore.data(), uint32_t(cb_->extrasLive_->restore.size()));
                cb_->extrasLive_ = nullptr;
            }
            p.raw(pl->methods.data(), uint32_t(pl->methods.size()));
            if (!pl->restore.empty())
                cb_->extrasLive_ = pl;
        }
        // the state's push words into the root table, as m2v's vkCmdPushConstants ranges reach it through NVK: the words its
        // ranges cover (the rest keep what an earlier state wrote), the draw's residuals in theirs, one LOAD_CONSTANT_BUFFER per
        // run of words that change
        uint32_t want[32];
        memcpy(want, pl->push, sizeof want);
        for (uint32_t m = residualMask; m; m &= m - 1)
            want[__builtin_ctz(m)] = residuals[__builtin_ctz(m)];
        for (int i = 0; i < 32;) {
            if (!(pl->pushMask >> i & 1) || root[i] == want[i]) {
                i++;
                continue;
            }
            int j = i;
            while (j < 32 && (pl->pushMask >> j & 1) && root[j] != want[j])
                j++;
            p.m(LOAD_CB_OFFSET, 0x700 + 4 * i);
            p.nonInc(LOAD_CB, want + i, j - i);
            memcpy(root + i, want + i, (j - i) * 4);
            i = j;
        }
        emitted_ = pl;
        residualsPushed_ = residualMask != 0;
    }
    const uint32_t topo = nvTopology(primitive), restart = (primitive == 2 || primitive == 4) ? 1 : 0;
    if (topo != topology_) {
        p.m(PRIMITIVE_TOPOLOGY, topo);
        topology_ = topo;
    }
    if (restart != restart_) {
        p.m(DA_PRIMITIVE_RESTART, restart);
        restart_ = restart;
    }
    if (viewportDirty_) {
        p.incArray(VIEWPORT_SCALE_X, viewport_, 6);
        p.inc(VIEWPORT_CLIP_H, {viewport_[6], viewport_[7]});
        viewportDirty_ = false;
    }
    if (scissorDirty_) {
        p.inc(SCISSOR_ENABLE, {1, scissor_[0], scissor_[1]});
        scissorDirty_ = false;
    }
    // NVK's SET_BLEND_CONST_RED..ALPHA, the floats' bits, written where a draw blends with them
    if (pl->constantBlend && blendColorDirty_) {
        p.incArray(BLEND_CONST_RED, blendColor_, 4);
        blendColorDirty_ = false;
    }
    // vertex stream 0 from the pipeline's vertex buffer slot
    if (r.vertexSlot != 0xff) {
        const Bound &b = bufs_[1][r.vertexSlot];
        // the stream ends where the bound bytes end: a setBytes chunk (256-aligned, as m2v's upload blocks) or the buffer's end
        uint64_t location = b.va, bytes = b.buffer ? b.size : (b.size + kUboChunk - 1) & ~uint64_t(kUboChunk - 1);
        if (!location ||
            !bytes) {  // none bound, or none of it left: NVK's empty stream (nvk_mme_bind_vb before Turing)
            location = d.zero_.va;
            bytes = 0;
        }
        const uint64_t limit = location + bytes - 1;
        if (location != vbVa_ || limit != vbLimit_) {
            p.inc(VERTEX_STREAM_LOCATION, {uint32_t(location >> 32), uint32_t(location)});
            p.inc(VERTEX_STREAM_LIMIT, {uint32_t(limit >> 32), uint32_t(limit)});
            vbVa_ = location;
            vbLimit_ = limit;
        }
        if (b.buffer && !b.buffer->wc && r.vertexStride) {  // the vertices this draw reads, CPU-written (cached memory)
            const uint64_t from = b.offset + uint64_t(vFirst) * r.vertexStride;
            const uint64_t bytes = std::min<uint64_t>(uint64_t(count) * r.vertexStride,
                                                      b.buffer->length - std::min(from, b.buffer->length));
            cb_->clean(b.buffer->cpu + from, bytes);
        }
    }
    {
        const uint32_t s[4] = {uint32_t(setVa), uint32_t(setVa >> 32), kSetBytes, 0};
        p.m(LOAD_CB_OFFSET, 0x38);
        p.nonInc(LOAD_CB, s, 4);
    }
    if (pl->setSlot[0] || pl->setSlot[1]) {
        p.inc(CB_SELECTOR_A, {kSetBytes, uint32_t(setVa >> 32), uint32_t(setVa)});
        if (pl->setSlot[0])
            p.m(BIND_GROUP_CB + 0x20 * 0, 1 | uint32_t(pl->setSlot[0]) << 4);
        if (pl->setSlot[1])
            p.m(BIND_GROUP_CB + 0x20 * 4, 1 | uint32_t(pl->setSlot[1]) << 4);
    }
    // set 0's uniform buffers the programs read as constant buffers, at the descriptors just written (NVK binds them from the
    // descriptor: NVK_MME_BIND_CBUF_DESC); one of no bytes is unbound
    for (uint8_t u = 0; u < pl->uboCount; u++) {
        const Pipeline::UboCbuf &c = pl->ubos[u];
        const uint32_t *desc = set + r.entry[c.entry].offset / 4;
        if (desc[2]) {
            p.inc(CB_SELECTOR_A, {std::min(desc[2], kMaxCbufBytes), desc[1], desc[0]});
            p.m(BIND_GROUP_CB + 0x20 * c.group, 1 | uint32_t(c.slot) << 4);
        } else {
            p.m(BIND_GROUP_CB + 0x20 * c.group, uint32_t(c.slot) << 4);
        }
    }
    if (pl->setSlot[0] || pl->setSlot[1] || pl->uboCount)
        p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(d.root_.va >> 32), uint32_t(d.root_.va)});
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    countRules(*pl);
#endif
    // framebuffer fetch reads a color attachment through the texture unit: a barrier where a draw since the last one wrote
    // any color, as m2v's barrier (0106) follows any color write, whichever colors this draw fetches.
    // The barrier holds this draw's pixel shading until the earlier pixels have left the ROP; vertex work and rasterization
    // go on (NVK's for a color-output to fragment-read dependency, as NVIDIA's drivers order one)
    // A storage access in the interlock is ordered after the earlier ones by the ticket, not after the ROP's writes; the
    // ROP's accesses are ordered after neither: a barrier where a texture fetch meets a write of either kind, a storage
    // access meets a ROP write, or a ROP access (a write, or the read a blend or a partial write mask makes) meets a
    // storage write.
    // A fetch through the texture unit that meets only ROP writes needs the barrier only where its pixels meet theirs
    // (raw_region.h); nothing bounds a storage access's pixels.
    const uint8_t texFetched = pl->fetchBits & ~pl->storageBits, ropAccessed = pl->writeBits | pl->readBits;
    const DrawGeometry g = geometry(*pl, primitive, first, indexed ? drawIndexCount_ : count, {x[0], y[0], x[1], y[1]});
    if (firstDraw && !pl->dropsPixels) {
        const uint8_t opaque = pl->writeBits & ~(pl->readBits | pl->fetchBits | pl->storageBits);
        if (opaque && WrittenRegion::covers(g, primitive == 3, areaW_, areaH_))
            covered_ = opaque;
    }
    const bool storage =
        (pl->storageBits & writtenBits_) | (ropAccessed & storageWrittenBits_) | (texFetched & storageWrittenBits_);
    if (storage || (texFetched && writtenBits_)) {
        if (!storage && !written_.meets(g)) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            dev_->stats.fetchBarriersElided++;
#endif
#ifdef SWITCH_RAW_HARNESS_MARKS
            // the host frame harness's builds only (test/build.sh): nvdec names this mark ELIDED, and eqcheck takes a
            // barrier NVK has and raw left out only where it is
            p.m(NO_OPERATION, 0x0e11de);
#endif
        } else {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            dev_->stats.fetchBarriers++;
#endif
            fetchBarrier(p);
            writtenBits_ = storageWrittenBits_ = 0;
            written_.clear();
        }
    }
    if (uint32_t(first) != first_ || baseInstance != baseInstance_) {
        // nvk_mme_build_set_draw_params: root draw.vs.{base_vertex, base_instance} and the base registers
        const uint32_t v[2] = {uint32_t(first), baseInstance};
        p.m(LOAD_CB_OFFSET, 0);
        p.nonInc(LOAD_CB, v, 2);
        p.m(GLOBAL_BASE_INSTANCE_INDEX, baseInstance);
        first_ = uint32_t(first);
        baseInstance_ = baseInstance;
    }
    // the base vertex the hardware adds to an indexed draw's indices (nvk_mme_build_draw_indexed); 0 for a draw of vertices
    const uint32_t baseVertex = indexed ? uint32_t(first) : 0;
    if (baseVertex != baseVertex_) {
        p.m(GLOBAL_BASE_VERTEX_INDEX, baseVertex);
        p.m(VERTEX_ID_BASE, baseVertex);
        baseVertex_ = baseVertex;
    }
    writtenBits_ |= pl->writeBits;
    storageWrittenBits_ |= pl->storageBits;
    if (pl->writeBits)
        written_.add(g);
    return true;
}

// What the region test reads of a draw: Core Animation's transform (Pipeline::caTransform) over a triangle list or strip
// of one instance, with its matrix, positions and indices where the CPU maps them; anything else is its scissor alone
DrawGeometry RenderEncoder::geometry(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count,
                                     const PixelBox &limit) const
{
    DrawGeometry g;
    g.limit = limit;
    g.viewport = {vp_[0], vp_[1], vp_[2], vp_[3]};
    g.first = first;
    g.count = count;
    g.indices = drawIndices_;
    g.indexSize = drawIndexSize_;
    const Bound &m = bufs_[1][2];
    const Bound &v = pl.rec->vertexSlot < 31 ? bufs_[1][pl.rec->vertexSlot] : bufs_[1][0];
    g.bounded = pl.caTransform && pl.positionComponents && (primitive == 3 || primitive == 4) && m.cpu &&
                m.size >= 64 && v.cpu && pl.rec->vertexSlot < 31;
    if (g.bounded) {
        g.matrix = m.cpu;
        g.positions = {v.cpu, pl.rec->vertexStride, pl.positionOffset, pl.positionComponents};
        g.positionBytes = v.size;
    }
    return g;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
const char *Device::fetchBarrierName(FetchBarrier b)
{
    switch (b) {
    case FetchBarrier::PixelShader:
        return "ps";
    case FetchBarrier::WaitForIdle:
        return "wfi";
    case FetchBarrier::PixelShaderOnly:
        return "psonly";
    case FetchBarrier::PixelShaderNoWfiInvalidate:
        return "psnowfi";
    case FetchBarrier::None:
        return "none";
    }
    return "?";
}
#endif

void RenderEncoder::fetchBarrier(Push &p)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    switch (dev_->fetchBarrier.load(std::memory_order_relaxed)) {
    case Device::FetchBarrier::None:
        return;
    case Device::FetchBarrier::WaitForIdle:
        p.m(WAIT_FOR_IDLE, 0);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
        return;
    case Device::FetchBarrier::PixelShaderOnly:
        p.m(PIXEL_SHADER_BARRIER, 0);
        return;
    case Device::FetchBarrier::PixelShaderNoWfiInvalidate:
        p.m(PIXEL_SHADER_BARRIER, 0);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
        return;
    case Device::FetchBarrier::PixelShader:
        break;
    }
#endif
    p.m(PIXEL_SHADER_BARRIER, 0);
    p.m(INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED, 0);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void RenderEncoder::countRules(const Pipeline &pl)
{
    if (pl.fetchBits && anyWritten_) {
        dev_->stats.fetchBarriersAnyColor++;
        anyWritten_ = false;
    }
    anyWritten_ |= pl.writeBits != 0;
    const uint8_t stored = pl.fetchBits & pl.writeBits, ropWrites = pl.writeBits & ~pl.fetchBits;
    if ((pl.fetchBits & ropWritten_) || (ropWrites & storageWritten_)) {
        dev_->stats.fetchBarriersInterlocked++;
        ropWritten_ = storageWritten_ = 0;
    }
    ropWritten_ |= ropWrites;
    storageWritten_ |= stored;
}

#endif

// The clear registers' words: an extended-range target's codes (raw_xr.h; `straight`: Metal's straight route), else m2v's
// value for the format (raw_clear.h)
static void clearWords(uint32_t format, const float in[4], uint32_t out[4], bool straight)
{
    if (const XrForm xr = xrForm(format); xr != XrForm::None) {
        xrClearTexel(xr, in, straight, out);
        return;
    }
    clearValue(format, in, out);
}

namespace {
// One side of a copy-engine copy: pitch (va the first texel's, rows `pitch` apart) or a block-linear image (va its first byte,
// `pitch` its row stride, the copy from texel (x, y) of plane `layer` of its width x height x depth)
struct CopySide {
    uint64_t va = 0;
    uint32_t pitch = 0, x = 0, y = 0, width = 0, height = 0;
    bool blockLinear = false;
    uint8_t blockHeight = 0;
    uint32_t depth = 1, layer = 0;
    uint8_t blockDepth = 0;
};

// z: a 3D texture's plane, or an array's layer (its own 2D image, layerBytes on: NVK's array_stride step)
CopySide sideOf(const Texture &t, uint32_t x, uint32_t y, uint32_t z)
{
    if (!t.blockLinear)
        return {t.va + uint64_t(y) * t.pitch + uint64_t(x) * t.bpp, t.pitch};
    if (t.layers > 1)
        return {t.va + z * t.layerBytes, t.pitch, x, y, t.pitch / t.bpp, t.height, true, t.blockHeight};
    return {t.va, t.pitch, x, y, t.pitch / t.bpp, t.height, true, t.blockHeight, t.depth, z, t.blockDepth};
}

// SET_REMAP_COMPONENTS of a texel copy: DST_X..W = SRC_X..W, COMPONENT_SIZE, NUM_SRC_COMPONENTS, NUM_DST_COMPONENTS
uint32_t texelRemap(uint32_t bpp)
{
    const uint32_t comp = std::min(bpp, 4u), n = bpp / comp;
    return 0 | 1 << 4 | 2 << 8 | 3 << 12 | (comp - 1) << 16 | (n - 1) << 20 | (n - 1) << 24;
}

// SET_REMAP_COMPONENTS of one-, two- or four-byte components (`size`): DST_X..W each SRC_X..W (0..3) or NO_WRITE (6), the
// source's and destination's component counts (clb0b5)
constexpr uint32_t kRemapNoWrite = 6;

constexpr uint32_t remapOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t src, uint32_t dst)
{
    return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (src - 1) << 20 | (dst - 1) << 24;
}

// `w` x `h` texels as nouveau_copy_rect moves them: a remap of components (`bytes` of each texel moved), so lengths, widths and
// origins count texels; a block-linear side by its block size (one GOB wide, Fermi's 8-row GOBs), its width the row stride in
// texels, a 3D image's plane by its depth and layer. `ordered`: NON_PIPELINED, after every earlier copy has completed (one that
// reads what the copy before it wrote)
void copyRemapped(Push &p, const CopySide &s, const CopySide &d, uint32_t w, uint32_t h, uint32_t remap, uint32_t bytes,
                  Device::Stats &stats, bool ordered = false)
{
    if (!w || !h || !bytes)
        return;
    stats.ceOps++;
    stats.ceBytes += uint64_t(w) * bytes * h;
    p.m(CE_SET_REMAP_COMPONENTS, remap, SUBC_COPY);
    p.inc(CE_OFFSET_IN_UPPER,
          {uint32_t(s.va >> 32), uint32_t(s.va), uint32_t(d.va >> 32), uint32_t(d.va), s.pitch, d.pitch, w, h},
          SUBC_COPY);
    constexpr uint32_t kGobHeightFermi8 = 1u << 12, kSrcPitch = 1u << 7, kDstPitch = 1u << 8;
    uint32_t launch = (kLaunchPitch & ~(kSrcPitch | kDstPitch)) | 1u << 10;  // REMAP_ENABLE
    if (s.blockLinear)
        p.inc(CE_SET_SRC_BLOCK_SIZE,
              {uint32_t(s.blockHeight) << 4 | uint32_t(s.blockDepth) << 8 | kGobHeightFermi8, s.width, s.height,
               s.depth, s.layer, s.y << 16 | s.x},
              SUBC_COPY);
    else
        launch |= kSrcPitch;
    if (d.blockLinear)
        p.inc(CE_SET_DST_BLOCK_SIZE,
              {uint32_t(d.blockHeight) << 4 | uint32_t(d.blockDepth) << 8 | kGobHeightFermi8, d.width, d.height,
               d.depth, d.layer, d.y << 16 | d.x},
              SUBC_COPY);
    else
        launch |= kDstPitch;
    if (ordered)
        launch = (launch & ~3u) | 2u;
    p.m(CE_LAUNCH_DMA, launch, SUBC_COPY);
}

// `w` x `h` texels of `bpp` bytes, each in min(bpp, 4)-byte components
void copySides(Push &p, const CopySide &s, const CopySide &d, uint32_t w, uint32_t h, uint32_t bpp,
               Device::Stats &stats)
{
    copyRemapped(p, s, d, w, h, texelRemap(bpp), bpp, stats);
}

// `lines` rows of `width` texels of `bpp` bytes, pitch to pitch
void copyRows(Push &p, uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width, uint32_t lines,
              uint32_t bpp, Device::Stats &stats)
{
    copySides(p, {src, srcPitch}, {dst, dstPitch}, width, lines, bpp, stats);
}
}  // namespace

static void targetWords(Push &p, uint32_t m, const Texture &t)
{
    ColorTargetDesc d;
    d.va = t.va;
    d.pitch = t.pitch;
    d.height = sampleRows(t);
    d.bpp = t.bpp;
    d.rtFormat = formatInfo(t.format)->rt;
    d.blockLinear = t.blockLinear;
    d.blockHeight = t.blockHeight;
    d.layerBytes = t.layerBytes;
    uint32_t w[kColorTargetWords];
    colorTargetWords(d, w);
    p.incArray(m, w, kColorTargetWords);
}

uint32_t Device::levelTic(Texture &t, uint32_t level)
{
    const FormatInfo *f = formatInfo(t.format);
    if (!t.chain || !f || level >= t.chain->layout.levels)
        return 0;
    std::lock_guard<std::mutex> l(lock_);
    uint32_t &tic = t.chain->levelTic[level];
    if (!tic && (tic = allocTic())) {
        uint32_t w[8];
        fx::mipChainTic(t.chain->layout, t.chain->va, f->tic0, f->srgb, level, 1, w);
        memcpy(tic_.cpu + 32 * tic, w, 32);  // visible at the next submission's header invalidate
    }
    return tic;
}

static bool chainLevelView(const Texture &t, uint32_t level, uint32_t rtFormat, Texture &out)
{
    ColorTargetDesc d;
    if (!t.chain || !chainLevelDesc(t.chain->layout, t.chain->va, level, rtFormat, d))
        return false;
    out = Texture();
    out.va = d.va;
    out.width = t.chain->layout.level[level].width;
    out.height = d.height;
    out.format = t.format;
    out.pitch = d.pitch;
    out.bpp = t.bpp;
    out.blockLinear = true;
    out.blockHeight = d.blockHeight;
    out.layerBytes = d.layerBytes;
    return true;
}

bool chainLevelTarget(const Texture &t, uint32_t level, Texture &out)
{
    const FormatInfo *f = formatInfo(t.format);
    if (!f || !f->rt || !chainLevelView(t, level, f->rt, out))
        return false;
    out.renderable = true;
    return true;
}

bool chainLevelImage(const Texture &t, uint32_t level, Texture &out)
{
    return chainLevelView(t, level, ~0u, out);
}

void RenderEncoder::setDepthBias(float constant, float slope, float clamp)
{
    depthBias_[0] = constant;
    depthBias_[1] = slope;
    depthBias_[2] = std::isfinite(clamp) ? clamp : 0.0f;
}

// The rasterizer state of a draw of `primitive` (MTLPrimitiveType): the encoder's, with m2v's depth bias for its topology
// (metal::state::depth_bias): on, of Metal's factors, for triangles; off and zero for points and lines, and zero where
// both factors are
RenderEncoder::Raster RenderEncoder::rasterFor(uint32_t primitive) const
{
    Raster r = raster_;
    r.bias = primitive == 3 || primitive == 4;
    if (r.bias && (depthBias_[0] != 0 || depthBias_[1] != 0)) {
        r.biasConstant = fbits(depthBias_[0]);
        r.biasSlope = fbits(depthBias_[1]);
        r.biasClamp = fbits(depthBias_[2]);
    }
    return r;
}

// NVK's nvk_flush_rs_state for the state m2v makes of Metal's (render_state.rs `fixed`, `vk_depth_bias`): culling on and
// the face culled, the front face, the polygon mode of both faces, the polygon offset and the depth bias factors (in the
// depth format's units: the pack's SET_DEPTH_BIAS_CONTROL); a register already holding its value is not written
void RenderEncoder::writeRaster(Push &p, const Raster &want)
{
    if (want.bias != rasterHeld_.bias) {
        p.m(SET_POLY_OFFSET_POINT, want.bias);
        p.m(SET_POLY_OFFSET_LINE, want.bias);
        p.m(SET_POLY_OFFSET_FILL, want.bias);
        rasterHeld_.bias = want.bias;
    }
    if (want.biasConstant != rasterHeld_.biasConstant || want.biasSlope != rasterHeld_.biasSlope ||
        want.biasClamp != rasterHeld_.biasClamp) {
        p.m(SET_DEPTH_BIAS, want.biasConstant);
        p.m(SET_SLOPE_SCALE_DEPTH_BIAS, want.biasSlope);
        p.m(SET_DEPTH_BIAS_CLAMP, want.biasClamp);
        rasterHeld_.biasConstant = want.biasConstant;
        rasterHeld_.biasSlope = want.biasSlope;
        rasterHeld_.biasClamp = want.biasClamp;
    }
    if (want.cull != rasterHeld_.cull) {
        p.m(OGL_SET_CULL, want.cull != 0);
        if (want.cull)
            p.m(OGL_SET_CULL_FACE, want.cull == 1 ? 0x404 : 0x405);  // FRONT, BACK
        rasterHeld_.cull = want.cull;
    }
    if (want.winding != rasterHeld_.winding) {
        p.m(OGL_SET_FRONT_FACE, want.winding ? 0x901 : 0x900);  // CCW, CW
        rasterHeld_.winding = want.winding;
    }
    if (want.fill != rasterHeld_.fill) {
        const uint32_t mode = want.fill ? 0x1b01 : 0x1b02;  // LINE, FILL
        p.m(SET_FRONT_POLYGON_MODE, mode);
        p.m(SET_BACK_POLYGON_MODE, mode);
        rasterHeld_.fill = want.fill;
    }
}

void RenderEncoder::end()
{
    Device &d = *dev_;
    if (p_.room() < 64 + kSampleWords)
        reserve();
    writeRaster(p_, Raster());
    // the draw parameters the next encoder starts from: the root table's and the base registers, 0
    if (first_ || baseInstance_) {
        const uint32_t zero[2] = {};
        p_.m(LOAD_CB_OFFSET, 0);
        p_.nonInc(LOAD_CB, zero, 2);
        p_.m(GLOBAL_BASE_INSTANCE_INDEX, 0);
    }
    if (baseVertex_) {
        p_.m(GLOBAL_BASE_VERTEX_INDEX, 0);
        p_.m(VERTEX_ID_BASE, 0);
    }
    if (samples_ > 1)
        sampleWords(p_, 1, d.root_.va);
    if (drew_)
        tiledCacheDrain(p_, d.k_.info());
    checkRoom(p_, "a pass body");
    if (p_.at > bodyStart_)
        bodies_.push_back({bodyStart_, p_.at});
    d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - d.push_.bo.cpu);  // give back the unused reservation
    // pass prologue: the full barrier m2v puts between passes, the targets, then the clears
    const std::vector<Clear> cl = clears();
    const std::vector<uint32_t> &restore = extrasAtBegin_ ? extrasAtBegin_->restore : std::vector<uint32_t>();
    const uint32_t room = 160 + 8 * 32 + uint32_t(cl.size()) * 48 + kZetaTargetWords + kZetaClearWords + 16 +
                          kSampleWords + uint32_t(restore.size());
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, room * 4, 256, va));
    Push p{start, start + room};
    p.raw(restore.data(), uint32_t(restore.size()));
    p.m(WAIT_FOR_IDLE, 0);
    p.m(INVALIDATE_SHADER_CACHES, kBetweenPasses);
    p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    // ahead of the clears: a clear reaches every sample of its rectangle's pixels through it
    if (samples_ > 1)
        sampleWords(p, samples_, d.root_.va);
    // shadow targets' pool images: their texels over the render area first, as NVK's tiled shadow loads them (a load that
    // keeps them: Load, and a DontCare the clears leave; a Clear clears it all), then the barrier to the pass
    bool copied = false;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (const Texture *o = shadowOf_[j]) {
            if (pass_.color[j].load == ColorAttachment::Clear)
                continue;
            copySides(p, sideOf(*o, 0, 0, 0), sideOf(*pass_.color[j].texture, 0, 0, 0), areaW_, areaH_, o->bpp,
                      d.stats);
            copied = true;
        }
    if (copied) {
        p.m(WAIT_FOR_IDLE, 0);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    }
    // the clears m2v makes by passes of their own first (Clear loads reaching past the render area): each its attachment alone
    // as target 0, over its reach, then the barrier between passes
    bool own = false;
    for (const Clear &c : cl) {
        if (!c.own)
            continue;
        const Texture *t = pass_.color[c.target].texture;
        targetWords(p, COLOR_TARGET, *t);
        p.m(COLOR_COMPRESSION, t->comptags);
        uint32_t map = 0;
        for (uint32_t k = 0; k < 8; k++)
            map |= k << (4 + 3 * k);
        p.m(CT_SELECT, map | 1);
        p.inc(SURFACE_CLIP_H, {c.x1 << 16, c.y1 << 16});
        p.m(ZT_SELECT, 0);
        uint32_t v[4] = {};
        clearWords(pass_.color[c.target].texture->format, c.value, v,
                   !called_ && pass_.color[c.target].texture->compressed);
        d.countZbc(*t, !c.x0 && !c.y0 && c.x1 >= t->width && c.y1 >= t->height, zbcBootColor(t->format, v));
        p.inc(COLOR_CLEAR_VALUE, {v[0], v[1], v[2], v[3]});
        p.inc(CLEAR_RECT_H, {c.x1 << 16 | c.x0, c.y1 << 16 | c.y0});
        p.m(CLEAR_SURFACE_CONTROL, 0x10);  // USE_CLEAR_RECT
        p.m(RENDER_ENABLE_OVERRIDE, 1);
        p.m(CLEAR_SURFACE, 0x3c);
        p.m(RENDER_ENABLE_OVERRIDE, 0);
        own = true;
    }
    if (own) {
        p.m(WAIT_FOR_IDLE, 0);
        p.m(INVALIDATE_SHADER_CACHES, kBetweenPasses);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    }
    uint32_t ct = 0, count = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (pass_.color[j].texture)
            count = j + 1;
    for (uint32_t j = 0; j < count; j++) {
        if (const Texture *t = pass_.color[j].texture) {
            targetWords(p, COLOR_TARGET + 0x40 * j, *t);
            p.m(COLOR_COMPRESSION + 4 * j, t->comptags);
        } else {
            uint32_t w[kColorTargetWords];
            disabledTargetWords(w);
            p.incArray(COLOR_TARGET + 0x40 * j, w, kColorTargetWords);
            p.m(COLOR_COMPRESSION + 4 * j, 1);
        }
    }
    // targets map to themselves (NVK writes all eight slots of the map; the hardware reads the first `count`)
    for (uint32_t k = 0; k < 8; k++)
        ct |= k << (4 + 3 * k);
    p.m(CT_SELECT, ct | count);
    p.inc(SURFACE_CLIP_H, {areaW_ << 16, areaH_ << 16});  // the render area (nvk_CmdBeginRendering)
    const DepthStencilAttachment &zs = pass_.depthStencil;
    zetaTargetWords(p, zs.texture);
    // NVK clears depth and stencil with the first color the pass clears over its render area, else on their own
    uint32_t zsBits = zs.texture ? zetaClearBits(zs.clearDepth && zetaHasDepth(zs.texture->format),
                                                 zs.clearStencil && zetaHasStencil(zs.texture->format))
                                 : 0;
    zetaClearValues(p, zsBits & 1, zs.depth, zsBits & 2, zs.stencil);
    if (zsBits & 1)
        d.countZbc(*zs.texture, areaW_ >= zs.texture->width && areaH_ >= zs.texture->height, zbcBootDepth(zs.depth));
    for (const Clear &c : cl) {
        if (c.own)
            continue;
        uint32_t v[4] = {};
        const Texture *t = pass_.color[c.target].texture;
        clearWords(t->format, c.value, v, !called_ && t->compressed);
        d.countZbc(*t, !c.x0 && !c.y0 && c.x1 >= t->width && c.y1 >= t->height, zbcBootColor(t->format, v));
        const bool area = !c.x0 && !c.y0 && c.x1 == areaW_ && c.y1 == areaH_;
        p.inc(COLOR_CLEAR_VALUE, {v[0], v[1], v[2], v[3]});
        p.inc(CLEAR_RECT_H, {c.x1 << 16 | c.x0, c.y1 << 16 | c.y0});
        p.m(CLEAR_SURFACE_CONTROL, 0x10);  // USE_CLEAR_RECT
        p.m(RENDER_ENABLE_OVERRIDE, 1);
        p.m(CLEAR_SURFACE, 0x3c | c.target << 6 | (area ? zsBits : 0));
        p.m(RENDER_ENABLE_OVERRIDE, 0);
        zsBits = area ? 0 : zsBits;
    }
    if (zsBits) {
        p.inc(CLEAR_RECT_H, {areaW_ << 16, areaH_ << 16});
        p.m(CLEAR_SURFACE_CONTROL, 0x10);
        p.m(RENDER_ENABLE_OVERRIDE, 1);
        p.m(CLEAR_SURFACE, zsBits);
        p.m(RENDER_ENABLE_OVERRIDE, 0);
    }
    checkRoom(p, "a pass prologue");
    cb_->segment(start, p.at);
    d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p.at) - d.push_.bo.cpu);
    for (auto &b : bodies_) {
        cb_->segment(b.first, b.second);
        d.stats.pushBytes += uint64_t(b.second - b.first) * 4;
    }
    d.stats.pushBytes += uint64_t(p.at - start) * 4;
    bodies_.clear();
    // shadow targets the pass stores: their pool images back over what it rendered (the render area; a Clear's reach past it),
    // after the 3D engine is done with them
    bool stores = false;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        stores |= shadowOf_[j] && pass_.color[j].store;
    if (stores) {
        const uint32_t room2 = 8 + 8 * 32;
        uint32_t *at = reinterpret_cast<uint32_t *>(d.alloc(d.push_, room2 * 4, 256, va));
        Push q{at, at + room2};
        q.m(WAIT_FOR_IDLE, 0);
        for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
            const Texture *o = shadowOf_[j];
            if (!o || !pass_.color[j].store)
                continue;
            uint32_t w = areaW_, h = areaH_;
            for (const Clear &c : cl)
                if (c.target == j && pass_.color[j].load == ColorAttachment::Clear) {
                    w = std::max(w, c.x1);
                    h = std::max(h, c.y1);
                }
            copySides(q, sideOf(*pass_.color[j].texture, 0, 0, 0), sideOf(*o, 0, 0, 0), w, h, o->bpp, d.stats);
        }
        checkRoom(q, "a pass's shadow stores");
        cb_->segment(at, q.at);
        d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(q.at) - d.push_.bo.cpu);
        d.stats.pushBytes += uint64_t(q.at - at) * 4;
    }
}

// The pass's clears, before its draws: each Clear load, and each DontCare load m2v answers with a clear (metal2vulkan
// clear_reach.rs, from Metal's measured behaviour). The pass reaches no tile without a draw call, every tile with one and a
// stored Clear-loaded attachment, else the tiles (32 x 32 pixels) its drawing draws' scissors within their viewports touch. A
// DontCare-loaded memoryless color some draw reads (or that is stored) clears to zero where the pass reaches anything; a stored
// DontCare-loaded color some draw reads (blended into, written through a partial mask, fetched) clears to zero over what the
// pass reaches: the whole render area, or the reached tiles' runs along each row of tiles. Any other DontCare load loads.
std::vector<RenderEncoder::Clear> RenderEncoder::clears() const
{
    std::vector<Clear> out;

    enum {
        Nothing,
        Every,
        Draws
    } reach = Nothing;

    bool clearStored = false;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const ColorAttachment &a = pass_.color[j];
        clearStored |= a.texture && a.load == ColorAttachment::Clear && a.store && !memoryless_[j];
    }
    if (called_)
        reach = clearStored ? Every : drew_ ? Draws : Nothing;
    // the reached tiles where that is not every tile (none: every tile)
    constexpr uint32_t kTile = 32;
    const uint32_t perRow = (areaW_ + kTile - 1) / kTile, rows = (areaH_ + kTile - 1) / kTile;
    std::vector<bool> tiles;
    if (reach == Draws && !whole_ && perRow && rows) {
        bool every = false;
        for (const auto &r : rects_)
            every |= r[0] < kTile && r[1] < kTile && (r[2] + kTile - 1) / kTile >= perRow &&
                     (r[3] + kTile - 1) / kTile >= rows;
        if (!every) {
            tiles.assign(size_t(perRow) * rows, false);
            for (const auto &r : rects_)
                for (uint32_t ty = r[1] / kTile; ty < std::min(rows, (r[3] + kTile - 1) / kTile); ty++)
                    for (uint32_t tx = r[0] / kTile; tx < std::min(perRow, (r[2] + kTile - 1) / kTile); tx++)
                        tiles[size_t(ty) * perRow + tx] = true;
            if (std::find(tiles.begin(), tiles.end(), false) == tiles.end())
                tiles.clear();  // every tile after all
            else if (std::find(tiles.begin(), tiles.end(), true) == tiles.end())
                reach = Nothing;
        }
    }
    static const float zero[4] = {};
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const ColorAttachment &a = pass_.color[j];
        if (!a.texture)
            continue;
        const uint32_t w = areaW_, h = areaH_;  // NVK's clears cover the render area (nvk_CmdBeginRendering)
        if (a.load == ColorAttachment::Clear) {
            // Metal's Clear load of a stored attachment reaches past the render area: over it rounded up to whole 32-pixel
            // tiles, clipped to the attachment, in a pass with a draw call; the whole attachment in one without (m2v
            // clear_reach.rs `cleared`). Past the render area, m2v clears it by a pass of its own before this one
            // (ReachClears), which the pass then loads.
            uint32_t rw = w, rh = h;
            if (a.store && !memoryless_[j]) {
                rw = called_ ? std::min((w + kTile - 1) / kTile * kTile, a.texture->width) : a.texture->width;
                rh = called_ ? std::min((h + kTile - 1) / kTile * kTile, a.texture->height) : a.texture->height;
            }
            Clear c{j, 0, 0, rw, rh, a.clear};
            c.own = rw != w || rh != h;
            dev_->stats.clears++;
            if (!c.own && (covered_ >> j & 1)) {
                dev_->stats.clearsElided++;
                continue;
            }
            out.push_back(c);
            continue;
        }
        if (a.load != ColorAttachment::DontCare || reach == Nothing || (covered_ >> j & 1))
            continue;
        const bool read = readBits_ >> j & 1;
        if (memoryless_[j]) {
            if (read || a.store)
                out.push_back({j, 0, 0, w, h, zero});
            continue;
        }
        if (!a.store || !read)
            continue;
        if (tiles.empty()) {
            out.push_back({j, 0, 0, w, h, zero});
            continue;
        }
        for (uint32_t ty = 0; ty < rows; ty++)
            for (uint32_t tx = 0; tx < perRow;) {
                if (!tiles[size_t(ty) * perRow + tx]) {
                    tx++;
                    continue;
                }
                const uint32_t first = tx;
                while (tx < perRow && tiles[size_t(ty) * perRow + tx])
                    tx++;
                const uint32_t x0 = first * kTile, y0 = ty * kTile, x1 = std::min(tx * kTile, w),
                               y1 = std::min((ty + 1) * kTile, h);
                if (x0 < x1 && y0 < y1)
                    out.push_back({j, x0, y0, x1, y1, zero});
            }
    }
    return out;
}

// ---- blit encoder ----

// NVK's copies of pitch memory: a byte copy (vkCmdCopyBuffer, nvk_cmd_copy_memory_ce: 128 KiB per launch, no remap) or a
// rectangle of pitch rows (vkCmdCopyImage of linear images, nouveau_copy_rect: remap of min(bpp, 4)-byte components, so the line
// length counts pixels). LAUNCH_DMA: DATA_TRANSFER_TYPE PIPELINED, FLUSH_ENABLE, SRC/DST_MEMORY_LAYOUT PITCH, MULTI_LINE_ENABLE.

void BlitEncoder::room(uint32_t words)
{
    if (p_.room() >= words + 8)
        return;
    checkRoom(p_, "a blit encoder");
    cb_->segment(start_, p_.at);  // a new reservation: the copies so far become a segment
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    p_ = Push{start_, start_ + (1 << 12)};
}

void BlitEncoder::copyBytes(uint64_t src, uint64_t dst, uint64_t bytes)
{
    for (uint64_t done = 0; done < bytes;) {
        room();
        const uint32_t n = uint32_t(std::min<uint64_t>(bytes - done, 1u << 17));
        p_.inc(CE_OFFSET_IN_UPPER,
               {uint32_t((src + done) >> 32), uint32_t(src + done), uint32_t((dst + done) >> 32), uint32_t(dst + done)},
               SUBC_COPY);
        p_.inc(CE_LINE_LENGTH_IN, {n, 1}, SUBC_COPY);
        p_.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);
        dev_->stats.ceOps++;
        dev_->stats.ceBytes += n;
        done += n;
    }
}

void BlitEncoder::fillBytes(uint64_t dst, uint64_t bytes, uint8_t value)
{
    // SET_REMAP_COMPONENTS: DST_X..W CONST_A (4), COMPONENT_SIZE ONE (0) or FOUR (3) bytes, one source and destination
    // component; LAUNCH_DMA: PIPELINED, FLUSH, pitch both sides, REMAP
    constexpr uint32_t kConstA = 0x4444, kRemap = 1u << 10, kMaxDim = 1u << 15;
    constexpr uint32_t kLaunch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | kRemap;
    auto run = [&](uint64_t at, uint32_t width, uint32_t height, uint32_t unit) {
        room();
        p_.inc(CE_OFFSET_IN_UPPER + 8, {uint32_t(at >> 32), uint32_t(at)}, SUBC_COPY);
        p_.inc(CE_LINE_LENGTH_IN, {width, height}, SUBC_COPY);
        p_.m(CE_LAUNCH_DMA, kLaunch | (height > 1 ? 1u << 9 : 0), SUBC_COPY);
        dev_->stats.ceOps++;
        dev_->stats.ceBytes += uint64_t(width) * height * unit;
    };
    auto bytesOf = [&](uint64_t at, uint64_t n) {
        if (!n)
            return;
        room();
        p_.m(CE_SET_REMAP_CONST_A, value, SUBC_COPY);
        p_.m(CE_SET_REMAP_COMPONENTS, kConstA, SUBC_COPY);
        run(at, uint32_t(n), 1, 1);
    };
    const uint64_t end = dst + bytes, first = std::min((dst + 3) & ~uint64_t(3), end),
                   last = std::max(end & ~uint64_t(3), first);
    bytesOf(dst, first - dst);
    if (last > first) {
        room();
        p_.m(CE_SET_REMAP_CONST_A, uint32_t(value) * 0x01010101u, SUBC_COPY);
        p_.m(CE_SET_REMAP_COMPONENTS, kConstA | 3u << 16, SUBC_COPY);
        p_.inc(CE_PITCH_IN, {kMaxDim * 4, kMaxDim * 4}, SUBC_COPY);
        for (uint64_t at = first, left = last - first; left >= 4;) {
            const uint64_t words = left / 4;
            const uint32_t width = uint32_t(std::min<uint64_t>(words, kMaxDim)),
                           height = words >= kMaxDim ? uint32_t(std::min<uint64_t>(words / kMaxDim, kMaxDim)) : 1;
            run(at, width, height, 4);
            at += uint64_t(width) * height * 4;
            left -= uint64_t(width) * height * 4;
        }
    }
    bytesOf(last, end - last);
}

void BlitEncoder::copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width,
                           uint32_t lines, uint32_t bpp)
{
    if (!width || !lines || !bpp)
        return;
    room();
    copyRows(p_, src, srcPitch, dst, dstPitch, width, lines, bpp, dev_->stats);
}

void BlitEncoder::copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                             uint32_t dy, uint32_t dz, uint32_t w, uint32_t h)
{
    room(32);
    copySides(p_, sideOf(src, sx, sy, sz), sideOf(dst, dx, dy, dz), w, h, src.bpp, dev_->stats);
}

void BlitEncoder::copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, uint32_t h, bool toTexture, uint8_t aspect)
{
    room(48);
    const CopySide rows{va, pitch}, texels = sideOf(t, x, y, z);
    const CopySide &from = toTexture ? rows : texels, &to = toTexture ? texels : rows;
    if (t.zeta && t.format == kZetaFormat && aspect == kAspectDepth) {
        // the texel's first word: depth
        copyRemapped(p_, from, to, w, h,
                     toTexture ? remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 1, 2)
                               : remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 4, 2, 1),
                     4, dev_->stats);
        return;
    }
    if (t.zeta && t.format == kZetaFormat && aspect == kAspectStencil && t.stencilTemp) {
        // its fifth byte, stencil: as the third of its two-byte components to or from the temp's two bytes a texel, and those
        // two bytes' first to or from the buffer's byte
        const Texture &s = *t.stencilTemp;
        const CopySide temp{s.va + uint64_t(y) * s.pitch + 2ull * x, s.pitch};
        if (toTexture) {
            copyRemapped(p_, rows, temp, w, h, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 1, 2), 1,
                         dev_->stats);
            copyRemapped(p_, temp, texels, w, h, remapOf(kRemapNoWrite, kRemapNoWrite, 0, kRemapNoWrite, 2, 1, 4), 2,
                         dev_->stats, true);
        } else {
            copyRemapped(p_, texels, temp, w, h, remapOf(2, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 2, 4, 1), 2,
                         dev_->stats);
            copyRemapped(p_, temp, rows, w, h, remapOf(0, kRemapNoWrite, kRemapNoWrite, kRemapNoWrite, 1, 2, 1), 1,
                         dev_->stats, true);
        }
        return;
    }
    if (xrForm(t.format) == XrForm::Planes) {
        // four-byte components: the texel's color word to the buffer's word, or into the texel with its alpha word NO_WRITE (6)
        constexpr uint32_t kOut = 3u << 16 | 1u << 20, kIn = 6u << 4 | 3u << 16 | 1u << 24;
        copyRemapped(p_, from, to, w, h, toTexture ? kIn : kOut, bufferTexelBytes(t.format, t.bpp), dev_->stats);
        return;
    }
    copySides(p_, from, to, w, h, t.bpp, dev_->stats);
}

// The codes Metal reads of RGB8_444_2P (pack/../m2v ycbcr_convert.rs: "Codes"): red Cr, green Y, blue Cb, each code / 255, alpha
// 1, an RGBA8Unorm texel of the codes, as m2v's image holds them. Remap per destination component: SRC_X..W 0..3, CONST_A 4,
// NO_WRITE 6 (clb0b5 SET_REMAP_COMPONENTS); one byte a component, four destination components.
void BlitEncoder::convertPlanar(const Texture &t)
{
    if (!t.planar || !t.width || !t.height)
        return;
    dev_->stats.ceOps += 2;
    dev_->stats.ceBytes +=
        uint64_t(t.width) * t.height * 4 * 2;  // (each launch writes the image's rows, through NO_WRITE)
    room();
    constexpr uint32_t A = 4, W = 6, one = 0 << 16, dst4 = 3u << 24;
    p_.m(CE_SET_REMAP_CONST_A, 0xff, SUBC_COPY);
    // the luma plane, one component a texel: green Y, alpha 255
    p_.m(CE_SET_REMAP_COMPONENTS, W | 0 << 4 | W << 8 | A << 12 | one | 0u << 20 | dst4, SUBC_COPY);
    p_.inc(CE_OFFSET_IN_UPPER,
           {uint32_t(t.planes.luma >> 32), uint32_t(t.planes.luma), uint32_t(t.va >> 32), uint32_t(t.va),
            t.planes.lumaPitch, t.pitch, t.width, t.height},
           SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kLaunchPitch | 1u << 10, SUBC_COPY);
    room();
    // the chroma plane, two components a texel (Cb, Cr): red Cr, blue Cb
    p_.m(CE_SET_REMAP_COMPONENTS, 1 | W << 4 | 0 << 8 | W << 12 | one | 1u << 20 | dst4, SUBC_COPY);
    p_.inc(CE_OFFSET_IN_UPPER,
           {uint32_t(t.planes.chroma >> 32), uint32_t(t.planes.chroma), uint32_t(t.va >> 32), uint32_t(t.va),
            t.planes.chromaPitch, t.pitch, t.width, t.height},
           SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kLaunchPitch | 1u << 10, SUBC_COPY);
}

void BlitEncoder::access(const void *read, const void *write)
{
    auto in = [](const void *const *names, uint8_t n, const void *name) {
        return name && std::find(names, names + n, name) != names + n;
    };
    if (in(written_, writes_, read) || in(written_, writes_, write) || in(read_, reads_, write) ||
        reads_ == kAccesses || writes_ == kAccesses) {
        barrier();
        reads_ = writes_ = 0;
    }
    if (read && !in(read_, reads_, read))
        read_[reads_++] = read;
    if (write && !in(written_, writes_, write))
        written_[writes_++] = write;
}

void BlitEncoder::barrier()
{
    room(8);
    p_.m(WAIT_FOR_IDLE, 0);
    p_.inc(CE_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    constexpr uint32_t kNonPipelined = 2, kFlush = 1u << 2, kPitchToPitch = 1u << 7 | 1u << 8, kRemap = 1u << 10;
    p_.m(CE_LAUNCH_DMA, kNonPipelined | kFlush | kPitchToPitch | kRemap, SUBC_COPY);
}

void BlitEncoder::generate(Texture &t)
{
    Texture::Chain *c = t.chain;
    if (!c || !c->fmt2d)  // (the facade sends those to m2v: PredMipmapsFormat)
        return;
    const uint32_t last = c->layout.levels - 1;
    room(uint32_t(fx::generateWords(c->layout, 0, last)));
    fx::emitGenerate(p_, c->layout, c->va, nullptr, c->fmt2d, 0, last);
    dev_->stats.chainGenerates++;
}

void BlitEncoder::end()
{
    checkRoom(p_, "a blit encoder");
    cb_->segment(start_, p_.at);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    dev_->stats.pushBytes += uint64_t(p_.at - start_) * 4;
}

}  // namespace raw
