// SPDX-License-Identifier: MIT
// The method names, constants and small helpers gpu.cpp and the units split from it share (see gpu.h).
// They have internal linkage and are inline: each unit that includes this compiles and inlines its own copy.
#pragma once
#include "blit_overlap.h"
#include "gpu.h"
#include "kit.h"
#include "multisample.h"
#include "rotated_pass.h"

#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace gm20b {

namespace {
enum : uint32_t {
    WAIT_FOR_IDLE = NVB197_WAIT_FOR_IDLE,
    INVALIDATE_SHADER_CACHES = NVB197_INVALIDATE_SHADER_CACHES,
    COLOR_TARGET = NVB197_SET_COLOR_TARGET_A(0),
    VIEWPORT_SCALE_X = NVB197_SET_VIEWPORT_SCALE_X(0),
    VIEWPORT_CLIP_H = NVB197_SET_VIEWPORT_CLIP_HORIZONTAL(0),
    VIEWPORT_CLIP_CONTROL = NVB197_SET_VIEWPORT_CLIP_CONTROL,
    CLEAR_RECT_H = NVB197_SET_CLEAR_RECT_HORIZONTAL,
    COLOR_CLEAR_VALUE = NVB197_SET_COLOR_CLEAR_VALUE(0),
    INVALIDATE_SHADER_CACHES_NO_WFI = NVB197_INVALIDATE_SHADER_CACHES_NO_WFI,
    PIXEL_SHADER_BARRIER = NVB197_PIXEL_SHADER_BARRIER,
    NO_OPERATION = NVB197_NO_OPERATION,
    PIXEL_SHADER_INTERLOCK_CONTROL = NVB197_SET_PIXEL_SHADER_INTERLOCK_CONTROL,
    PIPELINE_REGISTER_COUNT_VERTEX = NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_VERTEX),
    PIPELINE_REGISTER_COUNT_PIXEL = NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_PIXEL),
    PIPELINE_PROGRAM_TESS = NVB197_SET_PIPELINE_PROGRAM(NVB197_SET_PIPELINE_SHADER_TYPE_TESSELLATION_INIT),
    PIPELINE_REGISTER_COUNT_TESS =
        NVB197_SET_PIPELINE_REGISTER_COUNT(NVB197_SET_PIPELINE_SHADER_TYPE_TESSELLATION_INIT),
    kPipelineStageStride = NVB197_SET_PIPELINE_SHADER(1) - NVB197_SET_PIPELINE_SHADER(0),
    BINDLESS_TEXTURE = NVB197_SET_BINDLESS_TEXTURE,
    DEPTH_TEST = NVB197_SET_DEPTH_TEST,
    STENCIL_TEST = NVB197_SET_STENCIL_TEST,
    ANTI_ALIAS_ALPHA_CONTROL = NVB197_SET_ANTI_ALIAS_ALPHA_CONTROL,
    SET_ANTI_ALIAS = NVB197_SET_ANTI_ALIAS,
    ANTI_ALIAS_SAMPLE_POSITIONS = NVB197_SET_ANTI_ALIAS_SAMPLE_POSITIONS(0),
    HYBRID_ANTI_ALIAS_CONTROL = NVB197_SET_HYBRID_ANTI_ALIAS_CONTROL,
    // Not in NVIDIA's published class headers: the texture data cache invalidate that NVN (deko3d's ORDER_FRAGMENTS
    // barrier) and NVIDIA's Linux driver put after PIXEL_SHADER_BARRIER, which orders it after the earlier pixels. The
    // _NO_WFI form acts as the front end reads it, while those pixels may still be shading or in the ROP.
    INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED = SWITCH_NVB197_INVALIDATE_TEXTURE_DATA_CACHE_PIXEL_ORDERED,
    SCISSOR_ENABLE = NVB197_SET_SCISSOR_ENABLE(0),
    SURFACE_CLIP_H = NVB197_SET_SURFACE_CLIP_HORIZONTAL,
    CLEAR_SURFACE_CONTROL = NVB197_SET_CLEAR_SURFACE_CONTROL,
    CT_SELECT = NVB197_SET_CT_SELECT,
    INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI = NVB197_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI,
    INVALIDATE_SAMPLER_CACHE_NO_WFI = NVB197_INVALIDATE_SAMPLER_CACHE_NO_WFI,
    INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI = NVB197_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI,
    GLOBAL_BASE_INSTANCE_INDEX = NVB197_SET_GLOBAL_BASE_INSTANCE_INDEX,
    GLOBAL_BASE_VERTEX_INDEX = NVB197_SET_GLOBAL_BASE_VERTEX_INDEX,
    VERTEX_ID_BASE = NVB197_SET_VERTEX_ID_BASE,
    ZT_SELECT = NVB197_SET_ZT_SELECT,
    END = NVB197_END,
    BEGIN = NVB197_BEGIN,
    DA_PRIMITIVE_RESTART = NVB197_SET_DA_PRIMITIVE_RESTART,
    RENDER_ENABLE_OVERRIDE = NVB197_SET_RENDER_ENABLE_OVERRIDE,
    PRIMITIVE_TOPOLOGY = NVB197_SET_PRIMITIVE_TOPOLOGY,
    CLEAR_SURFACE = NVB197_CLEAR_SURFACE,
    BLEND_CONST_RED = NVB197_SET_BLEND_CONST_RED,
    COLOR_COMPRESSION = NVB197_SET_COLOR_COMPRESSION(0),
    CT_WRITE = NVB197_SET_CT_WRITE(0),
    VERTEX_STREAM_FORMAT = NVB197_SET_VERTEX_STREAM_A_FORMAT(0),
    kStreamMethods = NVB197_SET_VERTEX_STREAM_A_FORMAT(1) - NVB197_SET_VERTEX_STREAM_A_FORMAT(0),
    CB_SELECTOR_A = NVB197_SET_CONSTANT_BUFFER_SELECTOR_A,
    LOAD_CB_OFFSET = NVB197_LOAD_CONSTANT_BUFFER_OFFSET,
    LOAD_CB = NVB197_LOAD_CONSTANT_BUFFER(0),
    BIND_GROUP_CB = NVB197_BIND_GROUP_CONSTANT_BUFFER(0),
    INDEX_BUFFER_A = NVB197_SET_INDEX_BUFFER_A,
    INDEX_BUFFER_E = NVB197_SET_INDEX_BUFFER_E,
    DA_PRIMITIVE_RESTART_INDEX = NVB197_SET_DA_PRIMITIVE_RESTART_INDEX,
    CE_LAUNCH_DMA = NVB0B5_LAUNCH_DMA,
    CE_OFFSET_IN_UPPER = NVB0B5_OFFSET_IN_UPPER,
    CE_PITCH_IN = NVB0B5_PITCH_IN,
    CE_LINE_LENGTH_IN = NVB0B5_LINE_LENGTH_IN,
    CE_SET_REMAP_CONST_A = NVB0B5_SET_REMAP_CONST_A,
    CE_SET_REMAP_CONST_B = NVB0B5_SET_REMAP_CONST_B,
    CE_SET_REMAP_COMPONENTS = NVB0B5_SET_REMAP_COMPONENTS,
    CE_SET_DST_BLOCK_SIZE = NVB0B5_SET_DST_BLOCK_SIZE,  // then DST_WIDTH, HEIGHT, DEPTH, LAYER, ORIGIN
    CE_SET_SRC_BLOCK_SIZE = NVB0B5_SET_SRC_BLOCK_SIZE,  // then SRC_WIDTH, HEIGHT, DEPTH, LAYER, ORIGIN
};

constexpr uint32_t kRootBytes = 0x800, kUboChunk = 0x100;
// The bind groups of Pipeline::setSlot's programs (nvk_pipeline_bind_group): vertex, pixel, tessellation init, tessellation
constexpr uint8_t kSetGroups[4] = {0, 4, 1, 2};
constexpr uint32_t kTicCount = kTextureHeaders, kTscCount = 2048;
static_assert(kTicCount == kPackHeapElements && kTscCount <= kPackHeapElements, "a heap holds header i at element i");
constexpr uint32_t kEngines =
    TegraGM20BEngine3D | TegraGM20BEngineCompute | TegraGM20BEngineCopy | TegraGM20BEngine2D | TegraGM20BEngineM2MF;
// A client's first chunk of a kind, and its first heap part, as a share of the role's size: a widget's few surfaces and states
// fit, and jetsam limits of a few tens of MB leave nothing for a render server's steps.
constexpr uint64_t kClientFirstShare = 16, kClientHeapFirstShare = 4;
constexpr uint32_t kInvalidateInstruction = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, INSTRUCTION, TRUE),
                   kFlushData = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE),
                   kInvalidateData = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, DATA, TRUE),
                   kInvalidateConstant = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE);
// NVK's copies of pitch memory (nouveau_copy_rect)
constexpr uint32_t kLaunchPitch =
    NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, PIPELINED) | NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
    NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) | NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
    NVDEF(NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE);

#ifdef SWITCH_METAL_HARNESS_MARKS
// The host harnesses' builds only (test/build.sh): a push that ran past its reservation stops them where it was closed
inline void checkRoom(const Push &p, const char *what)
{
    if (p.at > p.end) {
        fprintf(stderr, "SwitchMetal: %s wrote %td words past its push reservation\n", what, p.at - p.end);
        abort();
    }
}
#else
inline void checkRoom(const Push &, const char *)
{}
#endif

inline uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// A pass's sample layout (samples.h): its registers and the root table's sample words, the root table left selected
constexpr uint32_t kSampleWords = 20;

inline void sampleWords(Push &p, uint32_t samples, uint64_t root, const SamplePositions &at = {},
                        const Rotation &turned = Rotation())
{
    SampleLayout l;
    sampleLayoutAt(samples, at, l);
    turnPositions(turned, l.positions);
    sampleLayoutMethods(p, l, kRootBytes, root);
}

// bits [lo, hi] of the 256-bit header
inline void setField(uint32_t *w, unsigned hi, unsigned lo, uint64_t v)
{
    for (unsigned b = lo; b <= hi; b++, v >>= 1) {
        uint32_t &word = w[b / 32];
        const uint32_t bit = 1u << (b % 32);
        word = (v & 1) ? (word | bit) : (word & ~bit);
    }
}

inline size_t pageSize()
{
    static const size_t page = size_t(getpagesize());
    return page;
}

// CPU writes to cached memory the GPU reads (an import), out to memory
inline void cleanToMemory(const void *at, uint64_t bytes)
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

// A pitch-linear color target: its address and pitch multiples of 128 (NVK renders a linear image only so, nvk_rendering_linear,
// "the render hardware gets grumpy" otherwise; m2v's NVK renders any other through a tiled shadow, which the driver leaves to
// m2v)
static inline bool linearTarget(const Texture &t)
{
    return !((t.va | t.pitch) & 127);
}

namespace {
using CopySide = CopySurface;

inline CopySide sideOf(const Texture &t, uint32_t x, uint32_t y, uint32_t z)
{
    return copySideOf(t, x, y, z);
}

// SET_REMAP_COMPONENTS of a texel copy: DST_X..W = SRC_X..W, COMPONENT_SIZE, NUM_SRC_COMPONENTS, NUM_DST_COMPONENTS
inline uint32_t texelRemap(uint32_t bpp)
{
    return copyTexelRemap(bpp);
}

// `w` x `h` texels by nouveau_copy_rect (kit.h): a remap of components (`bytes` of each texel moved). `ordered`: after
// every earlier copy has completed (one that reads what the copy before it wrote)
inline void copyRemapped(Push &p, const CopySide &s, const CopySide &d, uint32_t w, uint32_t h, uint32_t remap,
                         uint32_t bytes, [[maybe_unused]] Device::Stats &stats, bool ordered = false)
{
    if (!w || !h || !bytes)
        return;
    SWITCH_METAL_DIAGNOSTIC(stats.ceOps++);
    SWITCH_METAL_DIAGNOSTIC(stats.ceBytes += uint64_t(w) * bytes * h);
    emitCopy(p, s, d, w, h, remap, ordered);
}

// `w` x `h` texels of `bpp` bytes, each in min(bpp, 4)-byte components
inline void copySides(Push &p, const CopySide &s, const CopySide &d, uint32_t w, uint32_t h, uint32_t bpp,
                      Device::Stats &stats)
{
    copyRemapped(p, s, d, w, h, texelRemap(bpp), bpp, stats);
}
}  // namespace

}  // namespace gm20b
