// SPDX-License-Identifier: MIT
// See raw_gpu.h. Method numbers: NVIDIA's class headers as Mesa ships them (src/nouveau/headers/nvidia/classes, MIT). The texture
// header fill follows Mesa NIL's nvb097_fill_image_view_desc (pitch), the sampler fill Mesa NVK's nvk_sampler_get_header, the
// copies NVK's nouveau_copy_rect and nvk_cmd_copy_memory_ce (all MIT), restricted to what Core Animation uses. r7-raw wrote the
// first version (render encoder, pack replay) and checked it against NVK's stream; r8-rawca added the lifetimes, the pool, the
// imports, the copies and the abortable command buffer.
#include "raw_lock.h"
#include "../fx/fx_image.h"
#include "raw_nonfinite.h"
#include "raw_clear.h"
#include "raw_icb.h"
#include "raw_event_release.h"
#include "raw_attachment_lifetime.h"
#include "raw_attachment_target.h"
#include "raw_blend_kill.h"
#include "raw_dynamic_push.h"
#include "raw_keyline_probe.h"
#include "raw_glass_probe.h"
#include "raw_blend_color.h"
#include "raw_blend_reads.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_follow.h"
#include "raw_block_base.h"
#include "raw_gpu.h"
#include "TegraGM20BJobABI.h"
#include "raw_attachment_target.h"
#include "raw_layers.h"
#include "raw_kit.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h"
#include "raw_mme.h"
#include "raw_mip.h"
#include "../fx/fx_mip_route.h"
#include "raw_owned.h"
#include "raw_cbuf.h"
#include "raw_residual.h"
#include "raw_multisample.h"
#include "raw_rotated_pass.h"
#include "raw_draw_interlock.h"
#include "raw_ycbcr_store.h"
#include "raw_fragment_tests.h"
#include "raw_shader_tests.h"
#include "raw_shader_test_push.h"
#include "raw_slm.h"
#include "raw_srgb_table.h"
#include "raw_stamps.h"
#include "raw_storage.h"
#include "raw_surface_view.h"
#include "raw_native_surface.h"
#include "raw_target.h"
#include "raw_tiled_cache.h"
#include "raw_view.h"
#include "raw_xr_pass.h"
#include "raw_written_textures.h"
#include "TegraGM20BLayout.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <set>
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
constexpr size_t kDedicatedSpares = 2;
// The shader heap: the programs of the states a process uses, placed at first use (Device::place); the pack holds tens of MiB
// of programs, of which a process uses a few hundred. Its GPU VA is the whole program window: SET_PIPELINE_PROGRAM is a 32-bit
// offset from SET_PROGRAM_REGION, so every program lies in the 4 GiB after it (NVK's contiguous shader arena before Volta is
// that window too). Only VA is held. A program of a page or more runs in place from the pack's own pages (Device::placeInPlace:
// RenderBox's uber programs are 200-520 KiB each, a SwiftUI process's heap was MiBs of copies of them); a smaller one is
// copied into parts of host memory bound after the pack's region as programs are placed, with room after the last one for
// the GPU's instruction prefetch past a program's end. The render server binds kHeapAtOpen at its start: what Core
// Animation's states place in their first frames (17's journal: 1.13 MiB).
constexpr uint64_t kProgramWindow = 1ull << 32, kHeapStep = 256 << 10, kHeapSlack = kProgramPrefetchBytes,
                   kHeapAtOpen = 2 << 20;
// The window after the pack's region the copied programs may take; the store's mappings come after it
constexpr uint64_t kHeapCopiesWindow = 512ull << 20;
// A client's first chunk of a kind, and its first heap part, as a share of the role's size: a widget's few surfaces and states
// fit, and jetsam limits of a few tens of MB leave nothing for a render server's steps.
constexpr uint64_t kClientFirstShare = 16, kClientHeapFirstShare = 4;
constexpr uint32_t kInvalidateInstruction = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, INSTRUCTION, TRUE),
                   kFlushData = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE),
                   kInvalidateData = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, DATA, TRUE),
                   kInvalidateConstant = NVDEF(NVB197, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE);
// An instance of a draw: BEGIN with INSTANCE_ID SUBSEQUENT (two words), the first vertex or index and the count (three), END
constexpr uint32_t kInstanceWords = 6;
// a draw's shortcuts around its BEGIN/END (raw_draw_shortcuts.cpp): constant colour on and off, fill via triangle on and off
constexpr uint32_t kShortcutWords = 2 * (SWITCH_GM20B_CONSTANT_COLOR_WORDS + SWITCH_GM20B_FILL_VIA_TRIANGLE_WORDS);
// NVK's copies of pitch memory (nouveau_copy_rect)
constexpr uint32_t kLaunchPitch =
    NVDEF(NVB0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, PIPELINED) | NVDEF(NVB0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
    NVDEF(NVB0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) | NVDEF(NVB0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
    NVDEF(NVB0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE);

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

// The viewport's words (scale X Y Z, offset X Y Z as floats' bits, then each clip's x | width << 16) as the kit's state
switch_gm20b_viewport_state viewportState(const uint32_t w[8])
{
    switch_gm20b_viewport_state v = {};
    memcpy(v.scale, w, sizeof v.scale);
    memcpy(v.offset, w + 3, sizeof v.offset);
    v.clip_x = uint16_t(w[6]);
    v.clip_width = uint16_t(w[6] >> 16);
    v.clip_y = uint16_t(w[7]);
    v.clip_height = uint16_t(w[7] >> 16);
    return v;
}

constexpr uint32_t kRtLayerFromShader = NVDEF(NVB197, SET_RT_LAYER, CONTROL, GEOMETRY_SHADER_SELECTS_LAYER);

// A pass's sample layout (raw_samples.h): its registers and the root table's sample words, the root table left selected
constexpr uint32_t kSampleWords = 20;

void sampleWords(Push &p, uint32_t samples, uint64_t root, const SamplePositions &at = {},
                 const Rotation &turned = Rotation())
{
    SampleLayout l;
    sampleLayoutAt(samples, at, l);
    turnPositions(turned, l.positions);
    sampleLayoutMethods(p, l, kRootBytes, root);
}

// A turned pass's turn words into the root table (raw_rotation.h kRootTurn), which its fragment programs' turned variants read
constexpr uint32_t kTurnWordsMethods = 16;

void turnWordsInto(Push &p, uint64_t root, const Rotation &r)
{
    uint32_t w[kTurnWords];
    turnWords(r, w);
    p.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A, {kRootBytes, uint32_t(root >> 32), uint32_t(root)});
    p.m(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, kRootTurn);
    p.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), w, kTurnWords);
}

// Texture header word 0 per MTLPixelFormat as NVK writes it (pack/mkformats.py over the twin, 11-04), and the color target format
// NVK writes for a target of it (raw/test/targetcheck.py over the probe's PROBE_TARGETS run; 0 where m2v's NVK renders none).
// The integer formats' are NIL's rows (nil_formats.csv: the class's RU*/RS* target, UINT or SINT components, alpha ONE_INT).
const FormatInfo kFormats[] = {
    {1, 0xf7, 0x2002491d, 1, false},     // A8Unorm
    {10, 0xf3, 0x7012491d, 1, false},    // R8Unorm
    {11, 0, 0x7012491d, 1, true},        // R8Unorm_sRGB
    {13, 0xf6, 0x6014921d, 1, false},    // R8Uint (NIL's R8_UINT: UINT components, alpha ONE_INT)
    {14, 0xf5, 0x60136d9d, 1, false},    // R8Sint
    {20, 0xee, 0x7012491b, 2, false},    // R16Unorm
    {23, 0xf1, 0x6014921b, 2, false},    // R16Uint
    {24, 0xf0, 0x60136d9b, 2, false},    // R16Sint
    {25, 0xf2, 0x7017ff9b, 2, false},    // R16Float
    {30, 0xea, 0x70d24918, 2, false},    // RG8Unorm
    {33, 0xed, 0x60d49218, 2, false},    // RG8Uint
    {34, 0xec, 0x60d36d98, 2, false},    // RG8Sint
    {40, 0xe8, 0x74e24915, 2, false},    // B5G6R5Unorm
    {41, 0, 0x272a4913, 2, false},       // A1BGR5Unorm
    {42, 0, 0x272a4912, 2, false},       // ABGR4Unorm
    {43, 0xe9, 0x54e24914, 2, false},    // BGR5A1Unorm
    {53, 0xe4, 0x6014920f, 4, false},    // R32Uint
    {54, 0xe3, 0x60136d8f, 4, false},    // R32Sint
    {55, 0xe5, 0x7017ff8f, 4, false},    // R32Float
    {60, 0xda, 0x70d2490c, 4, false},    // RG16Unorm
    {63, 0xdd, 0x60d4920c, 4, false},    // RG16Uint
    {64, 0xdc, 0x60d36d8c, 4, false},    // RG16Sint
    {65, 0xde, 0x70d7ff8c, 4, false},    // RG16Float
    {70, 0xd5, 0x58d24908, 4, false},    // RGBA8Unorm (rendered: a 4:2:0 code texture's image)
    {71, 0xd6, 0x58d24908, 4, true},     // RGBA8Unorm_sRGB
    {73, 0xd9, 0x58d49208, 4, false},    // RGBA8Uint
    {74, 0xd8, 0x58d36d88, 4, false},    // RGBA8Sint
    {80, 0xcf, 0x54e24908, 4, false},    // BGRA8Unorm
    {81, 0xd0, 0x54e24908, 4, true},     // BGRA8Unorm_sRGB
    {90, 0xd1, 0x58d24909, 4, false},    // RGB10A2Unorm (rendered: a 10-bit code texture's image)
    {91, 0xd2, 0x58d49209, 4, false},    // RGB10A2Uint
    {92, 0xe0, 0x78d7ffa1, 4, false},    // RG11B10Float
    {93, 0, 0x78d7ffa0, 4, false},       // RGB9E5Float
    {94, 0xdf, 0x54e24909, 4, false},    // BGR10A2Unorm
    {103, 0xcd, 0x60d49204, 8, false},   // RG32Uint
    {104, 0xcc, 0x60d36d84, 8, false},   // RG32Sint
    {105, 0xcb, 0x70d7ff84, 8, false},   // RG32Float
    {110, 0xc6, 0x58d24903, 8, false},   // RGBA16Unorm
    {113, 0xc9, 0x58d49203, 8, false},   // RGBA16Uint
    {114, 0xc8, 0x58d36d83, 8, false},   // RGBA16Sint
    {115, 0xca, 0x58d7ff83, 8, false},   // RGBA16Float (rendered: a 4:2:0 texture's converted image)
    {123, 0xc2, 0x58d49201, 16, false},  // RGBA32Uint
    {124, 0xc1, 0x58d36d81, 16, false},  // RGBA32Sint
    {125, 0xc0, 0x58d7ff81, 16, false},  // RGBA32Float (rendered: an sRGB 4:2:0 texture's image)
    // block-compressed (sampled only: Metal's copies move whole blocks), word 0 NIL's row of NVK's format; ETC2 and EAC
    // NIL's MaxwellA rows, which the texture unit decodes as m2v's NVK twin does
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
    {170, 0, 0x70124919, 8, false, 4, 4},     // EAC_R11Unorm: ETC2_R11_UNORM
    {172, 0, 0x70112499, 8, false, 4, 4},     // EAC_R11Snorm: ETC2_R11_SNORM
    {174, 0, 0x70d2491a, 16, false, 4, 4},    // EAC_RG11Unorm: ETC2_RG11_UNORM
    {176, 0, 0x70d1249a, 16, false, 4, 4},    // EAC_RG11Snorm: ETC2_RG11_SNORM
    {178, 0, 0x58d2490b, 16, false, 4, 4},    // EAC_RGBA8: ETC2_RGBA8
    {179, 0, 0x58d2490b, 16, true, 4, 4},     // EAC_RGBA8_sRGB: ETC2_SRGBA8
    {180, 0, 0x78d24906, 8, false, 4, 4},     // ETC2_RGB8: ETC2_RGB8
    {181, 0, 0x78d24906, 8, true, 4, 4},      // ETC2_RGB8_sRGB: ETC2_SRGB8
    {182, 0, 0x58d2490a, 8, false, 4, 4},     // ETC2_RGB8A1: ETC2_RGB8A1
    {183, 0, 0x58d2490a, 8, true, 4, 4},      // ETC2_RGB8A1_sRGB: ETC2_SRGB8A1
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
    return icb::topology(mtl);
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
            if (it->submitted && done(it->seqno)) {
                it = r.inflight.erase(it);
                continue;
            }
            if (it->begin < at + bytes && at < it->end && it->submitted) {
                RAW_DIAGNOSTIC(stats.ringWaits++);
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
        if (!s.submitted && s.begin < at + bytes && at < s.end)
            return true;
    return false;
}

// The open command buffer is larger than the ring: it goes on in one twice the size (or its allocation's), from its start; the
// old one is freed once the work that reads it, the open command buffer's included, completed. false: no memory for it
bool Device::outgrow(Ring &r, uint32_t bytes, uint64_t &at)
{
    Bo larger;
    if (!k_.allocControl(std::max<uint64_t>(r.bo.bytes * 2, uint64_t(bytes) * 2), larger, ControlUse::Ring))
        return false;
    set_.add(k_, larger, &r == &upload_ ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
    RAW_DIAGNOSTIC(stats.ringsOutgrown++);
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
        grownOut_.push_back({b, seqno, channelGeneration()});
    r.outgrown.clear();
    while (!r.inflight.empty() && r.inflight.front().submitted && done(r.inflight.front().seqno))
        r.inflight.pop_front();
    for (auto &s : r.inflight)
        if (!s.submitted) {
            s.seqno = seqno;
            s.submitted = true;
        }
    if (r.head > r.since)
        r.inflight.push_back({r.since, r.head, seqno, true});
    r.since = r.head;
}

// ---- device: open ----

Device::~Device()
{
    transfers_.close(k_, lock_);
    cb_.releaseGeometry();
    // the render server keeps its device until it exits; a host test's device: the kernel frees what the connection held
    {
        std::lock_guard<raw::Mutex> l(lock_);
        stopping_ = true;
    }
    grow_.notify_all();
    if (grower_.joinable())
        grower_.join();
    compStop();
    if (channel_)
        k_.closeChannel(channel_);
    reapNativeSurfaces(true);
    for (Backing *b : trimmedBackings_)
        delete b;
}

uint32_t Device::contextWords() const
{
    return uint32_t(pack_.bytes(PackInit) / sizeof(PackInitMethod)) * 2 + 4096 + kDepthStencilWords +
           mme::kContextWords;
}

// The channel's context: NVK's own init methods with our addresses, then every register Core Animation's states leave the
// same. A channel the kernel kills takes its context with it (reopen).
void Device::emitContext(Push &p)
{
    const uint32_t initCount = uint32_t(pack_.bytes(PackInit) / sizeof(PackInitMethod));
    const PackInitMethod *im = pack_.section<PackInitMethod>(PackInit);
    const InitAddresses here{heap_.va, zero_.va, root_.va, tic_.va, tsc_.va, kTicCount - 1, kTscCount - 1, tickets_.va};
    for (uint32_t i = 0; i < initCount; i++) {
        const uint32_t d = relocated(im[i], here);
        p.m(im[i].mthd, d, im[i].subc);
    }
    const uint32_t *base = pack_.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack_.bytes(PackBaseline) / 8; i++)
        if (!ownedMethod(base[2 * i]))
            p.m(base[2 * i], base[2 * i + 1]);
    mme::install(p, root_.va);
    tiledCacheInit(p, k_.info());
    // the depth/stencil registers (the runtime's, as NVK leaves them for Metal's default state in a pass without the attachment)
    {
        DepthStencilRegs unknown;
        for (uint32_t &v : unknown.v)
            v = ~0u;
        emitDepthStencil(p, depthStencilRegs(DepthStencilDesc(), 0, 0, false, false), unknown);
        ds_ = unknown;
    }
    heldSegment_.lose();
    // root table selected for LOAD_CONSTANT_BUFFER from here on (draws reselect it after binding set 0 as a cbuf); NVK's init
    // above zeroed it, so one sample's layout goes in now, as every pass but a multisampled one leaves it
    sampleWords(p, 1, root_.va);
}

bool Device::open(const char *packPath, uint64_t stack, std::string &err, Role role)
{
    role_ = role;
    sizes_ = kSizes[role == Role::RenderServer ? 0 : 1];
    k_.observeFrees(set_);
#if SWITCH_RAW_DIAGNOSTICS
    if (role == Role::RenderServer)
        glassMem = std::make_unique<GlassMem>();
#endif
    if (!pack_.load(packPath, stack, err))
        return false;
    if (!k_.open(err))
        return false;
    if (k_.info().cls_eng3d != 0xb197 || k_.info().cls_copy != 0xb0b5) {
        err = "not a MAXWELL_B with MAXWELL_DMA_COPY_A";
        return false;
    }
    ZcullRegion probe;
    zcullOn_ = k_.zcullInfo(zcullInfo_) == KERN_SUCCESS && zcullRegion(zcullInfo_, 0, 0, 1, 1, probe);
    return init(err);
}

// Every process's: the header pools, the root table, the zero page, the interlock tickets and the shader heap's reservation,
// which textures, samplers and states need before any submission. The render server's channel, heap and first chunks too.
bool Device::init(std::string &err)
{
    if (!k_.allocControl(kTicCount * 32, tic_, ControlUse::TextureHeaders) ||
        !k_.allocControl(kTscCount * 32, tsc_, ControlUse::SamplerHeaders) ||
        !k_.allocControl(0x1000, root_, ControlUse::RootTable) ||
        !k_.allocControl(0x10000, zero_, ControlUse::ZeroPage) ||
        !k_.allocControl(0x1000, tickets_, ControlUse::InterlockTickets) ||
        !k_.allocControl(kColorTableBytes, colorTable_, ControlUse::ColorTable) ||
        !k_.allocControl(cbufBindBytes(kSrgbEncodeEntries * 2), srgbEncode_, ControlUse::ColorTable) ||
        !k_.allocControl(kStampBytes, stamps_, ControlUse::Stamps)) {
        err = k_.error();
        return false;
    }
    set_.add(k_, {{&tic_, TegraGM20BSurfaceRead},
                  {&tsc_, TegraGM20BSurfaceRead},
                  {&root_, TegraGM20BSurfaceWrite},
                  {&zero_, TegraGM20BSurfaceRead},
                  {&tickets_, TegraGM20BSurfaceWrite},
                  {&colorTable_, TegraGM20BSurfaceRead},
                  {&srgbEncode_, TegraGM20BSurfaceRead},
                  {&stamps_, TegraGM20BSurfaceWrite}});
#if SWITCH_RAW_DIAGNOSTICS
    static std::once_flag ticketsLogged;
    std::call_once(ticketsLogged, [this] {
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
               "raw: interlock tickets mapped %{public}s at 0x%llx",
               tickets_.gpuVolatile ? "volatile (GPU L2 bypassed)" : "cached (GPU L2)",
               (unsigned long long)tickets_.va);
    });
#endif
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
    static constexpr uint16_t kSrgbEncode[kSrgbEncodeEntries] = {
#include "../pack/srgb_encode.h"
    };
    memset(srgbEncode_.cpu, 0, srgbEncode_.bytes);
    memcpy(srgbEncode_.cpu, kSrgbEncode, sizeof kSrgbEncode);
    memset(stamps_.cpu, 0, stamps_.bytes);
#if SWITCH_RAW_DIAGNOSTICS
    if (role_ == Role::RenderServer && k_.allocControl(SpanMarks::kBytes, spanMarks_, ControlUse::Stamps)) {
        set_.add(k_, spanMarks_, TegraGM20BSurfaceWrite);
        spans_.open(spanMarks_.va, spanMarks_.cpu);
    }
#endif
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
        uint32_t neutral[8] = {0x26092, 0x291, 0xfff000, 0, 0, 0, 0, 0};
        memcpy(tsc_.cpu + 32 * nextTsc_, neutral, 32);
        neutralTsc_ = nextTsc_++;
        compareHeader(neutral, 0);  // m2v's neutral compare sampler: comparison Never
        memcpy(tsc_.cpu + 32 * nextTsc_, neutral, 32);
        neutralCompareTsc_ = nextTsc_++;
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
    segmentHeap_.setGrow([this](uint64_t bytes, SegmentChunk &chunk) {
        Bo bo;
        if (!k_.allocControl(bytes, bo, ControlUse::StateSegments))
            return false;
        set_.add(k_, bo, TegraGM20BSurfaceRead);
        chunk = {bo.cpu, bo.va, bo.bytes};
        return true;
    });
    {
        const PackHeader &h = *pack_.header();
        const uint64_t page = k_.info().page_size;
        uint64_t mapped = 0;
        for (const PackSection &section : h.section)
            mapped = std::max(mapped, (section.offset + section.bytes + page - 1) / page * page);
        packPages_.reset(h.section[PackHeap].offset, mapped, mapped, 0);
        heapCopiesAt_ = heapBound_ = heapPlaced_ = (packPages_.windowEnd() + kHeapStep - 1) & ~(kHeapStep - 1);
        heapStoresAt_ = heapCopiesAt_ + kHeapCopiesWindow;
    }
    if (role_ == Role::Client)
        return true;
    if (!ensureChannel()) {
        err = channelError_;
        return false;
    }
    // at the render server's start, not under its first frames: the heap's first part, the mip chains' first chunk and the
    // first pool chunk
    {
        std::lock_guard<raw::Mutex> l(lock_);
        heapGrow(heapCopiesAt_ + std::min<uint64_t>(pack_.bytes(PackHeap) + kHeapSlack, kHeapAtOpen));
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
    std::lock_guard<raw::Mutex> l(channelLock_);
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
    generations_.opened();
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
    if (!k_.allocControl(sizes_.push, push_.bo, ControlUse::Ring) ||
        !k_.allocControl(sizes_.upload, upload_.bo, ControlUse::Ring) || !k_.channel(kEngines, channel_, fence_))
        return undo(false);
    set_.add(k_, {{&push_.bo, TegraGM20BSurfaceRead}, {&upload_.bo, TegraGM20BSurfaceWrite}});
    const uint32_t words = contextWords() + uint32_t(fx::twodInitWords());
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, words * 4, 256, va));
    Push p{start, start + words};
    emitContext(p);
    fx::emitTwodInit(p);
    TegraGM20BAbiPush seg = {va, uint32_t(p.at - start), 0};
    uint32_t seq = 0;
    SurfaceList resources;
    SetView view;
    if (!job(view, channel_, &seg, 1, resources, JobVisibility::External, seq))
        return undo(true);
    lastSeqno_ = seq;
    retire(push_, seq);
    return true;
}

// lock_ held: program `prog`'s slot in the program window the first time a state uses it, its SET_PIPELINE_PROGRAM value and
// its shader data's GPU address (0: none). A slot of a page or more, or one whose pages are bound already, runs in place from
// the pack's pages; a smaller one is copied into the heap's host memory, since importing its page would wire a page for a
// few KiB. The prologue of the next submission committed after it invalidates the instruction caches too
// (CommandBuffer::commit), and every prologue the constant cache, before any draw reads it. False: the heap is full (that
// state stays m2v's).
bool Device::place(uint32_t prog, uint32_t &program, uint64_t &data)
{
    const PackProgram &p = pack_.section<PackProgram>(PackPrograms)[prog];
    if (placed_[prog] == ~0u) {
        uint64_t at = 0;
        const uint8_t *file = reinterpret_cast<const uint8_t *>(pack_.header());
        const uint64_t fileOffset = pack_.header()->section[PackHeap].offset + p.heapOffset;
        const bool inPlace = p.bytes >= k_.info().page_size || packPages_.bound(fileOffset, p.bytes + kHeapSlack);
        if (!(inPlace && placeInPlace(packPages_, file, fileOffset, p.bytes, at)) &&
            !heapPlace(pack_.section<uint8_t>(PackHeap) + p.heapOffset, p.bytes, at))
            return false;
        placed_[prog] = uint32_t(at);
        RAW_DIAGNOSTIC(stats.programsPlaced++);
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
    if (!k_.allocGpu(a.bytes, area))
        return false;
    if (slm_.bytes)
        slmOutgrown_.push_back(slm_);
    set_.add(k_, area, TegraGM20BSurfaceWrite, true);
    slm_ = area;
    slmVa_.store(area.va, std::memory_order_relaxed);
    slmPerWarp_.store(perWarp, std::memory_order_release);
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
    if (end > heapStoresAt_)
        return false;
    // a client's parts double from a quarter step, as its chunks do
    const uint64_t step = role_ == Role::Client
                              ? std::clamp(heapBound_ - heapCopiesAt_, kHeapStep / kClientHeapFirstShare, kHeapStep)
                              : kHeapStep;
    HeapPart part;
    part.offset = heapBound_;
    part.bytes = std::min(heapStoresAt_ - heapBound_, (end - heapBound_ + step - 1) & ~(step - 1));
    vm_address_t cpu = 0;
    if (vm_allocate(mach_task_self(), &cpu, part.bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return false;
    part.cpu = reinterpret_cast<uint8_t *>(cpu);
    if (!k_.importPages(part.cpu, part.bytes, part.bo, 0, true) ||
        !k_.bindImportAt(part.cpu, part.bo, heap_.va + part.offset, kPteKindPitch)) {
        vm_deallocate(mach_task_self(), cpu, part.bytes);
        return false;
    }
    set_.add(k_, part.bo, TegraGM20BSurfaceRead, true);
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
    if (size >= k_.info().page_size && slotLookup_ && placeFromStore(bytes, size, offset))
        return true;
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
    heapEpoch_.fetch_add(1, std::memory_order_release);
    return true;
}

// lock_ held: a slot of `size` bytes at `fileOffset` of the read-only mapping `file` run in place: the grains it and the
// instruction prefetch past its end lie in imported and bound at their window offsets where they are not yet, its window
// offset in `offset`. The kernel wires the pages read-only (BoImport's read-only retry; the GPU maps them read-only),
// cleans them at the import, and the bind's TLB and L2 invalidate makes the GPU's first walk read them; a slot in grains
// bound already is cleaned here. The imports are held as long as the device and are never a job's use or a set member: no
// job writes them, the CPU never changes a published slot, the kernel refuses a read-only import as a member, and a
// connection's close kills its channels before it unbinds and unwires anything (drainResources). False: not inside the
// file's region, or the kernel refused an import (the slot is then copied).
bool Device::placeInPlace(ProgramPages &pages, const uint8_t *file, uint64_t fileOffset, uint32_t size,
                          uint64_t &offset)
{
    std::vector<ProgramPages::Grain> grains;
    if (!pages.missing(fileOffset, uint64_t(size) + kHeapSlack, grains))
        return false;
    for (const ProgramPages::Grain &r : grains) {
        void *cpu = const_cast<uint8_t *>(file + r.offset);
        Bo bo;
        if (!k_.importPages(cpu, r.bytes, bo, 0, true)) {
            os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                         "raw: the kernel refused %llu KiB of program pages at file offset 0x%llx: copied instead",
                         (unsigned long long)(r.bytes >> 10), (unsigned long long)r.offset);
            return false;
        }
        if (!k_.bindImportAt(cpu, bo, heap_.va + pages.windowOffset(r.offset), kPteKindPitch)) {
            os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                         "raw: the program pages at file offset 0x%llx would not bind in the window: copied instead",
                         (unsigned long long)r.offset);
            return false;
        }
        inPlaceImports_.push_back(bo);
        pages.bind(r);
    }
    if (grains.empty())
        cleanToMemory(file + fileOffset, size);
    offset = pages.windowOffset(fileOffset);
    heapEpoch_.fetch_add(1, std::memory_order_release);
    return true;
}

// lock_ held: a slot the shader compiler's store holds (switch_compiler::Store::putProgramSlots: on pages of its own, zeros
// through the prefetch past its end) run in place from this process's mapping of the store. False: the store holds none of
// its bytes, or the window has no room for another mapping (the slot is then copied).
bool Device::placeFromStore(const uint8_t *slot, uint32_t size, uint64_t &offset)
{
    const uint8_t *mapping = nullptr;
    uint64_t bytes = 0;
    const uint8_t *at = slotLookup_(slot, size, mapping, bytes);
    if (!at)
        return false;
    StoreRegion *region = nullptr;
    for (StoreRegion &r : storeRegions_)
        if (r.mapping == mapping)
            region = &r;
    if (!region) {
        const uint64_t after = storeRegions_.empty() ? heapStoresAt_ : storeRegions_.back().pages.windowEnd();
        const uint64_t window = (after + kHeapStep - 1) & ~(kHeapStep - 1), page = k_.info().page_size;
        if (window + (bytes + page - 1) / page * page > heap_.bytes)
            return false;
        region = &storeRegions_.emplace_back(StoreRegion{mapping, {}});
        region->pages.reset(0, bytes, (bytes + page - 1) / page * page, window);
    }
    return placeInPlace(region->pages, mapping, uint64_t(at - mapping), size, offset);
}

void Device::runSlotsFrom(SlotLookup lookup)
{
    std::lock_guard<raw::Mutex> l(lock_);
    slotLookup_ = std::move(lookup);
}

// lock_ held: the CPU's view of window offset `at`: the pack's or a store's mapping, or the heap part holding it (null:
// none)
const uint8_t *Device::heapCpu(uint64_t at) const
{
    if (packPages_.holdsWindow(at))
        return reinterpret_cast<const uint8_t *>(pack_.header()) + packPages_.fileOffset(at);
    for (const StoreRegion &r : storeRegions_)
        if (r.pages.holdsWindow(at))
            return r.mapping + r.pages.fileOffset(at);
    const HeapPart *part = heapPartAt(at);
    return part ? part->cpu + (at - part->offset) : nullptr;
}

// The methods that bind program `prog`'s shader data at its slot of constant buffer group `group` (0 vertex, 4 pixel), leaving
// the root table selected; and the slot it reads set 0 at (0: none)
static uint8_t bindProgramCbufs(Push &w, const PackProgram &prog, uint32_t group, uint64_t data, uint64_t root,
                                bool exact)
{
    uint8_t setSlot = 0;
    for (uint8_t c = 1; c < prog.cbufs; c++) {
        if (prog.cbufType[c] == CbufSet)
            setSlot = c;
        if (prog.cbufType[c] != CbufShaderData)
            continue;
        w.inc(CB_SELECTOR_A, {cbufBindBytes(prog.dataBytes, exact), uint32_t(data >> 32), uint32_t(data)});
        w.m(BIND_GROUP_CB + 0x20 * group, 1 | uint32_t(c) << 4);
        w.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(root >> 32), uint32_t(root)});
    }
    return setSlot;
}

// The slot program `prog` reads set 1, a heap's arrays, at (0: none)
static uint8_t heapCbufSlot(const PackProgram &prog)
{
    for (uint8_t c = 1; c < prog.cbufs; c++)
        if (prog.cbufType[c] == kCbufHeapSet)
            return c;
    return 0;
}

// lock_ held: m2v's handle tables and roots, made at their first use (most processes name no object an argument buffer holds).
// A new binding's first submission invalidates the L2 for it (TegraGM20BTlb.h), after this fill.
bool Device::heapMemory()
{
    if (heapTables_.attached())
        return true;
    if (!k_.allocControl(HeapTables::tablesBytes(kTicCount, kTscCount), handleTables_, ControlUse::HandleTables))
        return false;
    if (!k_.allocControl(HeapTables::kRootsBytes, argumentRoots_, ControlUse::ArgumentRoots)) {
        k_.free(handleTables_);
        handleTables_ = {};
        return false;
    }
    heapTables_.attach({handleTables_.cpu, handleTables_.va, handleTables_.bytes},
                       {argumentRoots_.cpu, argumentRoots_.va, argumentRoots_.bytes}, kTicCount, kTscCount,
                       k_.info().va_start, k_.info().va_end, heapHolds_);
    set_.add(k_, {{&handleTables_, TegraGM20BSurfaceRead}, {&argumentRoots_, TegraGM20BSurfaceWrite}});
    return true;
}

// lock_ held: the CPU wrote the handle tables or a heap set, write-combined: drained here, on the writing core, before the
// next submission's job names them host-written (commit's DSB is the committing core's)
void Device::tablesWritten()
{
#if defined(__aarch64__)
    __asm__ volatile("dsb st" ::: "memory");
#endif
    tablesWritten_.fetch_add(1, std::memory_order_release);
}

// lock_ held: the set 1 of a record's arrays, shared by every record of the same arrays
const HeapSet *Device::heapSet(const HeapLayout &layout)
{
    if (!heapMemory())
        return nullptr;
    if (const HeapSet *set = heapTables_.find(layout))
        return set;
    Bo bo;
    if (!layout.bytes || !k_.allocControl(layout.bytes, bo, ControlUse::HandleTables))
        return nullptr;
    heapSets_.push_back(bo);
    set_.add(k_, bo, TegraGM20BSurfaceRead);
    const HeapSet *set = heapTables_.add(layout, {bo.cpu, bo.va, bo.bytes});
    tablesWritten();
    return set;
}

bool Device::heapTexture(uint32_t tic, const HeapTexture *texture)
{
    std::lock_guard<raw::Mutex> l(lock_);
    if (!texture && !heapTables_.attached())
        return true;
    if (!heapMemory())
        return false;
    heapTables_.texture(tic, texture);
    tablesWritten();
    return true;
}

bool Device::heapSampler(uint32_t tsc, const HeapSampler *sampler)
{
    std::lock_guard<raw::Mutex> l(lock_);
    if (!sampler && !heapTables_.attached())
        return true;
    if (!heapMemory())
        return false;
    heapTables_.sampler(tsc, sampler);
    tablesWritten();
    return true;
}

HeapTexture Device::heapTextureOf(const Texture &t, const HeapTextureFacts &facts, uint32_t flags)
{
    HeapTexture h;
    h.sampleTic = t.addressMap ? 0 : t.standIn ? t.standIn->tic : t.tic;
    h.flags = flags;
    h.facts = heapFacts(facts);
    return h;
}

bool Device::nameTexture(const Texture &t, const HeapTextureFacts &facts, uint32_t flags)
{
    const HeapTexture h = heapTextureOf(t, facts, flags);
    return heapTexture(t.tic, &h);
}

void Device::forgetTexture(const Texture &t)
{
    heapTexture(t.tic, nullptr);
}

bool Device::nameSampler(const Sampler &s, uint32_t word, Sampler &twin)
{
    HeapSampler h = {s.tsc, word};
    if (s.desc.pixel) {
        SamplerDesc normalized = s.desc;
        normalized.pixel = false;
        h.sampleTsc = twin.tsc || createSampler(normalized, twin) ? twin.tsc : 0;
    }
    return h.sampleTsc && heapSampler(s.tsc, &h);
}

void Device::forgetSampler(const Sampler &s, Sampler &twin)
{
    heapSampler(s.tsc, nullptr);
    if (twin.tsc)
        destroySampler(twin);
}

// lock_ held: what `pl`'s reads through argument buffers take of every draw: each stage's root table address (a stage whose
// root table holds buffer slots takes its own per draw: RenderEncoder::rootTable) and ExecutionStatus address in its push
// words, and its set 1. Its draws are refused where the device has no memory for them.
void Device::bindArguments(Pipeline &pl, const PackPipeline &pp)
{
    const bool roots = pp.rootTableWord[0] || pp.rootTableWord[1] || pp.statusWord[0] || pp.statusWord[1];
    if (!roots && !pp.heapArrays)
        return;
    if (!heapMemory()) {
        pl.heapMissing = true;
        return;
    }
    const auto address = [&](uint8_t word, uint64_t va) {
        if (!word || word > 31)
            return;
        pl.push[word - 1] = uint32_t(va);
        pl.push[word] = uint32_t(va >> 32);
        pl.pushMask |= 3u << (word - 1);
    };
    for (uint32_t k = 0; k < 2; k++) {
        address(pp.rootTableWord[k], pp.rootSlots[k] ? 0 : heapTables_.rootTableVa());
        address(pp.statusWord[k], heapTables_.statusVa());
    }
    if (pp.heapArrays && !(pl.heapSet = heapSet(heapLayout(pp))))
        pl.heapMissing = true;
}

// A state of the pack, made the first time it is looked up (most of the pack's states never are in a process): its registers
// as push words with its programs placed in the heap. Else one the runtime made (addRecord). A storage form that computes
// the pipeline's blend has no barrier twin to fall back on: its barrier form leaves the blend to the ROP after a fetch
// outside the interlock, which keeps Metal's result only drawn a triangle at a time. A pass it cannot view as its storage
// images refuses its draws (RenderEncoder::prepare), as Metal fails a state drawn into attachments it was not made for.
// Debug builds place that form as its blendTwin, which only the probe draws, to measure it against the storage form.
Pipeline *Device::pipeline(uint64_t keyHash, PackForm form, bool caTransform)
{
    Pipeline *pl = placePipeline(keyHash, form, caTransform);
    if (form == PackForm::Default && pl && pl->fetchBits && fetchFreeReachable(keyHash)) {
        Pipeline *free = placePipeline(keyHash, PackForm::FetchFree, caTransform);
        std::lock_guard<raw::Mutex> l(lock_);
        if (free && free != pl && free->freeForm.rung != FetchNone)
            pl->fetchFree = free;
    }
    if (form != PackForm::Default || !pl || !pl->storageBits)
        return pl;
#if defined(SWITCH_DEBUG) && !SWITCH_DEBUG
    if (pl->storageBlendBits)
        return pl;
#endif
    Pipeline *twin = placePipeline(keyHash, PackForm::Barrier, caTransform);
    std::lock_guard<raw::Mutex> l(lock_);
    if (twin && twin != pl && !twin->storageBits)
        (pl->storageBlendBits ? pl->blendTwin : pl->barrierTwin) = twin;
    return pl;
}

// Whether the state's fetch-free form, where the pack holds one, is a form a draw can take: its rung's unit proven, or a
// debug build, whose probe channel forces a rung (its programs are placed in the heap only then)
bool Device::fetchFreeReachable(uint64_t keyHash) const
{
    const PackPipeline *rec = pack_.find(keyHash, PackForm::FetchFree);
    if (!rec)
        return false;
    const FetchFreeForm f = fetchFreeForm(*rec, pack_.iterated(*rec));
    const bool proven =
        f.rung == FetchBlend ? fixedBlendProven() : f.rung == FetchIteratedBlend && iteratedPassesProven(f);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return proven || f.rung != FetchNone;
#else
    return proven;
#endif
}

bool Device::usableRecord(const PackPipeline *rec) const
{
    return rec && usableDynamicPush(*rec, pack_.string(rec->keyString)) &&
           usableStorageMetadata(*rec, pack_.section<uint32_t>(PackBlob) + rec->blob, rec->blobDwords);
}

std::vector<std::pair<std::string, Pipeline *>> Device::variantPipelines(uint64_t keyHash, bool caTransform)
{
    std::vector<std::pair<std::string, Pipeline *>> out;
    for (const PackPipeline *rec : pack_.variantRecords(keyHash)) {
        const std::string name = pack_.string(rec->variantName);
        // a barrier record is a storage form's twin unless it is the variant's one record (its storage form refused)
        if ((rec->flags & PipelineBarrierForm) && pack_.variantRecord(keyHash, name, PackForm::Default))
            continue;
        Pipeline *pl = nullptr;
        if (usableRecord(rec)) {
            std::lock_guard<raw::Mutex> l(lock_);
            pl = placeRecord(*rec, caTransform);
        }
        // a storage form's barrier twin, as Device::pipeline gives a state's own
        const PackPipeline *barrier = pl && pl->storageBits && !pl->storageBlendBits
                                          ? pack_.variantRecord(keyHash, name, PackForm::Barrier)
                                          : nullptr;
        if (usableRecord(barrier)) {
            std::lock_guard<raw::Mutex> l(lock_);
            Pipeline *twin = placeRecord(*barrier, caTransform);
            if (twin && twin != pl && !twin->storageBits)
                pl->barrierTwin = twin;
        }
        out.emplace_back(name, pl);
    }
    return out;
}

Pipeline *Device::placePipeline(uint64_t keyHash, PackForm form, bool caTransform)
{
    const PackPipeline *rec = pack_.find(keyHash, form);
    if (rec && !usableRecord(rec)) {
        const bool blends = rec->storageBlendMask;
        const PackPipeline *barrier = form == PackForm::Default && (rec->flags & PipelineStorageMetadata) && !blends
                                          ? pack_.find(keyHash, PackForm::Barrier)
                                          : nullptr;
        rec = usableRecord(barrier) ? barrier : nullptr;
        if (blends)
            os_log_error(
                os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                "raw: pack state %016llx computes its blend in a storage form this runtime cannot read: refused",
                (unsigned long long)keyHash);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (rec) {
            stats.fetch.storageFallback[size_t(StorageFallback::Metadata)]++;
            stats.fetch.fallbackKeys.note(keyHash);
        }
#endif
    }
    std::lock_guard<raw::Mutex> l(lock_);
    if (!rec) {
        const auto made = completed_.find(keyHash);
        return form == PackForm::Default && made != completed_.end() ? made->second : nullptr;
    }
    return placeRecord(*rec, caTransform);
}

Pipeline *Device::placeRecord(const PackPipeline &pp, bool caTransform)
{
    Pipeline &pl = pipelines_[&pp - pack_.section<PackPipeline>(PackPipelines)];
    if (pl.rec)
        return &pl;
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
    build(pl, pp, vs, fs, pack_.section<uint32_t>(PackBlob) + pp.blob, pack_.string(pp.keyString), caTransform,
          pack_.spills(pp));
    offerVariants(pl, pp);
    if (const PackSplit *split = pack_.split(pp.fs); split && !pl.storageBits) {
        SplitSource source;
        source.rec = *split;
        source.packed = true;
        source.pp = &pp;
        source.blob = pack_.section<uint32_t>(PackBlob) + pp.blob;
        source.keyString = pack_.string(pp.keyString);
        source.vs = vs;
        source.caTransform = caTransform;
        offerSplit(pl, std::move(source));
    }
    return &pl;
}

// lock_ held: `pl` of record `pp`, whose programs are placed at `vs` and `fs`, whose registers are `blob` (method, value pairs)
// and whose key's attachment and vertex fields are `keyString`
void Device::build(Pipeline &pl, const PackPipeline &pp, const PlacedProgram &vs, const PlacedProgram &fs,
                   const uint32_t *blob, const char *keyString, bool caTransform, const uint32_t *spills,
                   const PlacedProgram *tess)
{
    auto instructions = [](const PackProgram &program) {
        const uint32_t bytes = program.dataBytes ? program.dataOffset : program.bytes;
        return std::max(1u, (bytes > 128 ? bytes - 128 : 0) / 8);
    };
    pl.vertexInstructions = instructions(*vs.record);
    pl.fragmentInstructions = instructions(*fs.record);
    pl.memoryEntries = pp.memoryEntries;
    std::vector<uint32_t> words(64 + 16 + (tess ? 64 : 0) + pp.blobDwords * 2);
    Push w{words.data(), words.data() + words.size()};
    uint32_t writes[8];
    uint32_t interlockControl = 0;
    uint32_t sampleControl = 1;
    memcpy(writes, ctWrite_, sizeof writes);
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[k], v = blob[k + 1];
        if (ownedMethod(m) || swizzleMethod(m))
            continue;  // a pack made before the method was the runtime's: the probe process's value
        if (m >= CT_WRITE && m < CT_WRITE + 32)
            writes[(m - CT_WRITE) / 4] = v;
        if (m >= VERTEX_STREAM_FORMAT && m < VERTEX_STREAM_FORMAT + kStreamMethods * kPackVertexStreams &&
            (m - VERTEX_STREAM_FORMAT) % kStreamMethods == 0)
            pl.streamStride[(m - VERTEX_STREAM_FORMAT) / kStreamMethods] =
                uint16_t(NVVAL_GET(v, NVB197, SET_VERTEX_STREAM_A_FORMAT, STRIDE));
        if (m == PIXEL_SHADER_INTERLOCK_CONTROL) {
            interlockControl = v;
            continue;
        }
        if (m == HYBRID_ANTI_ALIAS_CONTROL) {
            sampleControl = v;
            continue;
        }
        if (m == VIEWPORT_CLIP_CONTROL) {
            pl.viewportClip = v;
            pl.namesViewportClip = true;
        }
        pl.namesViewportClipRange |= m == NVB197_SET_VIEWPORT_CLIP_MIN_Z(0) || m == NVB197_SET_VIEWPORT_CLIP_MAX_Z(0);
        w.m(m, v);
    }
    w.m(PIXEL_SHADER_INTERLOCK_CONTROL, legalInterlockControl(interlockControl, sampleControl));
    w.m(HYBRID_ANTI_ALIAS_CONTROL, sampleControl);
    pl.blendKill = iteratedBlendNoop(blob, pp.blobDwords, writes, keyString,
                                     (pp.flags & PipelineStorageMetadata) ? pp.storageMask : 0) == kNoopSourceRgba0000;
    w.m(kIteratedBlendOptimization, pl.blendKill ? kNoopSourceRgba0000 : kNoopNever);
    RAW_DIAGNOSTIC(stats.blendKillStates += pl.blendKill);
    for (uint32_t c = 0; c < 8; c++)
        if (writes[c] & 0x1111)  // R_ENABLE, G_ENABLE, B_ENABLE, A_ENABLE
            pl.writeBits |= uint8_t(1u << c);
    w.m(0x2044, vs.program);  // SET_PIPELINE_PROGRAM(VERTEX)
    w.m(0x2144, fs.program);  // SET_PIPELINE_PROGRAM(PIXEL)
    w.m(PIPELINE_REGISTER_COUNT_VERTEX, vs.record->gprs);
    w.m(PIPELINE_REGISTER_COUNT_PIXEL, fs.record->gprs);
    if (fs.record->boundTextures)
        w.m(BINDLESS_TEXTURE, fs.record->boundTextures - 1u);
    bool exactCbufs = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    exactCbufs = probeExactCbufs.load(std::memory_order_relaxed);
#endif
    pl.setSlot[0] = bindProgramCbufs(w, *vs.record, kSetGroups[0], vs.data, root_.va, exactCbufs);
    pl.setSlot[1] = bindProgramCbufs(w, *fs.record, kSetGroups[1], fs.data, root_.va, exactCbufs);
    const PackProgram *programs[4] = {vs.record, fs.record};
    for (uint32_t k = 0; tess && k < 2; k++) {
        w.m(PIPELINE_PROGRAM_TESS + kPipelineStageStride * k, tess[k].program);
        w.m(PIPELINE_REGISTER_COUNT_TESS + kPipelineStageStride * k, tess[k].record->gprs);
        pl.setSlot[2 + k] = bindProgramCbufs(w, *tess[k].record, kSetGroups[2 + k], tess[k].data, root_.va, exactCbufs);
        programs[2 + k] = tess[k].record;
        pl.tessReadsHeap |= heapCbufSlot(*tess[k].record) != 0;
    }
    for (uint32_t k = 0; k < 4; k++)
        for (uint8_t c = 1; programs[k] && c < programs[k]->cbufs; c++) {
            const PackProgram *prog = programs[k];
            const uint8_t group = kSetGroups[k];
            if (cbufKind(prog->cbufType[c]) != CbufUbo)
                continue;
            uint8_t entry = 0;
            while (entry < pp.entries &&
                   !(uniformEntry(pp.entry[entry]) && pp.entry[entry].offset == cbufUboOffset(prog->cbufType[c])))
                entry++;
            pl.ubosNamed &= entry < pp.entries;
            if (entry < pp.entries)
                pl.ubos[pl.uboCount++] = {group, c, entry};
        }
    pl.methods.assign(words.data(), w.at);
    pl.segment = segmentHeap_.place(pl.methods.data(), uint32_t(pl.methods.size()));
    memcpy(pl.push, pp.pushWords, sizeof pl.push);
    pl.pushMask = pp.pushMask;
    pl.heapSlot[0] = heapCbufSlot(*vs.record);
    pl.heapSlot[1] = heapCbufSlot(*fs.record);
    bindArguments(pl, pp);
    for (uint32_t k = 0; k < 2; k++)
        if (pp.spillEntry[k])
            pl.spill[k] = spills ? spills + (k ? pp.spillWords[0] : 0) : nullptr;
    // the color formats of its key, and which colors its draws read as they were (raw_blend_reads.h)
    for (const char *at = keyString; (at = strstr(at, " c")); at += 2) {
        unsigned c = 0;
        ColorKeyFields k = {};
        const int n = sscanf(at, " c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &c, &k.format, &k.writeMask, &k.blending, &k.srcRGB,
                             &k.dstRGB, &k.rgbOp, &k.srcAlpha, &k.dstAlpha, &k.alphaOp);
        if (n >= 4 && c < 8) {
            constexpr unsigned kRGB8_444_2P = 542,
                               kRGBA16Float = 115;  // rendered through its stand-in (Texture::planarStandIn)
            pl.colorFormats[c] = k.format == kRGB8_444_2P ? kRGBA16Float : k.format;
            if (colorReadsDestination(k))
                pl.readBits |= uint8_t(1u << c);
            // MTLBlendFactorBlendColor, OneMinusBlendColor, BlendAlpha, OneMinusBlendAlpha
            for (const unsigned factor : {k.srcRGB, k.dstRGB, k.srcAlpha, k.dstAlpha})
                pl.constantBlend |= k.blending && factor >= 11 && factor <= 14;
        }
    }
    if (pp.flags & PipelineTwoPlane) {
        const std::array<int8_t, 8> alpha = xrAlphaSlots(pl.colorFormats);
        xrPlaneFormats(pl.colorFormats);
        for (uint32_t j = 0; j < 8; j++)
            if (alpha[j] >= 0 && (pl.readBits >> j & 1))
                pl.readBits |= uint8_t(1u << alpha[j]);
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
    pl.primitiveId = (sph[5] >> 24) & 1;  // NVA097_SPHV3T2_IMAP_PRIMITIVE_ID, bit 184
    uint32_t vsph0;
    memcpy(&vsph0, vs.header, sizeof vsph0);
    pl.vertexStores = (vsph0 >> 16) & 1;    // NVA097_SPHV3T1_DOES_GLOBAL_STORE
    pl.fragmentDepth = (sph[19] >> 1) & 1;  // OMAP_DEPTH, bit 609
    pl.viewportIndex = headerWritesViewportIndex(vs.header);
    pl.writesLayer = headerWritesLayer((tess ? tess[1] : vs).header);
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
    if (pp.flags & PipelineStorageMetadata) {
        pl.storageBits = pp.storageMask;
        pl.storageWriteBits = pp.storageWriteMask;
        pl.storageBlendBits = pp.storageBlendMask & pp.storageWriteMask;
        memcpy(pl.storageTic, pp.storageTic, sizeof pl.storageTic);
        pl.readBits &= ~pl.storageBits;
    }
    pl.freeForm = (pp.flags & PipelineFetchFreeForm) ? fetchFreeForm(pp, pack_.iterated(pp)) : FetchFreeForm();
    if (pl.freeForm.rung != FetchNone) {
        pl.readBits |= uint8_t(1u << pl.freeForm.colour);
        if (pl.freeForm.rung == FetchIteratedBlend) {
            pl.restore.clear();
            iteratedBlendWords(*pack_.iterated(pp), pl.freeForm.colour, pl.methods, pl.restore);
        }
    }
    pl.turnRefused = pp.flags & PipelineTurnRefused;
    pl.xrSlots = pp.xrSlots;
    pl.writtenSlots = writtenSlots(pp);
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
bool Device::newChunk(uint64_t size, bool dedicated, std::unique_lock<raw::Mutex> *unlocked, uint32_t &index)
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
    set_.add(k_, ch.bo, TegraGM20BSurfaceRead);
    chunks_[c] = std::move(ch);
    RAW_DIAGNOSTIC(stats.poolBytes += size);
    RAW_DIAGNOSTIC(stats.poolChunks++);
    RAW_DIAGNOSTIC(glassMemEvent(dedicated ? "pool chunk made (dedicated)" : "pool chunk made", size));
    index = c;
    return true;
}

void Device::growLoop()
{
    std::unique_lock<raw::Mutex> l(lock_);
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
        RAW_DIAGNOSTIC(stats.poolStalls++);
        if (growing_) {  // the grower is making one: wait for it rather than wire a second
            std::unique_lock<raw::Mutex> l(lock_, std::adopt_lock);
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
        std::lock_guard<raw::Mutex> l(lock_);
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
    RAW_DIAGNOSTIC(stats.poolBytes -= ch.bytes);
    RAW_DIAGNOSTIC(stats.poolChunks--);
    RAW_DIAGNOSTIC(glassMemEvent("chunk freed", ch.bytes));
    k_.free(ch.bo);
    if (ch.pitchVa)
        k_.releaseVa(ch.pitchVa, ch.bytes);
    vm_deallocate(mach_task_self(), vm_address_t(ch.memory), ch.bytes);
    ch = Chunk();
}

void Device::trim()
{
    std::lock_guard<raw::Mutex> l(lock_);
    reapNativeSurfaces();
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
            RAW_DIAGNOSTIC(stats.importHits++);
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
    if (id) {
        const uint32_t layout = k_.surfaceLayout(i->bo, a.width, a.height, a.bpp, a.bytesPerRow, a.writes, &i->layout);
        if (layout == TegraGM20BLayoutOtherShape) {
            k_.free(i->bo);
            delete i;
            return nullptr;
        }
        if (layout == TegraGM20BLayoutTiled)
            i->kind = TGM_LAYOUT_KIND;
    }
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
    RAW_DIAGNOSTIC(stats.imports++);
    RAW_DIAGNOSTIC(stats.importsMade++);
    RAW_DIAGNOSTIC(stats.importBytes += i->bo.bytes);
    return i;
}

void Device::release(Import *i)
{
    if (!i || --i->refs)
        return;
    if (i->id)
        if (auto it = surfaces_.find(std::make_tuple(i->id, i->base, i->bytes));
            it != surfaces_.end() && it->second == i)
            surfaces_.erase(it);
    RAW_DIAGNOSTIC(stats.imports--);
    RAW_DIAGNOSTIC(stats.importBytes -= i->bo.bytes);
    if (i->native) {
        i->native->cached = false;
        nativeStale_.store(true, std::memory_order_release);
    }
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
    views_.freed(tic);
    const auto freeViews = [&](auto &byHeader) {
        const auto views = byHeader.equal_range(tic);
        for (auto v = views.first; v != views.second; ++v) {
            memcpy(tic_.cpu + 32 * v->second.second, tic_.cpu, 32);
            freeTic_.push_back(v->second.second);
        }
        byHeader.erase(views.first, views.second);
    };
    freeViews(viewTics_);
    freeViews(attachmentViewTics_);
    // a part's header has view headers of its own (passViewTic): freed with it
    std::vector<uint32_t> parts;
    const auto made = partTics_.equal_range(tic);
    for (auto v = made.first; v != made.second; ++v)
        parts.push_back(v->second.second);
    partTics_.erase(made.first, made.second);
    for (const uint32_t part : parts)
        freeTic(part);
}

uint32_t Device::allocTicPair()
{
    if (!freeTicPairs_.empty()) {
        const uint32_t first = freeTicPairs_.back();
        freeTicPairs_.pop_back();
        return first;
    }
    if (nextTic_ + 1 >= kTicCount)
        return 0;
    const uint32_t first = nextTic_;
    nextTic_ += 2;
    return first;
}

// As freeTic leaves a freed header: header 0's null descriptor in both (an over-planes RGB10A8_2P_XR10 texture's pair,
// raw_xr_texture.cpp)
void Device::freeTicPair(uint32_t first)
{
    memcpy(tic_.cpu + 32 * first, tic_.cpu, 32);
    memcpy(tic_.cpu + 32 * (first + 1), tic_.cpu, 32);
    freeTicPairs_.push_back(first);
}

// Texture header of a pitch-linear 2D texture (Mesa NIL nvb097_fill_image_view_desc for a linear image: HEADER_VERSION PITCH,
// TEXTURE_TYPE TWO_D_NO_MIPMAP), word 0 NVK's for the format
void Device::writeTic(const Texture &t)
{
    fx::MipChain one;
    uint32_t w[8] = {};
    if (t.samples > 1) {  // NIL's image of the sample grid (createStoredMultisample, createOwnedZeta, their views)
        const uint32_t z = zetaFormatIndex(t.format);
        const FormatInfo *f = z < kZetaFormats ? nullptr : formatInfo(t.format);
        multisampleHeader(t.width, t.height, t.samples, t.bpp, t.va, f ? f->tic0 : kZetaFormatInfo[z].tic0,
                          f && f->srgb, w);
    } else if (t.zeta) {  // NVK's image as laid out (createZetaOver): one level, its block height
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
        if (t.blockLinear || t.blockW > 1 || t.blockH > 1)
            one.yLog2 = t.blockHeight;
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
    std::lock_guard<raw::Mutex> l(lock_);
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

// createTexture's block-linear layout of shaped `t`, where it is asked tiled: a colour format's (the chunks are bound under
// colour kind 0xFE, which a depth/stencil format's zeta is not) or a block-compressed one's (NVK's optimal image, never
// pitch), of a shape that tiles
static bool plainTiled(const Texture &t, bool tiled, tgm_layout &bl)
{
    const bool blocks = t.blockW > 1 || t.blockH > 1;
    if (!tiled || !(t.renderable || blocks) || !tgm_layout_tiled(t.width, t.height, t.bpp, &bl))
        return false;
    if (blocks)
        blockLayout(t, bl);
    return true;
}

static void tiledRows(Texture &t, const tgm_layout &bl)
{
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
}

static void chainRows(Texture &t, const fx::MipChain &c, uint64_t layerBytes)
{
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.layerBytes = layerBytes;
}

bool Device::createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc, bool tiled,
                           const ArenaAt *at)
{
    if (at) {
        TextureAsk ask;
        ask.width = width;
        ask.height = height;
        ask.format = format;
        ask.tiled = tiled;
        return createLaidOut(ask, t, at);
    }
    if (!shape(width, height, format, t))
        return false;
    const bool blocks = t.blockW > 1 || t.blockH > 1;
    t.pitch = (t.width * t.bpp + 255) & ~255u;
    std::lock_guard<raw::Mutex> l(lock_);
    const uint64_t bytes = uint64_t(t.pitch) * t.height;
    tgm_layout bl;
    const bool laid = !wc && plainTiled(t, tiled, bl);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (probeCompressNext.exchange(false) && laid && !blocks && compressedKind(t.bpp, 0))
        return probeCompressible(bl, t);
#endif
    if (laid && blAlloc(bl.bytes, bl.align, t.span)) {
        tiledRows(t, bl);
    } else if (blocks) {
        return false;
    } else if (wc && wcAlloc(bytes, t.span)) {
        t.wc = true;
        RAW_DIAGNOSTIC(stats.wcBytes += t.span.bytes);
    } else {
        if (wc)
            RAW_DIAGNOSTIC(stats.wcFallbacks++);
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

bool Device::layout(const TextureAsk &ask, Texture &t, uint64_t &bytes, uint64_t &align, uint32_t &kind,
                    fx::MipChain *chain)
{
    fx::MipChain own;
    fx::MipChain &c = chain ? *chain : own;
    const FormatInfo *f = formatInfo(ask.format);
    const bool xr = xrForm(ask.format) != XrForm::None;
    kind = fx::kBlockLinearKind;
    switch (ask.kind) {
    case TextureAsk::Plain: {
        tgm_layout bl;
        if (!shape(ask.width, ask.height, ask.format, t))
            return false;
        if (plainTiled(t, ask.tiled, bl)) {
            tiledRows(t, bl);
            bytes = bl.bytes;
            align = bl.align;
            return true;
        }
        if (t.blockW > 1 || t.blockH > 1)
            return false;
        t.pitch = (t.width * t.bpp + 255) & ~255u;
        bytes = uint64_t(t.pitch) * t.height;
        align = 256;
        kind = kPteKindPitch;
        return true;
    }
    case TextureAsk::Line: {
        tgm_layout bl;
        if (!shape(ask.width, 1, ask.format, t) || xr || !tgm_layout_tiled(ask.width, 1, t.bpp, &bl))
            return false;
        tiledRows(t, bl);
        t.type = fx::kTexOneD;
        bytes = bl.bytes;
        align = bl.align;
        return true;
    }
    case TextureAsk::Mips:
        if (!f || ask.levels < 2 || !f->rt || xr || !fx::mipChainLayout(ask.width, ask.height, ask.levels, f->bpp, c) ||
            !shape(ask.width, ask.height, ask.format, t))
            return false;
        chainRows(t, c, c.bytes);
        break;
    case TextureAsk::Volume:
        if (!shape(ask.width, ask.height, ask.format, t) || xr ||
            !fx::volumeLayout(ask.width, ask.height, ask.depth, ask.levels, t.bpp, ask.view2d, c))
            return false;
        chainRows(t, c, c.bytes);
        t.type = fx::kTexThreeD;
        t.depth = ask.depth;
        t.blockDepth = c.zLog2;
        break;
    case TextureAsk::Array:
        if (!shape(ask.width, ask.height, ask.format, t) || xr ||
            !fx::arrayLayout(ask.width, ask.height, ask.layers, ask.levels, t.bpp, c))
            return false;
        chainRows(t, c, c.arrayStride);
        t.type = ask.type;
        t.layers = ask.layers;
        break;
    case TextureAsk::Multisample:
        if (!shape(ask.width, ask.height, ask.format, t) || !t.renderable || blockCompressed(f) || xr ||
            !fx::msLayout(t.width, t.height, ask.samples, t.bpp, c))
            return false;
        chainRows(t, c, uint64_t(c.level[0].rowBytes) * c.level[0].rows);
        t.samples = uint8_t(ask.samples);
        break;
    case TextureAsk::Zeta:
        if (!zetaShape(ask.width, ask.height, ask.format, ask.samples, t, bytes, align))
            return false;
        align = std::max(align, kBlAlign);
        kind = kZetaFormatInfo[zetaFormatIndex(ask.format)].kind;
        return true;
    }
    bytes = c.bytes;
    align = c.align;
    return true;
}

// An arena's block-linear memory at NVK's image alignment, as m2v's twin over it in place takes it
static void arenaRounded(uint32_t kind, uint64_t &bytes, uint64_t &align)
{
    if (kind == kPteKindPitch)
        return;
    bytes = (bytes + kBlAlign - 1) & ~(kBlAlign - 1);
    align = std::max(align, kBlAlign);
}

bool Device::footprint(const TextureAsk &ask, uint64_t &bytes, uint64_t &align)
{
    Texture t;
    uint32_t kind = 0;
    if (!layout(ask, t, bytes, align, kind))
        return false;
    arenaRounded(kind, bytes, align);
    if (!ask.standIn)
        return true;
    uint64_t standIn = 0, standInAlign = 0;
    if (!footprint(standInAsk(t), standIn, standInAlign))
        return false;
    bytes = (bytes + standInAlign - 1) / standInAlign * standInAlign + standIn;
    align = std::max(align, standInAlign);
    return true;
}

TextureAsk Device::standInAsk(const Texture &t)
{
    TextureAsk ask;
    ask.width = t.width;
    ask.height = t.height;
    ask.format = kXrStandInFormat;
    ask.tiled = true;
    return ask;
}

bool Device::createLaidOut(const TextureAsk &ask, Texture &t, const ArenaAt *at)
{
    uint64_t bytes = 0, align = 0;
    uint32_t kind = 0;
    Texture::Chain *chain = ask.kind == TextureAsk::Mips ? new Texture::Chain : nullptr;
    const bool image = ask.kind == TextureAsk::Volume || ask.kind == TextureAsk::Array;
    fx::MipChain imageLayout;
    if ((!at && ask.kind == TextureAsk::Plain) || !layout(ask, t, bytes, align, kind,
                                                          chain   ? &chain->layout
                                                          : image ? &imageLayout
                                                                  : nullptr)) {
        delete chain;
        return false;
    }
    if (at)
        arenaRounded(kind, bytes, align);
    const bool zeta = ask.kind == TextureAsk::Zeta;
    const uint32_t z = zetaFormatIndex(ask.format);
    std::lock_guard<raw::Mutex> l(lock_);
    Span owned;
    const bool placed = at     ? arenaSpan(*at, bytes, align, kind, t)
                        : zeta ? tiledAlloc(zChunks_[z], kind, bytes, align, owned)
                               : blAlloc(bytes, align, t.span);
    if (!placed) {
        delete chain;
        return false;
    }
    const Span &span = zeta && !at ? owned : t.span;
    if ((!zeta || ask.sampled || ask.samples > 1) && !(t.tic = allocTic())) {
        Lease *none = nullptr;
        if (zeta && !at)
            tiledFree(zChunks_[z], owned);
        else
            freeSpan(t.span, memoryOf(t), none);
        delete chain;
        return false;
    }
    t.va = span.va;
    if (zeta) {
        t.storage = {span.va, bytes, false};
        if (!at)
            ownedZeta_[span.va] = {z, owned};
    } else {
        t.cpu = span.cpu;
    }
    if (chain) {
        const FormatInfo *f = formatInfo(ask.format);
        chain->va = t.va;
        chain->fmt2d = fx::twodFormat(ask.format);
        uint32_t w[8];
        fx::mipChainTic(chain->layout, chain->va, f->tic0, f->srgb, 0, ask.levels, w);
        // the texture's header: the chain's (visible at the next header invalidate)
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
        t.chain = chain;
        RAW_DIAGNOSTIC(stats.chains++);
        RAW_DIAGNOSTIC(stats.chainBytes += chain->layout.bytes);
    } else if (image) {
        layImage(imageLayout, formatInfo(ask.format), t.type, t);
    } else if (t.tic) {
        writeTic(t);
    }
    return true;
}

bool Device::createLine(uint32_t width, uint32_t format, Texture &t, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Line;
    ask.width = width;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createVolume(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, bool view2d, Texture &t,
                          uint32_t levels, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Volume;
    ask.width = width;
    ask.height = height;
    ask.depth = depth;
    ask.levels = levels;
    ask.format = format;
    ask.view2d = view2d;
    return createLaidOut(ask, t, at);
}

bool Device::createArray(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t type, Texture &t,
                         uint32_t levels, const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Array;
    ask.width = width;
    ask.height = height;
    ask.layers = layers;
    ask.levels = levels;
    ask.format = format;
    ask.type = uint8_t(type);
    return createLaidOut(ask, t, at);
}

void Device::layImage(const fx::MipChain &c, const FormatInfo *f, uint8_t type, Texture &t)
{
    const bool volume = type == fx::kTexThreeD;
    t.pitch = c.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = c.level[0].yLog2;
    t.type = type;
    t.layers = volume ? 1 : c.layers;
    t.layerBytes = volume ? c.bytes : c.arrayStride;
    t.depth = c.depth;
    t.blockDepth = c.zLog2;
    if (c.levels == 1) {
        writeTic(t);
        return;
    }
    t.chain = new Texture::Chain;
    t.chain->layout = c;
    t.chain->va = t.va;
    t.chain->fmt2d = volume ? 0 : fx::twodFormat(t.format);
    uint32_t w[8];
    fx::mipChainTic(c, t.va, f->tic0, f->srgb, 0, c.levels, w, type);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // visible at the next submission's header invalidate
    RAW_DIAGNOSTIC(stats.chains++);
    RAW_DIAGNOSTIC(stats.chainBytes += c.bytes);
}

// A texture over a block-linear surface's import: TegraGM20BLayout.h's image from the surface's first byte, at the shape the
// kernel decided (a view of another format of the same texel size reads it as its own), under the import's
// TGM_LAYOUT_KIND binding, which the 3D and 2D engines, the copy engine and the texture unit all address so
bool Device::tiledSurfaceTexture(uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, Texture &t,
                                 Texture *sampled)
{
    const tgm_layout &l = t.import->layout;
    SurfaceView view;
    if (!surfaceView(l, offset, width, height, t.bpp, pitch, view) || (!view.direct && !sampled)) {
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
    t.storage = {t.va, l.bytes, !t.import->id};
    t.surfaceX = view.x;
    t.surfaceY = view.y;
    t.surfaceView = !view.direct;
    t.shadowTarget = view.x || view.y;
    if (t.surfaceView) {
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

bool Device::createSurfaceTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                  uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, uint32_t format,
                                  Texture &t, bool writes)
{
    // rows and offset a multiple of 16, as m2v takes a surface (linear_plan.rs); the texture header's 32 through `sampled`
    if (!shape(width, height, format, t) || (pitch & 15) || (offset & 15) || pitch < width * t.bpp ||
        offset > allocBytes || uint64_t(pitch) * (height - 1) + uint64_t(width) * t.bpp > allocBytes - offset ||
        (uintptr_t(base) & (pageSize() - 1)))
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
    std::unique_lock<raw::Mutex> l(lock_);
    t.pitch = pitch;
    const LayoutAsk ask = {width, height, t.bpp, pitch, writes && !offset && !sampled};
    t.import = importSurface(surfaceId, base, (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1), surface, &ask);
    if (t.import && t.import->kernelTiled()) {
        SurfaceView view;
        const bool valid = surfaceView(t.import->layout, offset, width, height, t.bpp, pitch, view);
        if (valid && !view.direct && !sampled) {
            l.unlock();
            sampled = new Texture;
            if (!createTexture(width, height, format, *sampled, false, true)) {
                delete sampled;
                sampled = nullptr;
            }
            l.lock();
        }
        tiledSurfaceTexture(offset, width, height, pitch, t, sampled);
    }
    if (!t.import) {
        l.unlock();
        if (sampled && sampled != t.sampled) {
            destroyTexture(*sampled);
            delete sampled;
        }
        return false;
    }
    if (t.blockLinear) {
        l.unlock();
        if (sampled && sampled != t.sampled) {
            destroyTexture(*sampled);
            delete sampled;
        }
        return true;
    }
    t.va = t.import->bo.va + offset;
    t.storage = {t.va, std::min(uint64_t(pitch) * height, allocBytes - offset), !t.import->id};
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
        std::lock_guard<raw::Mutex> l(lock_);
        planes = importSurface(surfaceId, base, (allocBytes + pageSize() - 1) & ~uint64_t(pageSize() - 1), surface);
    }
    if (!planes)
        return false;
    if (!createTexture(width, height, kRGBA8Unorm, t)) {  // the image: pool memory, its texture header
        std::lock_guard<raw::Mutex> l(lock_);
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
        pitch < width * t.bpp || offset > b.length || uint64_t(pitch) * height > b.length - offset ||
        offset > UINT64_MAX - b.va || ((b.va + offset) & 31))
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    t.pitch = pitch;
    t.va = b.va + offset;
    t.storage = {t.va, uint64_t(pitch) * height, b.hostVisible};
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
    std::lock_guard<raw::Mutex> l(lock_);
    t = of;
    t.storage = of.storageRange();
    memcpy(t.swizzle, kIdentitySwizzle, 4);
    t.view = true;
    t.format = format;
    t.renderable = f->rt != 0 && (t.blockLinear || linearTarget(t));
    t.shadowTarget = f->rt != 0 && !t.blockLinear && !linearTarget(t);
    // Native-plane lookup needs the parent import; a view never releases its non-owning reference.
    t.import = of.nativePlane != ~0u ? of.import : nullptr;
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
    std::lock_guard<raw::Mutex> l(lock_);
    t = image;
    t.storage = of.storageRange();
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
                            Texture &t, const char **why, uint32_t layers)
{
    uint64_t bytes, align;
    *why = !zetaShape(width, height, format, 1, t, bytes, align) ? "a size or format the raw path renders no zeta of"
           : !m.va                                               ? "its image is not bound"
           : !m.tiled                                            ? "its image is pitch-linear"
           : m.blockDepth                                        ? "its image's blocks are deeper than one plane"
           : m.bpp != t.bpp                                      ? "its texel size is not the format's"
           : m.pitch < width * t.bpp || m.pitch % 64             ? "its row stride is not whole GOBs of its texels"
           : m.bytes < m.layerBytes * layers || uint64_t(m.pitch) * height > m.bytes
               ? "its image is smaller than its rows"
           : sampled && m.pitch != (width * t.bpp + 63) / 64 * 64
               ? "its row stride is not the one a texture header implies"
               : nullptr;
    if (*why)
        return false;
    t.va = m.va;
    t.storage = {m.va, m.bytes, false};
    t.pitch = m.pitch;
    t.blockHeight = m.blockHeight;
    t.layerBytes = m.layerBytes;
    t.layers = layers;
    t.comptags = m.compressed;
    if (!sampled || layers > 1)
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

bool Device::createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t, uint32_t samples,
                              uint32_t layers)
{
    uint64_t bytes, align;
    const bool zeta = zetaFormatIndex(format) < kZetaFormats;
    if (!layers || layers > 2048 || (layers > 1 && samples > 1) ||
        (zeta ? !zetaShape(width, height, format, samples, t, bytes, align) : !shape(width, height, format, t)) ||
        (layers > 1 && !zeta && (!t.renderable || xrForm(format) != XrForm::None)))
        return false;
    t.layers = layers;
    fx::MipChain c;
    if (samples > 1 && !zeta &&
        (!t.renderable || xrForm(format) != XrForm::None || !fx::msLayout(width, height, samples, t.bpp, c)))
        return false;
    t.memoryless = true;
    t.samples = uint8_t(samples);
    compPrearm(t);
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
                           std::unique_lock<raw::Mutex> *unlocked, uint32_t &index)
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
    set_.add(k_, ch.bo, TegraGM20BSurfaceWrite);
    chunks[c] = std::move(ch);
    RAW_DIAGNOSTIC(stats.poolBytes += size);
    RAW_DIAGNOSTIC(stats.poolChunks++);
    RAW_DIAGNOSTIC(glassMemEvent(kind == fx::kBlockLinearKind ? "block-linear chunk made" : "zeta chunk made", size));
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
        RAW_DIAGNOSTIC(stats.poolStalls++);
        if (tiledWanted(chunks)) {  // the grower is making one: wait for it rather than wire a second
            std::unique_lock<raw::Mutex> l(lock_, std::adopt_lock);
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

uint64_t Device::blPitchVa(const Span &s)
{
    if (!s.valid() || s.chunk >= blChunks_.size())
        return 0;
    Chunk &ch = blChunks_[s.chunk];
    if (!ch.pitchVa) {
        const uint64_t va = k_.reserveVa(ch.bytes);
        if (!va)
            return 0;
        if (!k_.bindAlias(ch.bo, va, kPteKindPitch)) {
            k_.releaseVa(va, ch.bytes);
            return 0;
        }
        ch.pitchVa = va;
        Bo view = ch.bo;
        view.va = view.vaBase = va;
        // the chunk is a set member: the kernel takes no use of its backing, so a job names this binding by the set too
        set_.add(k_, view, TegraGM20BSurfaceWrite);
    }
    return ch.pitchVa + s.offset;
}

bool Device::createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t,
                              const ArenaAt *at)
{
    TextureAsk ask;
    ask.kind = TextureAsk::Mips;
    ask.width = width;
    ask.height = height;
    ask.levels = levels;
    ask.format = format;
    return createLaidOut(ask, t, at);
}

bool Device::createBlockTextureOver(void *bytes, uint64_t length, uint32_t width, uint32_t height, uint32_t format,
                                    const fx::MipChain &layout, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    if (!blockCompressed(f) || !bytes || length < layout.bytes || layout.bpp != f->bpp ||
        !shape(width, height, format, t))
        return false;
    const bool inPlace = blockImageInPlace(uintptr_t(bytes), layout);
    const uint32_t kind = inPlace ? TGM_LAYOUT_KIND : kPteKindPitch;
    cleanToMemory(bytes, layout.bytes);
    Import *i = new Import;
    if (!k_.importPages(bytes, length, i->bo, 0, true)) {
        delete i;
        return false;
    }
    std::lock_guard<raw::Mutex> l(lock_);
    if (!k_.bindImport(bytes, i->bo, kind)) {
        delete i;
        return false;
    }
    i->kind = kind;
    i->refs = 1;
    i->base = uintptr_t(bytes);
    i->bytes = length;
    RAW_DIAGNOSTIC(stats.imports++);
    RAW_DIAGNOSTIC(stats.importsMade++);
    RAW_DIAGNOSTIC(stats.importBytes += i->bo.bytes);
    if (!inPlace && !blAlloc(layout.bytes, layout.align, t.span)) {
        release(i);
        return false;
    }
    if ((!inPlace && !(t.imagePitchVa = blPitchVa(t.span))) || !(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, Memory::Bl, none);
        release(i);
        return false;
    }
    if (inPlace) {
        t.import = i;
        t.va = i->bo.va;
        t.cpu = static_cast<uint8_t *>(bytes);
    } else {
        t.callerPages = i;
        t.va = t.span.va;
        t.cpu = t.span.cpu;
    }
    t.storage = {t.va, layout.bytes, inPlace};
    layBlocks(layout, f, width, height, uint8_t(layout.layers > 1 ? fx::kTexTwoDArray : fx::kTexTwoD), t);
    return true;
}

bool Device::createBlockTexture(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, uint32_t format,
                                uint8_t type, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    fx::MipChain c;
    if (!blockCompressed(f) || !shape(width, height, format, t) ||
        !fx::blockChainLayout(width, height, levels, layers, f->blockW, f->blockH, f->bpp, c))
        return false;
    std::lock_guard<raw::Mutex> l(lock_);
    if (!blAlloc(c.bytes, c.align, t.span))
        return false;
    t.va = t.span.va;
    t.cpu = t.span.cpu;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(t.span, Memory::Bl, none);
        return false;
    }
    layBlocks(c, f, width, height, type, t);
    return true;
}

void Device::layBlocks(const fx::MipChain &layout, const FormatInfo *f, uint32_t width, uint32_t height, uint8_t type,
                       Texture &t)
{
    t.pitch = layout.level[0].rowBytes;
    t.blockLinear = true;
    t.blockHeight = layout.yLog2;
    t.layers = layout.layers;
    t.layerBytes = layout.arrayStride;
    t.type = type;
    uint32_t w[8];
    fx::mipChainTic(layout, t.va, f->tic0, f->srgb, 0, layout.levels, w, t.type);
    setField(w, 143, 128, width - 1);  // WIDTH_MINUS_ONE in texels (NIL's extent_px)
    setField(w, 175, 160, height - 1);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);
    if (layout.levels > 1) {
        t.chain = new Texture::Chain;
        t.chain->layout = layout;
        t.chain->va = t.va;
    }
}

void Device::destroyTexture(Texture &t)
{
#if SWITCH_RAW_DIAGNOSTICS
    if (glassMemOn.load(std::memory_order_relaxed))
        glassMem->forget(t.storageRange().va ? t.storageRange().va : t.va, t.view ? t.tic : 0);
#endif
    if (t.view) {
        std::lock_guard<raw::Mutex> l(lock_);
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
    delete t.ycbcrStore;
    t.ycbcrStore = nullptr;
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
    std::lock_guard<raw::Mutex> l(lock_);
    if (t.tic && t.xrPlanes) {
        freeTicPair(t.tic);
    } else if (t.tic) {
        freeTic(t.tic);
    }
    if (t.zeta)
        freeOwnedZeta(t.va);
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
    if (t.callerPages)
        release(t.callerPages);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (auto it = probeCompressed.find(t.va); t.comptags && !t.span.valid() && it != probeCompressed.end()) {
        k_.free(it->second);
        probeCompressed.erase(it);
    }
#endif
    releasePrivateCompressed(t);
    freeSpan(t.span, memoryOf(t), t.lease);
    t = {};
}

bool Device::createBuffer(uint64_t length, Buffer &b, bool wc)
{
    b = {};
    if (!length)
        length = 16;
    {
        std::lock_guard<raw::Mutex> l(lock_);
        if (wc && wcAlloc(length, b.span)) {
            b.wc = true;
            RAW_DIAGNOSTIC(stats.wcBytes += b.span.bytes);
        } else {
            if (wc)
                RAW_DIAGNOSTIC(stats.wcFallbacks++);
            if (!poolAlloc(length, b.span))
                return false;
        }
    }
    b.va = b.span.va;
    b.cpu = b.span.cpu;
    b.length = length;
    // A reused span may still have dirty GPU lines from its previous owner. Acquire before exposing CPU bytes.
    HostAccess access;
    if (!hostAccess(b, true, access) || !access.finish()) {
        destroyBuffer(b);
        return false;
    }
    return true;
}

bool Device::createControlBuffer(uint64_t length, Buffer &buffer)
{
    buffer = {};
    if (!length)
        length = 16;
    std::lock_guard<raw::Mutex> lock(lock_);
    Chunk chunk;
    if (!k_.allocControl(length, chunk.bo, ControlUse::Bytes))
        return false;
    set_.add(k_, chunk.bo, TegraGM20BSurfaceRead);
    chunk.memory = chunk.bo.cpu;
    chunk.bytes = chunk.used = chunk.bo.bytes;
    chunk.dedicated = true;
    uint32_t index = 0;
    while (index < wcChunks_.size() && wcChunks_[index].bo.handle)
        ++index;
    if (index == wcChunks_.size())
        wcChunks_.emplace_back();
    buffer.span = {index, 0, chunk.bytes, chunk.bo.va, chunk.bo.cpu};
    buffer.va = chunk.bo.va;
    buffer.cpu = chunk.bo.cpu;
    buffer.length = length;
    buffer.wc = true;
    buffer.hostVisible = false;
    RAW_DIAGNOSTIC(stats.wcBytes += chunk.bytes);
    wcChunks_[index] = std::move(chunk);
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
        set_.add(k_, ch.bo, TegraGM20BSurfaceRead);
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
    RAW_DIAGNOSTIC(stats.wcBytes -= s.bytes);
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
    std::lock_guard<raw::Mutex> l(lock_);
    uint32_t n = 0;
    for (const Chunk &ch : wcChunks_)
        n += ch.bo.handle && !ch.dedicated;
    return n;
}

uint64_t Device::heapInPlaceBytes()
{
    std::lock_guard<raw::Mutex> l(lock_);
    uint64_t bytes = packPages_.boundBytes();
    for (const StoreRegion &r : storeRegions_)
        bytes += r.pages.boundBytes();
    return bytes;
}

uint64_t Device::controlMappedBytes()
{
    std::lock_guard<raw::Mutex> l(lock_);
    uint64_t bytes = 0;
    for (const Bo *bo : {&tic_, &tsc_, &root_, &zero_, &tickets_, &colorTable_, &srgbEncode_, &stamps_, &handleTables_,
                         &argumentRoots_, &push_.bo, &upload_.bo})
        bytes += bo->mapped;
    for (const Bo &bo : heapSets_)
        bytes += bo.mapped;
    return bytes;
}

uint64_t Device::wcChunkBytesHeld()
{
    std::lock_guard<raw::Mutex> l(lock_);
    uint64_t n = 0;
    for (const Chunk &ch : wcChunks_)
        n += ch.bo.handle && !ch.dedicated ? ch.bytes : 0;
    return n;
}

uint64_t Device::blChunkFreeBytes()
{
    std::lock_guard<raw::Mutex> l(lock_);
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
    std::lock_guard<raw::Mutex> l(lock_);
    if (!(b.import = importSurface(0, bytes, length, nullptr)))
        return false;
    b.va = b.import->bo.va;
    b.cpu = static_cast<uint8_t *>(bytes);
    b.length = length;
    return true;
}

bool Device::createSurfaceBuffer(uint64_t id, void *base, uint64_t bytes, uint64_t length, const void *owner, Buffer &b,
                                 bool &tiled)
{
    b = {};
    tiled = false;
    std::lock_guard<raw::Mutex> l(lock_);
    Import *i = importSurface(id, base, bytes, owner);
    if (!i)
        return false;
    if (i->kind == TGM_LAYOUT_KIND) {
        tiled = true;
        release(i);
        return false;
    }
    b.import = i;
    b.va = i->bo.va;
    b.cpu = static_cast<uint8_t *>(base);
    b.length = length;
    return true;
}

void Device::destroyBuffer(Buffer &b)
{
    std::lock_guard<raw::Mutex> l(lock_);
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
    uint32_t w[8], compared[8];
    samplerHeader(d, w);
    memcpy(compared, w, sizeof w);
    compareHeader(compared, d.compare);
    s.desc = d;
    std::lock_guard<raw::Mutex> l(lock_);
    if (!takeTsc(s.tsc))
        return false;
    if (!takeTsc(s.compareTsc)) {
        freeTsc_.push_back(s.tsc);
        s.tsc = 0;
        return false;
    }
    memcpy(tsc_.cpu + 32 * s.tsc, w, 32);
    memcpy(tsc_.cpu + 32 * s.compareTsc, compared, 32);
    return true;
}

void Device::destroySampler(Sampler &s)
{
    std::lock_guard<raw::Mutex> l(lock_);
    if (s.tsc)
        freeTsc_.push_back(s.tsc);
    if (s.compareTsc)
        freeTsc_.push_back(s.compareTsc);
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
    RAW_ENCODE_LOCK(l, lock_);
    uint32_t tsc = 0;
    if (!takeTsc(tsc))
        return false;
    memcpy(tsc_.cpu + 32 * tsc, w, 32);
    s.mapped[map] = tsc;
    return true;
}

Pipeline *Device::ycbcrPipeline(uint8_t output)
{
    static const char *const names[] = {"",
                                        "raw.ycbcr.ycbcr_converted_frag",
                                        "raw.ycbcr.ycbcr_srgb_frag",
                                        "raw.ycbcr.ycbcr_codes_frag",
                                        "raw.ycbcr.ycbcr_codes10_frag",
                                        "raw.ycbcr.ycbcr_packed_converted_frag",
                                        "raw.ycbcr.ycbcr_packed_srgb_frag",
                                        "raw.ycbcr.ycbcr_packed_codes10_frag",
                                        "raw.ycbcr.ycbcr_packed_plane_frag"};
    if (!output || output >= sizeof names / sizeof *names)
        return nullptr;
    return pipeline(Pack::fnv(names[output], strlen(names[output])));
}

Pipeline *Device::ycbcrStorePipeline(bool chroma, const YcbcrStoreShape &store)
{
    static const char *const names[2][2][2] = {
        {{"raw.ycbcr_store.ycbcr_store_luma8_frag", "raw.ycbcr_store.ycbcr_store_luma16_frag"},
         {"raw.ycbcr_store.ycbcr_store_chroma8_frag", "raw.ycbcr_store.ycbcr_store_chroma16_frag"}},
        {{"raw.ycbcr_store.ycbcr_store_luma8_srgb_frag", "raw.ycbcr_store.ycbcr_store_luma16_srgb_frag"},
         {"raw.ycbcr_store.ycbcr_store_chroma8_srgb_frag", "raw.ycbcr_store.ycbcr_store_chroma16_srgb_frag"}}};
    static const char *const packed[2][2] = {
        {"raw.ycbcr_store.ycbcr_store_luma_packed_frag", "raw.ycbcr_store.ycbcr_store_chroma_packed_frag"},
        {"raw.ycbcr_store.ycbcr_store_luma_packed_srgb_frag", "raw.ycbcr_store.ycbcr_store_chroma_packed_srgb_frag"}};
    const char *name = store.plane    ? "raw.ycbcr_store.ycbcr_store_plane_packed_frag"
                       : store.packed ? packed[store.srgb != 0][chroma]
                                      : names[store.srgb != 0][chroma][store.max > 255];
    return pipeline(Pack::fnv(name, strlen(name)));
}

Pipeline *Device::xrPipeline(XrForm form, bool twoPlanes)
{
    const char *name = xrConversion(form, twoPlanes);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

Pipeline *Device::resolvePipeline(uint32_t format, uint32_t samples)
{
    const char *name = resolveState(format, samples);
    Pipeline *pl = name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
    RAW_DIAGNOSTIC(if (pl) pl->spanName = name);
    return pl;
}

Pipeline *Device::depthResolvePipeline(uint32_t format, uint32_t filter)
{
    const char *name = depthResolveState(format, filter);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

Pipeline *Device::mipPipeline(uint32_t format)
{
    const char *name = mipState(format);
    return name ? pipeline(Pack::fnv(name, strlen(name))) : nullptr;
}

bool Device::createStandIn(Texture &t, const ArenaAt *at)
{
    if (t.standIn || !xrPipeline(xrForm(t.format), t.xrPlanes))
        return t.standIn != nullptr;
    Texture *s = new Texture;
    if (!createTexture(t.width, t.height, kXrStandInFormat, *s, false, true, at)) {
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
    if (!createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.luma, plan.lumaWidth, height, plan.lumaPitch,
                              plan.lumaFormat, *y))
        return undo();
    if (c && !createSurfaceTexture(surfaceId, base, allocBytes, surface, plan.chroma, plan.chromaWidth,
                                   plan.chromaHeight, plan.chromaPitch, plan.chromaFormat, *c))
        return undo();
    if (!createTexture(width, height, plan.image, t, false, true))  // block-linear, as NVK makes m2v's converted image
        return undo();
    if (plan.nativePlanes) {
        y->nativePlane = 0;
        if (c)
            c->nativePlane = 1;
    }
    t.luma = y;
    t.chroma = c;
    memcpy(t.shape, plan.shape, sizeof t.shape);
    t.ycbcr = plan.output;
    t.addressMap = plan.addressMap;
    // its alpha read as one, as m2v's view of its own converted image reads it (W_SOURCE ONE_FLOAT; the conversion writes 1 there
    // too: the same texels, now the same header)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        uint32_t *w = reinterpret_cast<uint32_t *>(tic_.cpu + 32 * t.tic);
        w[0] = (w[0] & ~(7u << 28)) | 7u << 28;
        // a packed plane's channels past its codes read 0, as Metal reads R10Unorm_PACKED and RG10Unorm_PACKED (Z_SOURCE,
        // and Y_SOURCE of a one-code texel, ZERO)
        if (plan.output == YcbcrPackedPlane)
            w[0] &= ~(7u << 25 | (plan.shape[2] == 1 ? 7u << 22 : 0u));
    }
    return true;
}

// A memoryless attachment's memory: one per size, format, tag and layout, kept while passes use it (m2v 0105 gives a memoryless
// attachment linear memory in a pass that renders to linear memory; NVK renders a pass with a block-linear colour target or a
// depth/stencil attachment into block-linear colour). Called with the device lock held.
Device::PlainMem *Device::largestPlain(uint32_t tag, uint32_t memory, bool layered) const
{
    PlainMem *best = nullptr;
    for (const auto &m : plainMems_)
        if (m->holds(tag, memory, layered) && (!best || m->span.bytes > best->span.bytes))
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
    } else if (m.layers > 1) {
        return nullptr;
    } else {
        if (!f || !shape(m.width, m.height, m.format, t))
            return nullptr;
        t.pitch = (m.width * t.bpp + 255) & ~255u;
        bytes = uint64_t(t.pitch) * t.height;
    }
    // a layered pass's: its layers one layer's whole tiles apart (NIL's array stride of a one-level image)
    t.layers = std::max(m.layers, 1u);
    const bool layered = t.layers > 1;
    if (layered)
        bytes = (t.layerBytes * t.layers + align - 1) / align * align;
    PlainMem *mem = largestPlain(tag, memory, layered);
    if (!mem || mem->span.bytes < bytes || (mem->span.va & (align - 1))) {
        auto made = std::make_unique<PlainMem>();
        made->tag = tag;
        made->memory = memory;
        made->layered = layered;
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
        RAW_DIAGNOSTIC(stats.backingBytes += made->span.bytes);
        RAW_DIAGNOSTIC(glassMemEvent("memoryless plain memory made", made->span.bytes));
        plainMems_.push_back(std::move(made));
        mem = plainMems_.back().get();
        // the views of its tag's smaller memory: no pass takes them again (an animated layer seldom asks a size twice), so it
        // goes back once their last uses complete rather than 10 s on
        for (Backing *b : backings_)
            if (b->plain && b->plain != mem && b->plain->holds(tag, memory, layered))
                b->retired = true;
        backingsChanged();
        compRearm_ = true;  // an idle process gives it back when it turns stale
        compStart();
    }
    t.va = mem->span.va;
    t.storage = {t.va, bytes, false};
    t.cpu = memory == kPlainPool ? mem->span.cpu : nullptr;
    if (m.zeta) {  // a depth resolve's or a turn copy's source samples it
        if (!(t.tic = allocTic())) {
            if (!mem->views)
                dropPlain(mem);
            return nullptr;
        }
        writeTic(t);
    }
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
    RAW_DIAGNOSTIC(stats.backings++);
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
    RAW_DIAGNOSTIC(stats.backingBytes -= mem->span.bytes);
    RAW_DIAGNOSTIC(glassMemEvent("memoryless plain memory freed", mem->span.bytes));
    freePlainSpan(*mem);
    plainMems_.erase(std::find_if(plainMems_.begin(), plainMems_.end(), [&](const std::unique_ptr<PlainMem> &p) {
        return p.get() == mem;
    }));
}

void Device::plainFree(Backing &b)
{
    if (b.texture.tic)
        freeTic(b.texture.tic);
    RAW_DIAGNOSTIC(stats.backings--);
    PlainMem *mem = b.plain;
    b.plain = nullptr;
    if (!--mem->views)
        dropPlain(mem);
}

Texture *Device::lastBacking(const Texture &m, uint32_t tag, bool blockLinear, bool compression, bool copied)
{
    const uint64_t epoch = backingEpoch_.load(std::memory_order_seq_cst);
    for (const BackingAsk &ask : asks_) {
        if (ask.epoch != epoch || !ask.same(m, tag, blockLinear, compression, copied, zetaKindsProbed()))
            continue;
        Backing *b = ask.backing;
        b->lastCommit.store(commitCount_, std::memory_order_relaxed);
        b->lastClosed.store(closedCount_, std::memory_order_seq_cst);
        // a trim moves the epoch before it reads the stamps: one that began since reads ours or is seen here
        return backingEpoch_.load(std::memory_order_seq_cst) == epoch ? &b->texture : nullptr;
    }
    return nullptr;
}

Texture *Device::backing(const Texture &m, uint32_t tag, bool blockLinear, bool compression, bool copied)
{
    BackingAsk ask{m.width, m.height,    m.format,    tag,    std::max(m.layers, 1u), m.samples,
                   m.zeta,  blockLinear, compression, copied, zetaKindsProbed()};
    blockLinear |= m.zeta;
    // a layered pass's backing: plain memory of its layers (compressed backings are one layer's)
    const uint32_t kind = compression && m.layers <= 1 ? compKind(m, blockLinear, copied) : 0;
    const CompMem *mem = kind ? comp_.largest(tag, kind) : nullptr;
    Backing *plain = nullptr;
    blockLinear |= m.samples > 1;
    const auto asked = [&](Backing *b) {
        b->lastCommit.store(commitCount_, std::memory_order_relaxed);
        b->lastClosed.store(closedCount_, std::memory_order_relaxed);
        if (tag < lastBackings_.size())
            lastBackings_[tag] = b;
        ask.backing = b;
        ask.epoch = backingEpoch_.load(std::memory_order_relaxed);
        asks_[nextAsk_++ % asks_.size()] = ask;
        return &b->texture;
    };
    const auto matches = [&](const Backing *b) {
        return b && !b->retired && b->texture.width == m.width && b->texture.height == m.height &&
               b->texture.format == m.format && b->tag == tag && b->texture.blockLinear == blockLinear &&
               b->texture.samples == m.samples && b->texture.layers == std::max(m.layers, 1u);
    };
    if (Backing *last = tag < lastBackings_.size() ? lastBackings_[tag] : nullptr;
        matches(last) && backingCompressionMatches(last->comp != nullptr, last->comp, mem, kind != 0))
        return asked(last);
    for (Backing *b : backings_)
        if (matches(b)) {
            if (b->plain && b->plain != largestPlain(tag, b->plain->memory, b->plain->layered)) {
                b->retired = true;  // larger plain memory of its tag took its place: a view of that replaces it
                backingsChanged();
                continue;
            }
            if (backingCompressionMatches(b->comp != nullptr, b->comp, mem, kind != 0))
                return asked(b);
            if (b->comp && kind) {
                b->retired = true;  // larger memory of its tag and kind took its place: a view of that replaces it
                backingsChanged();
            } else if (!b->comp)
                plain = b;
        }
    if (Backing *c = kind ? compBacking(m, tag, kind) : nullptr) {
        backings_.push_back(c);
        return asked(c);
    }
    if (plain)
        return asked(plain);
    Backing *view = plainBacking(m, tag, blockLinear);
    if (!view)
        return nullptr;
    backings_.push_back(view);
    return asked(view);
}

// Backings no pass used for 600 command buffers (~10 s of frames) and whose last use completed go back to the pool; a retired
// one (a compressed backing, or one on larger memory of its tag, replaced it) as soon as its last use completed, as no pass
// takes it again; one on compressed or shared plain memory after 10 s unused too, also while the process commits nothing
// (raw_comp.h backingStale; the compressed-memory thread trims then).
void Device::trimBackings()
{
    // the counts before the backings' stamps: a commit stores its stamps before it moves them
    const uint64_t committed = commits(), closedNow = closedCount_.load(std::memory_order_acquire);
    const uint64_t now = compClock();
    const auto stale = [&](const Backing *b) {
        return backingStale(committed, b->lastCommit.load(std::memory_order_seq_cst), closedNow,
                            b->lastClosed.load(std::memory_order_seq_cst), now,
                            b->usedNs.load(std::memory_order_relaxed), b->retired, b->comp || b->plain) &&
               (!b->submitted.load(std::memory_order_relaxed) || done(b->lastUse.load(std::memory_order_relaxed)));
    };
    const size_t ready = comp_.ready.size();
    compTrim(now);
    for (auto it = spares_.begin(); it != spares_.end();) {
        if (committed - it->freedAt > 600) {
            freeChunk(chunks_[it->chunk]);
            it = spares_.erase(it);
            continue;
        }
        ++it;
    }
    // A pass's stamps (lastBacking, with no lock) only make a backing less stale: where none is stale before the epoch
    // moves none is after, and the passes' kept asks stay good
    if (comp_.ready.size() != ready || std::any_of(backings_.begin(), backings_.end(), stale)) {
        backingsChanged();
        for (auto it = backings_.begin(); it != backings_.end();) {
            Backing *b = *it;
            if (!stale(b)) {
                ++it;
                continue;
            }
            if (b->tag < lastBackings_.size() && lastBackings_[b->tag] == b)
                lastBackings_[b->tag] = nullptr;
            if (b->comp) {
                compFree(*b);
            } else if (b->plain) {
                plainFree(*b);
            }
            trimmedBackings_.push_back(b);
            trimmedPending_.store(true, std::memory_order_release);
            it = backings_.erase(it);
        }
    }
#if SWITCH_RAW_DIAGNOSTICS
    uint64_t retired = 0;
    for (const Backing *b : backings_)
        retired += !b->comp && b->retired;
    stats.retiredBackings = retired;
    uint64_t tiledFree = tiledFreeBytes(blChunks_);
    for (const std::vector<Chunk> &z : zChunks_)
        tiledFree += tiledFreeBytes(z);
    stats.tiledFreeBytes = tiledFree;
#endif
}

// ---- command buffer ----

// A wait the kernel refuses with no kill (the call failed, or the submit thread's reopen destroyed the channel meanwhile) is
// no completion: the channel's status decides. A healthy channel is waited on again; one that keeps refusing is taken as
// killed, with everything it has not completed, so nothing it may still run is released or written over as done.
bool Device::waitFence(uint32_t seqno, uint64_t generation)
{
    constexpr unsigned kRefusals = 3;
    const uint32_t ch = channel_, opened = k_.opened();
    if (done(seqno) && semanticFence(seqno).status == SemanticFenceStatus::Superseded) {
        int result = KERN_SUCCESS;
        if (k_.wait(ch, seqno, ~0ull, &result))
            return !lost(seqno);
        if (result == kIOReturnAborted) {
            keepFailure(seqno);
            return false;
        }
    }
    for (unsigned refused = 0; !done(seqno);) {
        if (channelGeneration() != generation)
            return false;
        if (k_.wait(ch, seqno))
            break;
        if (channelGeneration() != generation)
            return false;
        TegraGM20BAbiStatus st;
        if (k_.lost(ch) || !k_.status(ch, st)) {  // killed, or destroyed by a reopen that kept its kill record
            break;
        }
        if (st.error) {
            k_.markLost(ch, opened);
            break;
        }
        if (done(seqno))
            return !lost(seqno);
        if (++refused == kRefusals) {
            os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                         "raw: the kernel refused %u waits on channel %u's seqno %u (%s): taken as killed", refused, ch,
                         seqno, k_.error().c_str());
            {
                std::lock_guard<raw::Mutex> l(killedLock_);
                killedKept_[killedNext_++ % killedKept_.size()] = {
                    st.completed + 1, __atomic_load_n(&lastSeqno_, __ATOMIC_RELAXED), kWaitRefused};
                keptThrough_.store(__atomic_load_n(&lastSeqno_, __ATOMIC_RELAXED), std::memory_order_relaxed);
                keptKills_.store(true, std::memory_order_release);
            }
            k_.markLost(ch, opened);  // begin() makes it again
            return false;
        }
    }
    return !lost(seqno);
}

// A job the kernel says its own event wait failed, which the fence record no longer names
void Device::keepFailure(uint32_t seqno)
{
    std::lock_guard<raw::Mutex> l(killedLock_);
    killedKept_[killedNext_++ % killedKept_.size()] = {seqno, seqno, kDependencyFailed};
    const uint32_t through = keptThrough_.load(std::memory_order_relaxed);
    if (!keptKills_.load(std::memory_order_relaxed) || int32_t(seqno - through) > 0)
        keptThrough_.store(seqno, std::memory_order_relaxed);
    keptKills_.store(true, std::memory_order_release);
}

// What the live channel failed, for its retirement: the kept records (its kill record among them) and the newest job its
// fence record names failed
std::vector<KillRange> Device::failedSeqnos() const
{
    std::vector<KillRange> failed;
    {
        std::lock_guard<raw::Mutex> l(killedLock_);
        for (const Killed &kept : killedKept_)
            if (kept.error)
                failed.push_back(kept);
    }
    Killed live = {};
    if (!k_.killRecords())  // nothing tells the seqnos the channel finished from those it took with it
        failed.push_back({1, lastSeqno_, kWaitRefused});
    else if (k_.lost(channel_) && !liveKill(live))  // the kernel publishes the error before the record and the fence
        failed.push_back({*fence_ + 1, lastSeqno_, k_.lostError() ? k_.lostError() : kWaitRefused});
    const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence_);
    if (const uint32_t seqno =
            newestSemanticFailure(reinterpret_cast<const volatile TegraGM20BAbiFenceSemantic *>(record + 1)))
        failed.push_back({seqno, seqno, kDependencyFailed});
    return failed;
}

// A kept record names the seqnos of the slot it was written in, which a channel made in another slot gives out again
void Device::forgetKept()
{
    std::lock_guard<raw::Mutex> l(killedLock_);
    killedKept_ = {};
    keptThrough_.store(0, std::memory_order_relaxed);
    keptKills_.store(false, std::memory_order_release);
}

// The barriers order the record after the caller's read of the fence word, and its range after its error: the kernel
// writes the range, the error, then the fence word.
bool Device::liveKill(Killed &k) const
{
    const volatile uint32_t *fence = fence_;
    if (fence == &kNoFence)
        return false;
    if (k_.killRecords()) {
        const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
        std::atomic_thread_fence(std::memory_order_acquire);
        k.error = record->error;
        if (k.error) {
            std::atomic_thread_fence(std::memory_order_acquire);
            k.first = record->killed_first;
            k.last = record->killed_last;
            return true;
        }
    }
    return false;
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
    if (semanticFence(seqno).status == SemanticFenceStatus::Failed) {
        killedError_.store(kDependencyFailed, std::memory_order_relaxed);
        return true;
    }
    if (!keptKills_.load(std::memory_order_acquire) ||
        int32_t(seqno - keptThrough_.load(std::memory_order_relaxed)) > 0)
        return false;
    std::lock_guard<raw::Mutex> l(killedLock_);
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
        for (size_t i = 0; i < chunks.size(); i++) {
            if (chunks[i].bo.handle && in(chunks[i].bo, chunks[i].bo.bytes, what))
                return true;
            if (const uint64_t view = chunks[i].pitchVa; view && va >= view && va - view < chunks[i].bytes) {
                snprintf(line, sizeof line, "%s's kind-0 view +0x%llx", what, (unsigned long long)(va - view));
                return true;
            }
        }
        return false;
    };
    std::lock_guard<raw::Mutex> l(lock_);
    if (in(heap_, heapBound_, "the shader heap") || in(heap_, heap_.bytes, "the shader heap's unbound reservation") ||
        in(push_.bo, push_.bo.bytes, "the push ring") || in(upload_.bo, upload_.bo.bytes, "the upload ring") ||
        in(root_, root_.bytes, "the root table") || in(tic_, tic_.bytes, "the texture headers") ||
        in(tsc_, tsc_.bytes, "the sampler headers") || in(zero_, zero_.bytes, "the zero page") ||
        in(tickets_, tickets_.bytes, "the interlock tickets") ||
        in(colorTable_, colorTable_.bytes, "the colour table") ||
        in(srgbEncode_, srgbEncode_.bytes, "the sRGB encode table") ||
        in(stamps_, stamps_.bytes, "the GPU time stamps") ||
        in(handleTables_, handleTables_.bytes, "the argument buffers' handle tables") ||
        in(argumentRoots_, argumentRoots_.bytes, "the argument buffers' root table") ||
        std::any_of(heapSets_.begin(), heapSets_.end(),
                    [&](const Bo &bo) {
                        return in(bo, bo.bytes, "an argument buffers' heap set");
                    }) ||
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
#if SWITCH_RAW_DIAGNOSTICS
    const uint64_t began = mach_absolute_time();
#endif
    if (channel_) {  // none after a reopen the kernel refused: that channel was retired then
        keptSlot_ = fence_;
        Killed live = {};
        if (liveKill(live)) {
            k_.markLost(channel_, k_.opened());
            std::lock_guard<raw::Mutex> l(killedLock_);
            killedKept_[killedNext_++ % killedKept_.size()] = live;
            keptThrough_.store(live.last, std::memory_order_relaxed);
            keptKills_.store(true, std::memory_order_release);
        }
        contextless_ = true;
        const uint64_t retiring = generations_.retire(failedSeqnos());
        // Closed once: the connection is NVK's, whose next channel may take the slot number
        k_.closeChannel(channel_);
        generations_.closed(retiring);
    }
    if (!makeChannel(k_, kEngines, channel_, fence_, lastSeqno_, &kNoFence, [this](uint32_t channel, uint32_t &seqno) {
            return newContext(channel, seqno);
        })) {
        std::lock_guard<raw::Mutex> l(channelLock_);
        channelError_ = k_.error();
        return false;
    }
    if (fence_ != keptSlot_)
        forgetKept();
    contextless_ = false;
    RAW_DIAGNOSTIC(stats.reopenTicks = mach_absolute_time() - began);
    return true;
}

// A channel made again: the context the dead one took with it, before anything else runs on it
bool Device::newContext(uint32_t channel, uint32_t &seq)
{
    stats.reopens++;
    push_.inflight.clear();
    upload_.inflight.clear();
    {
        std::lock_guard<raw::Mutex> l(lock_);
        for (Backing *b : backings_)
            b->submitted = false;
        heapInvalidated_ = ~0ull;
    }
    slmChannel_ = 0;
    memset(rootPush_, 0, sizeof rootPush_);
    restart_ = 0;
    rtLayer_ = ~0u;
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
    SurfaceList resources;
    if (!member(zero_))
        resources.add(zero_.handle, false);
    if (!member(tickets_))
        resources.add(tickets_.handle, true);
    if (!job(setView_, channel, &seg, 1, resources, JobVisibility::External, seq))
        return false;
    retire(push_, seq);
    return true;
}

// Between command buffers: a ring that made an allocation wait for the GPU doubles (to `most`), the old one freed once the work
// that used it completed, so a client's rings follow what its command buffers use.
void Device::growRing(Ring &r, uint64_t most)
{
    const bool grow = r.waited && r.bo.bytes < most;
    r.waited = false;
    Bo larger;
    if (!grow || !k_.allocControl(std::min(r.bo.bytes * 2, most), larger, ControlUse::Ring))
        return;
    set_.add(k_, larger, &r == &upload_ ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
    grownOut_.push_back({r.bo, lastSeqno_, channelGeneration()});
    r.bo = larger;
    r.head = r.since = r.clear = 0;
    r.inflight.clear();
}

void Device::reapAtBegin()
{
    if (!trimmedPending_.load(std::memory_order_acquire) && !nativeStale_.load(std::memory_order_acquire))
        return;
    RAW_ENCODE_LOCK(l, lock_);
    reapNativeSurfaces();
    nativeStale_.store(std::any_of(nativeSurfaces_.begin(), nativeSurfaces_.end(),
                                   [](const std::unique_ptr<NativeSurface> &s) {
                                       return !s->cached;
                                   }),
                       std::memory_order_relaxed);
    for (Backing *b : trimmedBackings_)
        delete b;
    trimmedBackings_.clear();
    trimmedPending_.store(false, std::memory_order_relaxed);
}

CommandBuffer *Device::begin()
{
    if (!ensureChannel())
        return nullptr;
    Killed live = {};
    if ((contextless_ || k_.lost(channel_) || liveKill(live)) && !reopen())
        return nullptr;
    for (auto it = grownOut_.begin(); it != grownOut_.end();)
        if (it->unused(channelGeneration(), [this](uint32_t seqno) {
                return done(seqno);
            })) {
            k_.free(it->bo);
            it = grownOut_.erase(it);
        } else {
            ++it;
        }
    growRing(push_, kSizes[0].push);
    growRing(upload_, kSizes[0].upload);
#if SWITCH_RAW_DIAGNOSTICS
    if (glassMemOn.load(std::memory_order_relaxed) || glassMemLive_ || glassMemBusyCount_)
        glassMemFrame();
    cb_.glassMemSlot_ = -1;
#endif
    CommandBuffer &cb = cb_;
    cb.dev_ = this;
    cb.segments_.clear();
    cb.batchWork_ = {};
    cb.dirty_.clear();
    cb.backings_.clear();
    cb.heaps_.clear();
    cb.surfaces_.clear();
    set_.view(setView_);
    cb.memberUses_.clear();
    cb.memberRemovals_ = setView_.removals;
    cb.memberHostWritten_ = false;
    cb.named_.clear();
    cb.sharedWaits_.clear();
    cb.sharedSignals_.clear();
    cb.sharedReleases_.clear();
    cb.predication_.endUnit();
    cb.loops_ = {};
    cb.visibility_ = JobVisibility::Legacy;
    cb.externalWrites_ = false;
    cb.publication_ = {};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb.forceWholePrograms_ = cb.inlineStates_ = false;
    cb.splitProbe_ = SplitProbe::Twin;
    cb.splitForced_ = false;
    cb.splitCapturing_ = splitCaptureLeft.load(std::memory_order_relaxed) != 0;
    cb.splitCaptures_.clear();
    cb.foldCounts();
#endif
    cb.draws = cb.passes = cb.blits = cb.bands = 0;
    cb.refusal = {};
    cb.partial = false;
    cb.admissionBlocked = false;
    cb.quietEnd = false;
    cb.promises.clear();
    cb.promisesKept = false;
    cb.nativeError = 0;
    cb.setRefused.clear();
    cb.nativeReads_.clear();
    cb.nativeSurfaces_.clear();
    cb.pitchImports_.clear();
    overflowing_ = false;
    overflow_.clear();
    cb.pushStart_ = push_.head;
    cb.uploadStart_ = upload_.head;
    memcpy(cb.rootPush_, rootPush_, sizeof rootPush_);
    cb.restart_ = restart_;
    cb.rtLayer_ = rtLayer_;
    cb.ds_ = ds_;
    cb.extrasLive_ = extrasLive_;
    cb.heldSegment_ = heldSegment_;
    cb.segmentBinds_ = 0;
    cb.drawn_ = false;
    reapAtBegin();
    // a trim's lock once in 64 command buffers, never waited for: the compressed-memory thread holding it trims too
    if (commits() % 64 == 0) {
        std::unique_lock<raw::Mutex> l(lock_, std::try_to_lock);
        if (l.owns_lock()) {
            RAW_ENCODE_COUNT(Locks);
            trimBackings();
        }
    }
    cb.stamp = stampOf(commits());
    RAW_DIAGNOSTIC(spans_.begin(cb.stamp));
    cb.timings.clear();
    cb.loopsLaid = false;
    // submission prologue, as NVK opens each submission: header/sampler caches (headers written since), constant, shader data and
    // texture caches, and the instruction caches where commit finds the heap grew
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, 64 * 4, 256, va));
    Push p{start, start + 64};
    emitStartStamp(p, stamps_.va, cb.stamp);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    cb.forms_ = nextForms;
    nextForms = {};
    cb.taken_ = {};
    cb.lastEngine_ = ProbeEngine::None;
    if (cb.forms_.entry != EntryForm::Today) {
        cb.shaderInvalidate_ = probeEntry(p, cb.forms_.entry, cb.taken_);
    } else
#endif
    {
        p.m(INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
        p.m(INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
        p.inc(INVALIDATE_SHADER_CACHES_NO_WFI, {kInvalidateConstant});
        cb.shaderInvalidate_ = p.at - 1;
        p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
        p.m(INVALIDATE_SHADER_CACHES_NO_WFI, kInvalidateData);
    }
    checkRoom(p, "a submission prologue");
    cb.segment(start, p.at);
    cb.prologueDwords_ = uint32_t(p.at - start);
    cb.splits_.clear();
    cb.splitRecords_.clear();
    cb.splitReuse_.clear();
    cb.buffersWritten_ = false;
    cb.geometryBudget_ = {};
    cb.geometryPool_.clear();
    cb.geometryRefusedBuffers_.clear();
    cb.geometryRefusedBackings_.clear();
    return &cb;
}

LoopStream CommandBuffer::loopStream(Push &p, uint32_t *&start)
{
    return {p, start, segments_, dev_->push_.bo.cpu, dev_->push_.bo.va};
}

LoopStream BlitEncoder::stream()
{
    return cb_->loopStream(p_, start_);
}

void CommandBuffer::segment(uint32_t *from, uint32_t *to)
{
    if (to == from)
        return;
    const uint64_t va = dev_->push_.bo.va + (reinterpret_cast<uint8_t *>(from) - dev_->push_.bo.cpu);
    segments_.append({va, uint32_t(to - from), 0});
}

void CommandBuffer::timestamps(const counters::Stamp *stamps, size_t count)
{
    if (!count)
        return;
    const uint32_t words = uint32_t(count) * counters::kTimestampWords;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, words * 4, 256, va));
    Push p{start, start + words};
    for (size_t i = 0; i < count; i++) {
        useAddress(stamps[i].va, counters::kRecordBytes, true);
        counters::emit(p, stamps[i]);
    }
    checkRoom(p, "counter samples");
    segment(start, p.at);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p.at) - dev_->push_.bo.cpu);
}

void CommandBuffer::clean(const void *cpu, uint64_t bytes, bool invalidate)
{
    dirty_.add(cpu, bytes, invalidate);
}

RenderEncoder *CommandBuffer::render(const PassDesc &pass)
{
    RAW_ENCODE_SPAN(PassBegin);
    RenderEncoder &e = render_;
    // a pass's lists keep the last one's capacity: no allocation a steady frame
    std::vector<RenderEncoder::Body> bodies = std::move(e.bodies_);
    std::vector<std::array<uint32_t, 4>> rects = std::move(e.rects_);
    SplitPassPlan splitPlan = std::move(e.splitPlan_);
    std::vector<RenderEncoder::Clear> clears = std::move(e.initialClears_);
    std::vector<bool> tiles = std::move(e.tiles_);
    std::vector<GeometryAnalysis> written = e.written_.release();
    bodies.clear();
    rects.clear();
    splitPlan.clear();
    clears.clear();
    // made again where it is, not built aside and moved in (the encoder is 7 KB)
    e.~RenderEncoder();
    new (&e) RenderEncoder();
    e.bodies_ = std::move(bodies);
    e.rects_ = std::move(rects);
    e.splitPlan_ = std::move(splitPlan);
    e.initialClears_ = std::move(clears);
    e.tiles_ = std::move(tiles);
    e.written_.adopt(std::move(written));
    e.cb_ = this;
    e.dev_ = dev_;
    e.pass_ = pass;
    RAW_DIAGNOSTIC(e.prologueMark_ = dev_->spans_.reserve());
    // the render hardware takes one memory layout for all of a pass's color targets (nvk_rendering_linear): block-linear when
    // one of them is, or is a pitch target the hardware cannot take (rendered through a shadow either way), or beside a zeta,
    // its pitch ones then rendered through block-linear shadows as NVK does, or when every one is memoryless; memoryless ones
    // get a backing of the pass's layout
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
    if (passRotation(pass, e.rot_) != RotatedRefusal::None || layeredRefusal(pass, e.samples_, e.rot_.turned()))
        return nullptr;
    const uint32_t layers = e.pass_.layers = std::max(pass.layers, 1u);
    const auto layered = [layers](Texture shape) {
        shape.layers = layers;
        return shape;
    };
    // a turned pass's attachment that is not turned is transient (passRotation): a backing laid out as the plane stands in
    const auto backed = [&](const Texture &t) {
        return t.memoryless || standsInTurned(t, e.rot_);
    };
    const bool hidden = !zeta && pass.hiddenZeta;
    bool blockLinear = zeta != nullptr || hidden || layers > 1, pitchTarget = false;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture; t && !backed(*t)) {
            if (t->blockLinear || t->shadowTarget || (pass.arrayedStorageMask >> j & 1))
                blockLinear = true;
            else
                pitchTarget = true;
        }
    blockLinear |= !pitchTarget;
    // the device's backings: those the last passes asked the same of with no lock, else under the device's lock once a
    // pass however many attachments need one (the compression thread trims them under it)
    std::unique_lock<raw::Mutex> backingLock(dev_->lock_, std::defer_lock);
    const auto backing = [&](const Texture &shape, uint32_t tag, bool linear, bool compression, bool copied = false) {
        if (Texture *b = backingLock.owns_lock() ? nullptr : dev_->lastBacking(shape, tag, linear, compression, copied))
            return b;
        if (!backingLock.owns_lock()) {
            backingLock.lock();
            RAW_ENCODE_COUNT(Locks);
        }
        return dev_->backing(shape, tag, linear, compression, copied);
    };
    if (zeta && backed(*zeta)) {
        Texture *b = backing(layered(planeShaped(*zeta, e.rot_)), 0, false, true);
        if (!b)
            return nullptr;
        e.pass_.depthStencil.texture = b;
        backings_.push_back(b);
    }
    uint8_t scratchMask = 0;
    for (uint32_t j = 0; j < pass.count && j < 8; ++j)
        if (const Texture *t = pass.color[j].texture;
            t && (backed(*t) || t->shadowTarget || (blockLinear && !t->blockLinear)))
            scratchMask |= uint8_t(1u << j);
    const auto scratchTags = transientColorTags(scratchMask);
    for (uint32_t j = 0; j < pass.count && j < 8; j++) {
        const Texture *t = pass.color[j].texture;
        if (!t)
            continue;
        if (!t->renderable && !t->shadowTarget)
            return nullptr;
        const bool mixed = blockLinear && !t->blockLinear && !backed(*t);
        if ((t->shadowTarget && !backed(*t)) ||
            mixed) {  // rendered through a pool image (end(): copied in before, back after)
            RAW_DIAGNOSTIC(dev_->stats.xrPlaneShadows += t->xrPlane != 0);
            if (mixed) {
                RAW_DIAGNOSTIC(dev_->stats.rtMixed++);
            } else {
                RAW_DIAGNOSTIC(dev_->stats.rtUnaligned++);
                RAW_DIAGNOSTIC(dev_->stats.rtUnalignedPitch = t->pitch);
                RAW_DIAGNOSTIC(dev_->stats.rtUnalignedVa = uint32_t(t->va & 255));
            }
            Texture *b = backing(*t, scratchTags[j], blockLinear, !(pass.storageMask >> j & 1), true);
            if (!b || !b->renderable)
                return nullptr;
            e.pass_.color[j].texture = b;
            e.shadowOf_[j] = t;
            backings_.push_back(b);
            continue;
        }
        if (backed(*t)) {  // back it for this pass
            Texture *b = backing(layered(planeShaped(*t, e.rot_)), scratchTags[j], blockLinear,
                                 layers == 1 && (dev_->storageCompressedOk() || !(pass.storageMask >> j & 1)));
            if (!b)
                return nullptr;
            e.pass_.color[j].texture = b;
            e.memoryless_[j] = true;
            backings_.push_back(b);
        }
    }
    if (hidden) {
        Texture *b = backing(e.hiddenZetaShape(pass), 0, false, true);
        if (!b)
            return nullptr;
        e.pass_.depthStencil = {b, true, true, 1.0f, 0};
        e.hiddenZeta_ = true;
        backings_.push_back(b);
    }
    if (backingLock.owns_lock())
        backingLock.unlock();
    for (uint32_t j = 0; j < e.pass_.count && j < 8; ++j)
        if (const Texture *texture = e.pass_.color[j].texture)
            use(*texture, true);
    if (const Texture *texture = e.pass_.depthStencil.texture)
        use(*texture, true);
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
    if (e.rot_.turned())
        frameArea(pass, e.rot_, e.areaW_, e.areaH_);
    if (pass.width)
        e.areaW_ = std::min(e.areaW_, pass.width);
    if (pass.height)
        e.areaH_ = std::min(e.areaH_, pass.height);
    // (raw_zcull.h's region is one plane's: an array's zeta, or a layer of one, keeps zcull off)
    e.zcull_.candidate = dev_->zcullOn_ && zeta && zetaHasDepth(zeta->format) && e.samples_ == 1 &&
                         pass.depthStencil.clearDepth && layers == 1 && zeta->layers <= 1 &&
                         (!zeta->storage.va || zeta->va == zeta->storage.va);
    // DontCare can become a clear after later draws establish readBits_. Treat its deferred prologue write like Clear.
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (e.pass_.color[j].texture && e.pass_.color[j].load != ColorAttachment::Load)
            e.attachmentWrites_.rop |= uint8_t(1u << j);
    if (e.attachmentWrites_.rop) {
        DrawGeometry cleared;
        cleared.limit = {0, 0, e.areaW_, e.areaH_};
        e.written_.add(cleared);
    }
    // the body goes straight into the push ring; the pass prologue (targets, clears) is written at end() and runs first
    e.reserve();
    e.setViewport(0, 0, e.areaW_, e.areaH_, 0, 1);
    e.setScissor(0, 0, e.areaW_, e.areaH_);
    e.extrasAtBegin_ = extrasLive_;
    if (extrasLive_) {
        heldSegment_.lose();
        RAW_DIAGNOSTIC(dev_->stats.segmentExtrasLosses++);
    } else {
        heldSegment_.passBoundary();
    }
    extrasLive_ = nullptr;
    passes++;
    return &e;
}

void CommandBuffer::releaseEvent(uint64_t va, uint32_t payload)
{
    uint64_t pushVa;
    auto *words = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, kEventReleaseWords * 4, 4, pushVa));
    Push p{words, words + kEventReleaseWords};
    // a kernel without addressed jobs publishes DRAM at every job end, so its mid-job releases do as well
    const bool publish = publication_.take() || !dev_->k_.exactJobs();
    emitEventRelease(p, va, payload, publish);
    segment(words, p.at);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (!publish)
        dev_->unflushed_.fetch_add(1, std::memory_order_relaxed);
#endif
}

BlitEncoder *CommandBuffer::blit()
{
    BlitEncoder &e = blit_;
    e = BlitEncoder();
    heldSegment_.lose();
    e.cb_ = this;
    e.dev_ = dev_;
    uint64_t va;
    e.start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    e.p_ = Push{e.start_, e.start_ + (1 << 12)};
    loops_.openSegment(e.stream());
    // what the render work before wrote is complete before the copy engine reads it (NVK: a WFI on the 3D subchannel; the
    // switch to the copy subchannel then waits for the 3D engine anyway)
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    probeBlitOpening(e.p_, forms_, lastEngine_, taken_);
    lastEngine_ = ProbeEngine::Copy;
#else
    e.p_.m(WAIT_FOR_IDLE, 0);
#endif
    blits++;
    return &e;
}

Texture::StorageRange Texture::storageRange() const
{
    if (storage.bytes)
        return storage;
    if (span.bytes)
        return {span.va, span.bytes, false};
    if (chain)
        return {chain->va, chain->layout.bytes, false};
    if ((blockLinear || nativeGeneration) && layerBytes && layers && layerBytes <= UINT64_MAX / layers)
        return {va, layerBytes * layers, false};
    return {};
}

static uint64_t rowStorageBytes(uint32_t pitch, uint32_t width, uint32_t height, uint32_t element)
{
    if (!width || !height || !element)
        return 0;
    const uint64_t rows = uint64_t(pitch) * (height - 1), last = uint64_t(width) * element;
    return last <= UINT64_MAX - rows ? rows + last : 0;
}

uint64_t CommandBuffer::batchLimit() const
{
    return dev_->role() == Device::Role::RenderServer ? BatchWork::kRenderServerLimit : BatchWork::kLimit;
}

// Read ownership remains held until every consuming GPU job is published. Releasing after the CPU copy alone
// would allow a producer to replace the vertices between the snapshot and this draw's GPU read.
bool CommandBuffer::acquireGeometry(const Buffer &buffer, const uint8_t *cpu, uint64_t bytes)
{
    Device &d = *dev_;
    if (!cpu || !buffer.cpu || uintptr_t(cpu) < uintptr_t(buffer.cpu) || !bytes)
        return false;
    const uint64_t delta = uintptr_t(cpu) - uintptr_t(buffer.cpu);
    const uint64_t extent = (buffer.length + 3) & ~uint64_t(3);
    if (delta > extent || bytes > extent - delta || delta > UINT64_MAX - buffer.va)
        return false;
    const uint64_t address = buffer.va + delta;
    // Imported cached buffers may have producers outside this frontend's completion/cache contract. Until their
    // transitions are attested, use conservative GPU ordering rather than discarding or cleaning unqualified lines.
    if (!buffer.wc && (buffer.import || !(d.k_.info().flags & TegraGM20BInfoCpuPreserveDirty)))
        return false;
    const uint32_t access =
        TegraGM20BCpuRead | TegraGM20BCpuAvoidSync | (buffer.wc ? 0u : uint32_t(TegraGM20BCpuPreserveDirty));
    for (const auto &read : geometryReads_) {
        const auto &bo = read.bo;
        const uint64_t base = bo.vaBase ? bo.vaBase : bo.va;
        const uint64_t start = base + read.offset;
        if (address >= start && address - start < read.bytes && bytes <= read.bytes - (address - start))
            return !surfaces_.writesBacking(bo.handle, base, bo.bytes);
    }
    if (std::find(geometryRefusedBuffers_.begin(), geometryRefusedBuffers_.end(), &buffer) !=
        geometryRefusedBuffers_.end())
        return false;
    RAW_ENCODE_SPAN(GeometryAcquire);
    RAW_ENCODE_LOCK(lock, d.lock_);
    Bo bo;
    if (buffer.import) {
        bo = buffer.import->bo;
    } else if (buffer.span.valid()) {
        const auto index = buffer.span.chunk;
        if (buffer.arena) {
            if (index >= d.arenas_.size() || d.arenas_[index].purged)
                return false;
            if (d.arenas_[index].bufferVa != d.arenas_[index].va)
                return false;  // Private placement aliases can also be written through their tiled VA.
            bo = d.arenas_[index].bo;
        } else {
            const auto &chunks = buffer.wc ? d.wcChunks_ : d.chunks_;
            if (index >= chunks.size())
                return false;
            bo = chunks[index].bo;
        }
    }
    const uint64_t base = bo.vaBase ? bo.vaBase : bo.va;
    if (!bo.handle || address < base || address - base > bo.bytes || bytes > bo.bytes - (address - base) ||
        surfaces_.writesBacking(bo.handle, base, bo.bytes))
        return false;
    if (std::find(geometryRefusedBackings_.begin(), geometryRefusedBackings_.end(), bo.handle) !=
        geometryRefusedBackings_.end()) {
        geometryRefusedBuffers_.push_back(&buffer);
        return false;
    }
    // Small page windows amortize adjacent matrix/vertex/index views while bounding cache-maintenance work.
    // Ownership remains whole-backing; only visibility maintenance is narrowed to these verified physical bytes.
    constexpr uint64_t page = 4096;
    const uint64_t offset = (address - base) & ~(page - 1);
    const uint64_t end = std::min(bo.bytes, (address - base + bytes + page - 1) & ~(page - 1));
    const uint64_t maintained = end - offset;
    if (geometryReads_.size() >= 64 || maintained > geometryBudget_.maintainedBytes)
        return false;
    if (!d.k_.cpuAccess(bo, offset, maintained, access, true)) {
        geometryRefusedBuffers_.push_back(&buffer);
        geometryRefusedBackings_.push_back(bo.handle);
        return false;
    }
    geometryBudget_.maintainedBytes -= maintained;
    geometryReads_.push_back({bo, offset, maintained, access});
    return true;
}

bool CommandBuffer::geometryWritable() const
{
    for (const auto &read : geometryReads_) {
        const auto &bo = read.bo;
        if (surfaces_.writesBacking(bo.handle, bo.vaBase ? bo.vaBase : bo.va, bo.bytes))
            return true;
    }
    return false;
}

void CommandBuffer::releaseGeometry()
{
    if (geometryReads_.empty())
        return;
    RAW_ENCODE_SPAN(GeometryRelease);
    RAW_ENCODE_LOCK(lock, dev_->lock_);
    for (const auto &read : geometryReads_)
        if (!dev_->k_.cpuAccess(read.bo, read.offset, read.bytes, read.access, false))
            os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                         "raw: geometry CPU read release failed for BO %llu", (unsigned long long)read.bo.handle);
    geometryReads_.clear();
}

std::shared_ptr<ResourceReadyCompletion> CommandBuffer::resourceReadiness(std::function<void()> ready, int &result)
{
    auto events = dev_->k_.sharedEvents();
    result = kIOReturnUnsupported;
    return events ? events->resources(surfaces_.uses(), std::move(ready), result) : nullptr;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void CommandBuffer::foldCounts()
{
    Device::Stats &stats = dev_->stats;
    stats.uploadBytes += counts_.uploadBytes;
    stats.segmentBinds += counts_.segmentBinds;
    stats.segmentWords += counts_.segmentWords;
    stats.segmentSkips += counts_.segmentSkips;
    stats.blendKillBinds += counts_.blendKillBinds;
    stats.interlockDraws += counts_.interlockDraws;
    stats.fetch.interlocked += counts_.interlocked;
    stats.fetch.interlockedBlends += counts_.interlockedBlends;
    counts_ = {};
}
#endif

bool CommandBuffer::commit(uint32_t &seqno)
{
    RAW_ENCODE_SPAN(Commit);
    RAW_ENCODE_COUNT(GeometryVertices, GeometryBudget().vertices - geometryBudget_.vertices);
    if (nativeError)
        return false;

    struct ReleaseReads {
        CommandBuffer &cb;

        ~ReleaseReads()
        {
            cb.releaseGeometry();
        }
    } releaseReads{*this};

    Device &d = *dev_;
    RAW_DIAGNOSTIC(foldCounts());
    // a commit that fails after some of its jobs ran leaves the channel holding a segment the device cannot name
    d.heldSegment_.lose();
    launchPrologues();
    if (d.overflowing_) {
        os_log_error(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
                     "raw: a command buffer larger than its ring, with no memory for a larger one: not submitted");
        return false;
    }
    bool invalidates = false, published = false;
    uint64_t invalidatedBefore = 0;
    bool tablesInvalidate = false;
    uint64_t tablesBefore = 0;

    struct RollbackInvalidation {
        Device &device;
        bool &invalidates, &published;
        uint64_t &before;
        bool &tables;
        uint64_t &tablesBefore;

        ~RollbackInvalidation()
        {
            if (invalidates && !published)
                device.heapInvalidated_ = before;
            if (tables && !published)
                device.tablesInvalidated_ = tablesBefore;
        }
    } rollback{d, invalidates, published, invalidatedBefore, tablesInvalidate, tablesBefore};

    uint64_t slmGiven = 0, slmVa = 0;
    const uint64_t placed = d.heapEpoch_.load(std::memory_order_acquire);
    if (shaderInvalidate_ && placed != d.heapInvalidated_) {
        *shaderInvalidate_ |= kInvalidateInstruction;
        invalidates = true;
        invalidatedBefore = d.heapInvalidated_;
        d.heapInvalidated_ = placed;
    }
    if (const uint64_t perWarp = d.slmPerWarp_.load(std::memory_order_acquire); perWarp != d.slmChannel_) {
        slmGiven = perWarp;
        slmVa = d.slmVa_.load(std::memory_order_relaxed);
    }
    // ahead of everything in it: a program placed after begin() may ask for more than the channel has
    if (slmGiven && !segments_.empty()) {
        uint64_t va;
        auto *head = reinterpret_cast<uint32_t *>(d.alloc(d.push_, kSlmWords * 4, 4, va));
        Push p{head, head + kSlmWords};
        emitSlm(p, slmVa, slmArea(slmGiven, d.k_.info().tpc_count));
        segments_.insert(0, {va, uint32_t(p.at - head), 0});
    }
#if SWITCH_RAW_DIAGNOSTICS
    if (!segments_.empty() && d.glassMemLive_)
        glassMemTail();
#endif
    if (!segments_.empty()) {
        uint64_t va;
        auto *tail = reinterpret_cast<uint32_t *>(d.alloc(d.push_, (kTiledCacheFlushWords + kStampWords) * 4, 4, va));
        Push p{tail, tail + kTiledCacheFlushWords + kStampWords};
        if (tiledCache(d.k_.info()))
            p.at = tiledCacheFlush(p.at);
        emitEndStamp(p, d.stamps_.va, stamp);
        segment(tail, p.at);
    }
    for (const auto &range : dirty_.merged()) {
#if defined(__aarch64__)
        if (range.invalidate) {
            for (uintptr_t address = range.begin; address < range.end; address += 64)
                __asm__ volatile("dc civac, %0" ::"r"(address) : "memory");
        } else {
            for (uintptr_t address = range.begin; address < range.end; address += 64)
                __asm__ volatile("dc cvac, %0" ::"r"(address) : "memory");
        }
#endif
        RAW_DIAGNOSTIC(d.stats.cleanBytes += range.end - range.begin);
    }
#if defined(__aarch64__)
    // The cleans and WC ring writes complete before the kernel rings GP_PUT.
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    // A loop's copies end with the GPU's writes into the push ring (raw_loop_copies.h): the ring is a member the set
    // otherwise names read-only
    if (loopsLaid) {
        useAddress(d.push_.bo.va, d.push_.bo.bytes, true);
        for (const Bo &bo : d.push_.outgrown)
            useAddress(bo.va, bo.bytes, true);
    }
    if (d.k_.exactJobs()) {
        // the handle tables and heap sets the CPU wrote since the last submission that invalidated the L2 for them, set
        // members the job's flag names host-written
        const uint64_t written = d.tablesWritten_.load(std::memory_order_acquire);
        if (written != d.tablesInvalidated_) {
            tablesInvalidate = true;
            tablesBefore = d.tablesInvalidated_;
            d.tablesInvalidated_ = written;
        }
        if (!heaps_.empty()) {
            RAW_ENCODE_LOCK(lock, d.lock_);
            nameHeaps();
        }
    }
    if (geometryWritable())
        return false;  // later internal writes invalidate the CPU proof; no prefix may reach the GPU
    if (!batchWork_.operations)
        segments_.includeTail();
    const uint32_t maxPushes = d.k_.maxPushes(), maxFences = d.k_.maxFences();
    if (!segments_.valid(maxPushes))
        return false;  // validate all indivisible continuation groups before publishing any prefix
    // A command buffer of several jobs holds its shared uses' CPU-lock admission from its first job to its last
    const bool multiple = (!segments_.empty() && segments_.submission(0, maxPushes, maxFences) < segments_.size()) ||
                          sharedWaits_.size() + sharedSignals_.size() > TegraGM20BMaxJobEvents;
    const uint64_t admission = multiple ? d.k_.nextAdmission() : 0;
    bool admissionHeld = false;
    uint32_t seq = 0;
    auto submit = [&](const TegraGM20BAbiPush *pushes, uint32_t count, SurfaceList &resources, JobVisibility visibility,
                      const JobEvents &events, bool last) {
        int result = KERN_SUCCESS;
        SurfaceList &uses = admission ? surfaces_ : resources;
        const uint32_t admissionFlags =
            admission ? (published ? uint32_t(TegraGM20BJobAdmissionContinue) : uint32_t(TegraGM20BJobAdmissionBegin)) |
                            (last ? uint32_t(TegraGM20BJobAdmissionEnd) : 0u)
                      : 0u;
        RAW_DIAGNOSTIC(noteExecUses(uses.uses().size()));
        RAW_DIAGNOSTIC(noteExecPushes(count));
        RAW_ENCODE_BEGIN(execSpan, Exec);
        const bool executed =
            d.job(d.setView_, d.channel_, pushes, count, uses, visibility, seq, &result, events, &memberUses_,
                  memberRemovals_, invalidates || memberHostWritten_ || tablesInvalidate, admission, admissionFlags,
                  !last || quietEnd, last && !promises.empty() ? &promises : nullptr);
        RAW_ENCODE_END(execSpan);
        if (!executed) {
            admissionBlocked = !published && result == kIOReturnCannotLock;
            setRefused = d.setView_.refused;
            if (admissionHeld) {
                SurfaceList none;
                uint32_t cancelled = 0;
                (void)d.job(d.setView_, d.channel_, nullptr, 0, none, JobVisibility::Internal, cancelled, nullptr, {},
                            nullptr, 0, false, admission, TegraGM20BJobAdmissionCancel);
                admissionHeld = false;
            }
            if ((!nativeSurfaces_.empty() || !sharedWaits_.empty() || !sharedSignals_.empty()) &&
                (result == kIOReturnNotReady || result == kIOReturnAborted))
                nativeError = result;
            if (published) {
                partial = true;
                d.wait(d.lastSeqno_);
                d.contextless_ = true;
            }
            return false;
        }
        published = true;
        promisesKept |= last && !promises.empty();
        admissionHeld = admission && !last;
        d.lastSeqno_ = seq;
        return true;
    };
    SurfaceList eventResources;
    size_t waited = 0, signaled = 0;
    // Capacity is an ABI bound, not permission to truncate dependencies. Prefix joins use the same channel's order.
    while (sharedWaits_.size() - waited + std::min<size_t>(sharedSignals_.size(), TegraGM20BMaxJobEvents) >
           TegraGM20BMaxJobEvents) {
        const uint32_t count = uint32_t(std::min<size_t>(sharedWaits_.size() - waited, TegraGM20BMaxJobEvents));
        const JobEvents events{sharedWaits_.data() + waited, nullptr, count, 0};
        if (!submit(nullptr, 0, eventResources, JobVisibility::Internal, events, false))
            return false;
        waited += count;
    }
    const bool pushed = segments_.submit(maxPushes, maxFences, [&](size_t at, size_t n) {
        const bool closing = at + n == segments_.size();
        const JobVisibility visibility = visibility_ == JobVisibility::Legacy ? JobVisibility::Legacy
                                         : externalWrites_ && closing         ? JobVisibility::External
                                                                              : JobVisibility::Internal;
        const uint32_t waitCount = uint32_t(sharedWaits_.size() - waited);
        const uint32_t signalCount =
            closing ? uint32_t(std::min<size_t>(sharedSignals_.size(), TegraGM20BMaxJobEvents - waitCount)) : 0;
        const JobEvents events{waitCount ? sharedWaits_.data() + waited : nullptr,
                               signalCount ? sharedSignals_.data() : nullptr, waitCount, signalCount,
                               signalCount ? sharedReleases_.data() : nullptr};
        const bool last =
            closing && waited + waitCount == sharedWaits_.size() && signaled + signalCount == sharedSignals_.size();
        if (!submit(segments_.data() + at, uint32_t(n), surfaces_, visibility, events, last))
            return false;
        waited += waitCount;
        signaled += signalCount;
        for (NativeSurface *surface : nativeSurfaces_) {
            surface->lastUse = seq;
            surface->channelGeneration = d.channelGeneration();
        }
        return true;
    });
    if (!pushed)
        return false;
    while (waited < sharedWaits_.size() || signaled < sharedSignals_.size()) {
        const uint32_t waitCount = uint32_t(std::min<size_t>(sharedWaits_.size() - waited, TegraGM20BMaxJobEvents));
        const uint32_t signalCount =
            uint32_t(std::min<size_t>(sharedSignals_.size() - signaled, TegraGM20BMaxJobEvents - waitCount));
        const JobEvents events{waitCount ? sharedWaits_.data() + waited : nullptr,
                               signalCount ? sharedSignals_.data() + signaled : nullptr, waitCount, signalCount,
                               signalCount ? sharedReleases_.data() + signaled : nullptr};
        const bool last = waited + waitCount == sharedWaits_.size() && signaled + signalCount == sharedSignals_.size();
        if (!submit(nullptr, 0, eventResources, JobVisibility::External, events, last))
            return false;
        waited += waitCount;
        signaled += signalCount;
    }
    seqno = d.lastSeqno_;
#if SWITCH_RAW_DIAGNOSTICS
    if (glassMemSlot_ >= 0) {
        d.glassMemCbs_[glassMemSlot_].seqno = seqno;
        d.glassMemBusy_[glassMemSlot_] = true;
        d.glassMemBusyCount_++;
        glassMemSlot_ = -1;
    }
#endif
    if (slmGiven)
        d.slmChannel_ = slmGiven;
    d.retire(d.push_, seq);
    d.retire(d.upload_, seq);
    memcpy(d.rootPush_, rootPush_, sizeof rootPush_);
    d.restart_ = restart_;
    d.rtLayer_ = rtLayer_;
    d.ds_ = ds_;
    d.extrasLive_ = extrasLive_;
    d.heldSegment_ = heldSegment_;
    const uint64_t now = compClock();
    for (Texture *b : backings_) {
        Device::Backing &k = Device::backingOf(*b);
        k.lastUse.store(seq, std::memory_order_relaxed);
        k.submitted.store(true, std::memory_order_relaxed);
        k.usedNs.store(now, std::memory_order_relaxed);
    }
    d.commitCount_.fetch_add(1, std::memory_order_relaxed);
    d.closedCount_.fetch_add(1, std::memory_order_release);
    d.noteCommittedTimings(timings);
    releaseNativeReads();
    RAW_DIAGNOSTIC(d.stats.commits++);
    return true;
}

uint64_t Device::loopRecordVa(uint32_t stamp) const
{
    return stamps_.va + loopRecordOffset(stamp);
}

LoopContinue Device::loopContinue(uint32_t stamp) const
{
    if (!stamp || !stamps_.cpu)
        return {};
    const auto *record = reinterpret_cast<const volatile uint32_t *>(stamps_.cpu + loopRecordOffset(stamp));
    return raw::loopContinue(stamp, record[0], record[1]);
}

bool Device::gpuNanoseconds(uint32_t stamp, uint64_t &ns) const
{
    if (!stamp || !stamps_.cpu)
        return false;
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    return stampedNanoseconds(*slot, stamp, ns);
}

#if SWITCH_RAW_DIAGNOSTICS
std::vector<StateSpan> Device::longSpans(uint32_t stamp)
{
    if (!spans_.on() || !stamp)
        return {};
    const auto *slot = reinterpret_cast<const volatile StampSlot *>(stamps_.cpu + stampOffset(stamp));
    if (slot->start.payload != stamp || slot->end.payload != stamp)
        return {};
    std::vector<StateSpan> spans = spans_.take(stamp, slot->start.ptimer, slot->end.ptimer);
    spans.erase(std::remove_if(spans.begin(), spans.end(),
                               [](const StateSpan &s) {
                                   return s.ns < kLongSpanNs;
                               }),
                spans.end());
    return spans;
}

void RenderEncoder::markSpan(Push &p, uint32_t index, SpanKind kind)
{
    const Texture *t = pass_.color[0].texture;
    const uint32_t kpx = t ? uint32_t(uint64_t(t->width) * t->height * t->samples / 1000) : 0;
    SpanLabel label;
    if (kind == SpanKind::Draws && emitted_)
        label = drawSpanLabel(*emitted_, emitted_->rec ? emitted_->rec->keyHash : 0, kpx, samples_);
    label.kind = kind;
    label.kpx = uint16_t(std::min<uint32_t>(kpx, 0xffff));
    label.samples = samples_;
    dev_->spans_.markAt(p, index, label);
}
#endif

void CommandBuffer::abandon()
{
    releaseGeometry();
    releaseNativeReads();
    Device &d = *dev_;
    d.closedCount_.fetch_add(1, std::memory_order_release);
    d.overflowing_ = false;
    d.overflow_.clear();
    for (Device::Ring *r : {&d.push_, &d.upload_}) {
        // drop the open command buffer's spans and give back what it took (nothing else was allocated since: one encodes)
        for (auto it = r->inflight.begin(); it != r->inflight.end();)
            it = it->submitted ? std::next(it) : r->inflight.erase(it);
        r->head = r->since = (r == &d.push_ ? pushStart_ : uploadStart_);
        if (!r->outgrown.empty()) {  // it began in a ring it grew out of: the new one is empty
            for (const Bo &b : r->outgrown)
                d.grownOut_.push_back({b, d.lastSeqno_, d.channelGeneration()});
            r->outgrown.clear();
            r->head = r->since = 0;
        }
    }
    segments_.clear();
    dirty_.clear();
    surfaces_.clear();
    named_.clear();
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

void RenderEncoder::finishDraw(uint32_t vertices, uint32_t instances, bool indirect)
{
    const Pipeline &pl = *emitted_;
#if SWITCH_RAW_DIAGNOSTICS
    if (dev_->spans_.on()) {
        if (p_.room() < kSpanMarkWords)
            reserve();
        markSpan(p_, dev_->spans_.reserve(), SpanKind::Draws);
    }
#endif
    const uint64_t vertexWork = BatchWork::product(BatchWork::product(vertices, instances), pl.vertexInstructions);
    const uint64_t fragmentWork = BatchWork::product(BatchWork::product(drawPixels_, instances),
                                                     uint64_t(pl.fragmentInstructions) * pl.samplePasses);
    if (cb_->batchWork_.add(indirect ? BatchWork::kSaturated : vertexWork + fragmentWork, cb_->batchLimit()))
        cutBatch();
}

void RenderEncoder::cutBatch()
{
    if (p_.at > bodyStart_) {
        bodies_.push_back({bodyStart_, p_.at, 0, 0, false, true});
        bodyStart_ = p_.at;
    } else if (!bodies_.empty()) {
        bodies_.back().batchEnd = true;  // complete indirect argument continuation, never its method header
    }
    cb_->batchWork_ = {};
}

bool RenderEncoder::drawnWhole(const Pipeline &pl) const
{
    return amplifies() || (pl.viewportIndex && views_.viewports > 1);
}

void RenderEncoder::wholeDraw(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                              uint32_t baseVertex, uint32_t baseInstance)
{
    if (amplifies()) {
        amplifyCall(pl, indexed, first, count, instances, baseVertex, baseInstance);
        return;
    }
    if (p_.room() < kInstanceWords)
        reserve();
    mme::draw(p_, params_, indexed, first, count, instances, baseVertex, baseInstance);
}

void RenderEncoder::setBytes(int stage, uint32_t slot, const void *bytes, uint32_t length)
{
    RAW_ENCODE_SPAN(Uniforms);
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    uint64_t va;
    const uint32_t chunk = (length + kUboChunk - 1) & ~(kUboChunk - 1);
    uint8_t *m = dev_->alloc(dev_->upload_, chunk ? chunk : kUboChunk, kUboChunk, va);
    memcpy(m, bytes, length);
    memset(m + length, 0, (chunk ? chunk : kUboChunk) - length);
    bufs_[stage][slot] = {va, length, nullptr, 0,
                          m};  // m2v binds the Metal length (its upload chunks are 256-byte aligned)
    bufs_[stage][slot].resourceVa = dev_->upload_.bo.va;
    bufs_[stage][slot].named = true;
    bufferMoved(stage, slot);
    RAW_DIAGNOSTIC(cb_->counts_.uploadBytes += chunk);
}

void RenderEncoder::setBytesAt(int stage, uint32_t slot, uint64_t va, uint32_t length, const uint8_t *cpu,
                               const Buffer *allocation)
{
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    const Bound &old = bufs_[stage][slot];
    const uint64_t resourceVa = allocation ? allocation->va : va;
    if (old.va == va && old.size == length && !old.buffer && old.cpu == cpu && old.resourceVa == resourceVa)
        return;
    bufs_[stage][slot] = {va, length, nullptr, 0, cpu, resourceVa, allocation != nullptr};
    if (allocation) {
        cb_->use(*allocation, false);
        if (allocation->hostVisible && !allocation->wc)
            cb_->clean(cpu, length);
    }
    bufferMoved(stage, slot);
}

void RenderEncoder::setBuffer(int stage, uint32_t slot, const Buffer *b, uint64_t offset)
{
    if (slot >= 31 || stage < 1 || stage > 2)
        return;
    const Bound next =
        b ? Bound{b->va + offset, uint32_t(std::min<uint64_t>(b->length - std::min(offset, b->length), 0xffffffffu)), b,
                  offset, b->cpu && offset < b->length ? b->cpu + offset : nullptr}
          : Bound{};
    const Bound &old = bufs_[stage][slot];
    if (old.va == next.va && old.size == next.size && old.buffer == next.buffer && old.offset == next.offset &&
        old.cpu == next.cpu)
        return;
    bufs_[stage][slot] = next;
    bufferMoved(stage, slot);
}

void RenderEncoder::setBufferOffset(int stage, uint32_t slot, uint64_t offset)
{
    if (slot >= 31 || stage < 1 || stage > 2 || !bufs_[stage][slot].buffer)
        return;
    setBuffer(stage, slot, bufs_[stage][slot].buffer, offset);
}

void RenderEncoder::setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (reachStep_) {
        reachScissor_[0] = x, reachScissor_[1] = y, reachScissor_[2] = w, reachScissor_[3] = h;
        const double *v = reachViewport_;
        if (scissorOutsideViewport(x, y, w, h, v[0], v[1], v[2], v[3]))
            w = h = 0;
        const Rect r = reachScissor(x, y, w, h, reachW_, reachH_, reachStep_);
        x = r.x0, y = r.y0, w = r.x1 - r.x0, h = r.y1 - r.y0;
    }
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
    views_.scissors = 1;
}

// Metal's viewport as m2v gives it to Vulkan (y flipped: y + h, height -h) and NVK writes it (nvk_emit_viewport)
void RenderEncoder::setViewport(double x, double y, double w, double h, double znear, double zfar)
{
    const Rect reachClip = reachStep_ ? reachViewportClip(x, y, w, h, reachStep_) : Rect{};
    if (reachStep_) {
        reachViewport_[0] = x, reachViewport_[1] = y, reachViewport_[2] = w, reachViewport_[3] = h;
        const double s = 1.0 / reachStep_;
        x *= s, y *= s, w *= s, h *= s;
    }
    depthRange_[0] = float(znear);
    depthRange_[1] = float(zfar);
    spillDirty_ = true;
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
    if (reachStep_) {
        viewport_[6] = (reachClip.x1 - reachClip.x0) << 16 | reachClip.x0;
        viewport_[7] = (reachClip.y1 - reachClip.y0) << 16 | reachClip.y0;
        const uint32_t *c = reachScissor_;
        setScissor(c[0], c[1], c[2], c[3]);
    }
    viewportDirty_ = true;
    views_.viewports = 1;
}

void RenderEncoder::setViewports(const double (*viewports)[6], uint32_t count)
{
    count = std::clamp(count, 1u, kMaxViewports);
    setViewport(viewports[0][0], viewports[0][1], viewports[0][2], viewports[0][3], viewports[0][4], viewports[0][5]);
    for (uint32_t j = 1; j < count; j++) {
        viewportWords(viewports[j], views_.viewport[j]);
        std::copy_n(viewports[j], 4, views_.vp[j]);
    }
    views_.dirtyViewports |= ((1u << count) - 1) & ~1u;
    views_.viewports = count;
}

void RenderEncoder::setScissors(const uint32_t (*rects)[4], uint32_t count)
{
    count = std::clamp(count, 1u, kMaxViewports);
    setScissor(rects[0][0], rects[0][1], rects[0][2], rects[0][3]);
    for (uint32_t j = 1; j < count; j++)
        views_.scissor[j] = scissorBox(rects[j], areaW_, areaH_);
    views_.dirtyScissors |= ((1u << count) - 1) & ~1u;
    views_.scissors = count;
}

// Viewports and scissors 1 .. count - 1 as NVK writes viewport j (nvk_emit_viewport, nvk_emit_scissor), through a turned
// pass's transform and its clip-space swizzle at each index (index 0's is the pass's prologue's)
void RenderEncoder::emitViewports(Push &p)
{
    for (uint32_t dirty = views_.dirtyViewports; dirty; dirty &= dirty - 1) {
        const uint32_t j = uint32_t(__builtin_ctz(dirty));
        switch_gm20b_viewport_state v = viewportState(views_.viewport[j]);
        turnViewport(rot_, v);
        kit(p, switch_gm20b_viewport, j, &v);
        if (rot_.turned() && !(views_.swizzled >> j & 1)) {
            kit(p, switch_gm20b_viewport_swizzle, j, rot_.swizzle());
            views_.swizzled |= 1u << j;
        }
    }
    for (uint32_t dirty = views_.dirtyScissors; dirty; dirty &= dirty - 1) {
        const uint32_t j = uint32_t(__builtin_ctz(dirty));
        const ScissorBox &b = views_.scissor[j];
        const Rect s = planeRect(b.x0, b.y0, b.x1, b.y1);
        kit(p, switch_gm20b_scissor, j, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
    }
    views_.dirtyViewports = views_.dirtyScissors = 0;
}

void RenderEncoder::setBlendColor(const float rgba[4])
{
    uint32_t v[4];
    memcpy(v, rgba, sizeof v);
    if (memcmp(v, blendColor_, sizeof v)) {
        memcpy(blendColor_, v, sizeof v);
        blendColorDirty_ = true;
        blendWordsDirty_ = true;
        spillDirty_ = true;
    }
}

void RenderEncoder::setDepthStencil(const DepthStencilState *s)
{
    if (reachStep_)
        return;  // a reach target has no depth or stencil
    dsState_ = s ? s : &DepthStencilState::metalDefault();
    ds_ = dsState_->desc();
    compatible_ = nullptr;
    dsDirty_ = true;
    spillDirty_ = true;
}

void RenderEncoder::setStencilReference(uint32_t front, uint32_t back)
{
    stencilRef_[0] = front;
    stencilRef_[1] = back;
    dsDirty_ = true;
    spillDirty_ = true;
}

// The full barrier m2v puts between passes: the shaders' data writes flushed (storage buffers and textures; ROP writes are
// behind the wait for idle) and their data and texture caches invalidated
void RenderEncoder::barrier()
{
    if (p_.room() < 64)
        reserve();
    kit(p_, switch_gm20b_pass_barrier, tiledCache(dev_->k_.info()));
    attachmentWrites_ = {};
    written_.clear();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    anyWritten_ = false;
    ropWritten_ = storageWritten_ = 0;
#endif
}

bool RenderEncoder::draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
                         const char **why)
{
    if (amplifies() && runsPerAmplification(primitive, count, instances, false))
        return amplified([&] {
            return draw(primitive, first, count, instances, baseInstance, why);
        });
    RAW_ENCODE_SPAN(Draw);
    Pipeline *pl = formFor(pipeline_);
    if (!pl)
        return true;  // m2v drops a draw without a pipeline (parity 0006) the same way
    called_ = true;
    readBits_ |= pl->fetchBits;
    if (!count || !instances)
        return true;
    const Runs *ordered = selfRuns(*pl, primitive, count, instances, why);
    if (!ordered)
        return false;
    const Runs &runs = *ordered;
    if (!runs.empty()) {
        size_t run = 0;
        for (const auto &[begin, end] : runs) {
#ifdef SWITCH_RAW_HARNESS_MARKS
            // the host frame harness's builds only: nvdec names this mark SPLIT, and eqcheck folds the run into the one before
            if (begin) {
                if (p_.room() < 1024)
                    reserve();
                p_.m(NO_OPERATION, 0x5b1175);
            }
#endif
            const bool reverse = runWinding_[run++];
            raster_.winding ^= reverse;
            const bool prepared = prepare(pl, primitive, first + begin, first + begin, end - begin, false, why);
            raster_.winding ^= reverse;
            if (!prepared)
                return false;
            if (drawnWhole(*pl)) {
                wholeDraw(*pl, false, first + begin, end - begin, 1, first + begin, baseInstance);
                finishDraw(end - begin, 1);
            } else {
                emitDraw(false, first + begin, end - begin, 1, first + begin, baseInstance, Shortcut{});
            }
        }
        cb_->draws++;
        return true;
    }
    if (instances == 1 && !amplifies() && blurTwod(*pl, primitive, first, count, false)) {
        cb_->draws++;
        return true;
    }
    const Shortcut cut = instances == 1 && !amplifies() ? shortcut(*pl, primitive, first, count, false) : Shortcut{};
    if (!prepare(pl, primitive, first, first, count, false, why))
        return false;
    cb_->draws += amplifies() ? amplifications_ : 1;
    const uint32_t drawnInstances = amplifies() ? instances * amplifications_ : instances;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (const auto probe = instances == 1 ? dev_->armedProbe() : nullptr) {
        if (p_.room() < kInstanceWords + probe->before.size() + probe->after.size() + kShortcutWords)
            reserve();
        if (!probe->before.empty())
            p_.raw(probe->before.data(), uint32_t(probe->before.size()));
        dev_->probeEmitted.fetch_add(1, std::memory_order_relaxed);
        shortcutBegin(cut);
        wholeDraw(*pl, false, first, cut.bbox ? 3 : count, instances, first, baseInstance);
        shortcutEnd(cut);
        if (!probe->after.empty())
            p_.raw(probe->after.data(), uint32_t(probe->after.size()));
        finishDraw(count, drawnInstances);
        return true;
    }
#endif
    if (drawnWhole(*pl)) {
        wholeDraw(*pl, false, first, count, instances, first, baseInstance);
        finishDraw(count, drawnInstances);
    } else {
        emitDraw(false, first, count, instances, first, baseInstance, cut);
    }
    return true;
}

// The runs a draw that fetches a color it writes through the texture unit is drawn as, one triangle each
// (sequentialRuns); empty: it is drawn whole, as a draw that fetches inside the pixel interlock is. The indexed draw being
// prepared sets drawIndices_ first
const RenderEncoder::Runs *RenderEncoder::selfRuns(const Pipeline &pl, uint32_t primitive, uint32_t count,
                                                   uint32_t instances, const char **why)
{
    static thread_local Runs runs;
    runs.clear();
    runWinding_.fill(false);
    const bool indexed = drawIndices_ != nullptr;
    const FetchDraw shape = {primitive, count, instances, false, indexed};
    const bool fetchesOwn = fetchesOwnColor(pl.fetchBits, pl.writeBits, pl.storageBits);
    if (!fetchesOwn || singlePrimitive(shape))
        return &runs;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (dev_->probeFetchRung.load(std::memory_order_relaxed) == kProbeBlendTwinWhole)
        return &runs;
#endif
    if (const FetchRefusal refusal = fetchRefusal(fetchesOwn, pl.primitiveId, shape); refusal != FetchRefusal::None)
        return refuseFetch(pl, refusal, primitive, count, instances, why);
    return sequentialRuns(pl, shape, runs, why);
}

// A draw whose own triangles may cover a pixel twice, one triangle a run: each is ordered after the one before by
// prepare()'s framebuffer-fetch barrier, and no position or index byte is read
const RenderEncoder::Runs *RenderEncoder::sequentialRuns(const Pipeline &pl, const FetchDraw &shape, Runs &runs,
                                                         const char **why)
{
    const FetchRefusal refusal = sequentialRefusal(pl.primitiveId, shape);
    if (refusal != FetchRefusal::None ||
        !TriangleRuns::sequential(shape.primitive, shape.count, shape.instances, shape.indexed, runs, runWinding_))
        return refuseFetch(pl, refusal != FetchRefusal::None ? refusal : FetchRefusal::Geometry, shape.primitive,
                           shape.count, shape.instances, why);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    dev_->stats.fetch.sequential++;
    dev_->stats.fetch.sequentialTriangles += runs.size();
#endif
    return &runs;
}

StorageFallback RenderEncoder::storageFallback(const Pipeline &pl) const
{
    if (!pl.storageBits)
        return StorageFallback::None;
    if (const Pipeline *twin = pl.barrierTwin;
        twin && fetchesOnlyUnwritten(pl.fetchBits, pl.writeBits, pl.storageWriteBits, twin->fetchBits, twin->writeBits))
        return StorageFallback::ReadOnly;
    StorageFallback reason = StorageFallback::None;
    for (uint32_t slot = 0; slot < 8 && reason == StorageFallback::None; slot++)
        if (const Texture *t = pass_.color[slot].texture; t && (pl.storageBits >> slot & 1))
            reason = storageViewMismatch(texelBytes(pl.storageTic[slot]), pl.rec->storageSamples, *t,
                                         dev_->storageCompressedOk());
    return reason;
}

// The colour targets the pass binds: through its last attachment (CommandBuffer::render's targets)
uint32_t RenderEncoder::colorTargets() const
{
    uint32_t count = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (pass_.color[j].texture)
            count = j + 1;
    return count;
}

// The state's fetch-free form where this draw takes it (raw_fetch_free.h): the blended colour's target in the pass,
// CPU-visible memory that is not a surface, and the rung-2 blends already over it decide rule fetch.blend
Pipeline *RenderEncoder::fetchFreeFor(const Pipeline &pl)
{
    Pipeline *free = pl.fetchFree;
    if (!free)
        return nullptr;
    const FetchFreeForm &f = free->freeForm;
    const Texture *t = f.colour < pass_.count ? pass_.color[f.colour].texture : nullptr;
    if (t && f.colour < 8 && shadowOf_[f.colour])
        t = shadowOf_[f.colour];  // the texture the client holds, not the pool image the pass renders it through
    if (!t)
        return nullptr;
    const FetchFreeDraw draw = {t->format, t->storage.hostVisible && !t->import, ropLayers_[f.colour]};
    uint8_t forced = 0;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    forced = dev_->probeFetchRung.load(std::memory_order_relaxed);
#endif
    if (!fetchFreeTaken(f, draw, forced))
        return nullptr;
    if (f.rung == FetchBlend && ropLayers_[f.colour] < 255)
        ropLayers_[f.colour]++;
    RAW_DIAGNOSTIC(dev_->stats.fetch.fetchFree[f.rung == FetchBlend ? 0 : 1]++);
    return free;
}

Pipeline *RenderEncoder::formFor(Pipeline *pl)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (const uint8_t forced = dev_->probeFetchRung.load(std::memory_order_relaxed);
        pl && (forced == kProbeBlendTwinTriangles || forced == kProbeBlendTwinWhole))
        if (Pipeline *twin = pl->blendTwin ? pl->blendTwin : pl->barrierTwin) {
            dev_->stats.fetch.blendTwin++;
            return twin;
        }
#endif
    if (pl)
        if (Pipeline *free = fetchFreeFor(*pl))
            return free;
    if (!pl || !pl->storageBits)
        return pl;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb_->forms_.barrierFetch && pl->barrierTwin)
        return pl->barrierTwin;
#endif
    const StorageFallback reason = storageFallback(*pl);
    if (reason == StorageFallback::None)
        return pl;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const StorageFallback counted = pl->barrierTwin ? reason : StorageFallback::NoTwin;
    dev_->stats.fetch.storageFallback[size_t(counted)]++;
    dev_->stats.fetch.fallbackKeys.note(pl->rec->keyHash);
#endif
    return pl->barrierTwin ? pl->barrierTwin : pl;
}

const RenderEncoder::Runs *RenderEncoder::refuseFetch(const Pipeline &pl, FetchRefusal refusal, uint32_t primitive,
                                                      uint32_t count, uint32_t instances, const char **why)
{
    *why = fetchRefusalWhy(refusal);
    cb_->refusal = {RefGeometry, primitive, count, instances};
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    dev_->stats.fetch.refusedAtEncode[size_t(refusal)]++;
    dev_->stats.fetch.encodeKeys.note(pl.rec ? pl.rec->keyHash : 0);
#else
    (void)pl;
#endif
    return nullptr;
}

// NVK's indexed draw on MAXWELL_B: the index buffer as NVK_MME_BIND_IB sets it (start, last byte, the restart index of the type,
// the index size) where it changed, the draw parameters as NVK_MME_DRAW_INDEXED's (the base vertex in the root table and the
// base registers, the base instance), then per instance BEGIN, SET_INDEX_BUFFER_F (first index) and DRAW_INDEX_BUFFER (count),
// END. m2v binds the indices the draw reads.
bool RenderEncoder::drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib,
                                uint64_t offset, uint32_t instances, int32_t baseVertex, uint32_t baseInstance,
                                const char **why)
{
    if (amplifies() && runsPerAmplification(primitive, count, instances, true))
        return amplified([&] {
            return drawIndexed(primitive, count, indexSize, ib, offset, instances, baseVertex, baseInstance, why);
        });
    RAW_ENCODE_SPAN(Draw);
    Pipeline *pl = formFor(pipeline_);
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
    // indices read up to the length rounded up to 4 bytes, index 0 at every position past that
    offset = uint32_t(offset);
    offset -= offset % indexSize;
    const uint64_t fetchEnd = offset < ib->length ? (ib->length + 3) & ~uint64_t(3) : ib->length,
                   inside = fetchEnd > offset ? (fetchEnd - offset) / indexSize : 0;
    Buffer padded;
    drawIndexBuffer_ = ib;
    drawIndexCpuSafe_ = false;
    // the index fetch's last byte: the draw's last index where the buffer holds every one, else the buffer's fetch end
    // with index 0 past it from the hardware, the zero page, or a padded copy
    uint64_t indexLimit = uint64_t(count) * indexSize;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint32_t probedLimit = cb_->forms_.indexLimit;
    if (probedLimit)
        cb_->taken_.indexLimits++;
#else
    constexpr uint32_t probedLimit = 0;
#endif
    if (probedLimit) {
        indexLimit = probedLimit;
        drawIndexBuffer_ = nullptr;
    } else if (count > inside && !inside && uint64_t(count) * indexSize <= dev_->zero_.bytes) {
        padded.va = dev_->zero_.va;
        padded.cpu = dev_->zero_.cpu;
        padded.length = dev_->zero_.bytes;
        ib = &padded;
        drawIndexCpuSafe_ = true;
        drawIndexBuffer_ = nullptr;
        offset = 0;
    } else if (count > inside && dev_->indexZeroPastLimit) {
        indexLimit = fetchEnd - offset;
        drawIndexBuffer_ = nullptr;  // the CPU reads no index past the buffer's end (an unbounded draw)
    } else if (count > inside) {
        if (inside && !cb_->acquireGeometry(*ib, ib->cpu + offset, inside * indexSize)) {
            *why = "index padding requires available CPU read ownership";
            cb_->refusal = {RefIndexedRange, uint32_t(offset), indexSize, count};
            return false;
        }
        const uint64_t bytes = uint64_t(count) * indexSize;
        uint8_t *m =
            dev_->alloc(dev_->upload_, (bytes + kUboChunk - 1) & ~uint64_t(kUboChunk - 1), kUboChunk, padded.va);
        if (inside)
            memcpy(m, ib->cpu + offset, inside * indexSize);
        memset(m + inside * indexSize, 0, bytes - inside * indexSize);
        padded.cpu = m;
        padded.length = bytes;
        padded.wc = true;
        RAW_DIAGNOSTIC(cb_->counts_.uploadBytes += bytes);
        ib = &padded;
        drawIndexCpuSafe_ = true;  // immutable upload bytes owned by this command buffer
        drawIndexBuffer_ = nullptr;
        offset = 0;
    }
    cb_->use(*ib, false);
    // Never scan potentially GPU-produced indices to choose a CPU cache range. Cached vertex bindings are
    // cleaned conservatively below; WC vertices need no clean, and geometry reads have their own read lease.
    const uint8_t *at = ib->cpu + offset;
    if (ib->hostVisible && !ib->wc)
        cb_->clean(at, std::min<uint64_t>(indexLimit, uint64_t(count) * indexSize));
    drawIndices_ = at;
    drawIndexSize_ = indexSize;
    drawIndexCount_ = count;
    const Runs *ordered = selfRuns(*pl, primitive, count, instances, why);
    if (!ordered) {
        drawIndices_ = nullptr;
        return false;
    }
    const Runs &runs = *ordered;
    const bool whole = runs.empty() && instances == 1 && baseVertex >= 0 && !amplifies();
    if (whole && blurTwod(*pl, primitive, uint32_t(baseVertex), count, true)) {
        drawIndices_ = nullptr;
        cb_->draws++;
        return true;
    }
    const Shortcut cut = whole ? shortcut(*pl, primitive, uint32_t(baseVertex), count, true) : Shortcut{};
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
        const bool reverse = !runs.empty() && runWinding_[r];
        raster_.winding ^= reverse;
        const bool prepared = prepare(pl, primitive, baseVertex, 0, 0, true, why);
        raster_.winding ^= reverse;
        drawIndices_ = nullptr;
        if (!prepared)
            return false;
        const uint64_t va = ib->va + offset, last = va + indexLimit - 1;
        if (va != ibVa_ || last != ibEnd_ || indexSize != ibSize_) {
            p_.inc(INDEX_BUFFER_A, {uint32_t(va >> 32), uint32_t(va), uint32_t(last >> 32), uint32_t(last)});
            p_.m(DA_PRIMITIVE_RESTART_INDEX, indexSize == 2 ? 0xffffu : 0xffffffffu);
            cb_->restart_ = indexSize == 2 ? 0xffffu : 0xffffffffu;
            p_.m(INDEX_BUFFER_E, indexSize == 2 ? 1 : 2);  // INDEX_SIZE TWO_BYTES / FOUR_BYTES
            ibVa_ = va;
            ibEnd_ = last;
            ibSize_ = indexSize;
        }
        if (drawnWhole(*pl)) {
            wholeDraw(*pl, true, begin, end - begin, instances, uint32_t(baseVertex), baseInstance);
            finishDraw(end - begin, amplifies() ? instances * amplifications_ : instances);
        } else {
            emitDraw(true, begin, end - begin, instances, uint32_t(baseVertex), baseInstance, cut);
        }
    }
    cb_->draws += amplifies() ? amplifications_ : 1;
    return true;
}

// GPU-owned indirect arguments are fetched by the command streamer after the method header. No argument bytes, indices,
// or positions are read back to the CPU. Geometry uses the conservative render/scissor extent for this draw.
bool RenderEncoder::indirect(uint32_t primitive, bool indexed, const Buffer *arguments, uint64_t offset,
                             const char **why)
{
    RAW_ENCODE_SPAN(Draw);
    if (!pipeline_)
        return true;
    Pipeline *form = formFor(pipeline_);
    const Pipeline &pl = *form;
    const bool fetchesOwn = fetchesOwnColor(pl.fetchBits, pl.writeBits, pl.storageBits);
    if (const FetchRefusal refusal = fetchRefusal(fetchesOwn, pl.primitiveId, {primitive, 0, 1, true});
        refusal != FetchRefusal::None) {
        refuseFetch(pl, refusal, primitive, 0, 0, why);
        return false;
    }
    const uint32_t bytes = indexed ? 20 : 16;
    if (!arguments || !arguments->va || (offset & 3) || offset > arguments->length ||
        arguments->length - offset < bytes) {
        *why = "indirect arguments outside their buffer or not four-byte aligned";
        return false;
    }
    called_ = true;
    indirectGeometry_ = true;
    const bool prepared = prepare(form, primitive, 0, 0, UINT32_MAX, indexed, why);
    indirectGeometry_ = false;
    if (!prepared)
        return false;
    cb_->use(*arguments, false);
    if (arguments->hostVisible && !arguments->wc && arguments->cpu)
        cb_->clean(arguments->cpu + offset, bytes);
    if (p_.room() < 8 + mme::kAmplificationScratchWords + mme::kAmplifyCallWords)
        reserve();
    // Prior shader writes must reach the command fetcher; NO_PREFETCH on the argument entry prevents early fetch.
    p_.m(WAIT_FOR_IDLE, 0);
    p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateConstant);
    p_.m(0x0050, 0);  // NV906F_SET_REFERENCE: pre-Turing MME command-fetch visibility (NVK indirect-read barrier)
    if (amplifies()) {
        amplificationScratch(p_, pl);
        mme::amplifyHeader(p_, indexed);
        heldAmplifyWords(pl);
    } else {
        mme::indirectHeader(p_, indexed);
    }
    bodies_.push_back({bodyStart_, p_.at, 0, 0, true});
    bodies_.push_back(PushBody::resident(arguments->va + offset, bytes / 4, TegraGM20BPushNoPrefetch));
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    bodyStart_ = nullptr;
    reserve();
    params_.unknown = true;
    cb_->draws += amplifies() ? amplifications_ : 1;
    finishDraw(0, 0, true);
    return true;
}

bool RenderEncoder::drawIndirect(uint32_t primitive, const Buffer *arguments, uint64_t offset, const char **why)
{
    return indirect(primitive, false, arguments, offset, why);
}

bool RenderEncoder::drawIndexedIndirect(uint32_t primitive, uint32_t indexSize, const Buffer *indices,
                                        uint64_t indexOffset, const Buffer *arguments, uint64_t argumentOffset,
                                        const char **why)
{
    if (!pipeline_)
        return true;
    if (!indices || !indices->va || (indexSize != 2 && indexSize != 4) || indexOffset > indices->length ||
        indices->length - indexOffset < indexSize || indexOffset % indexSize) {
        *why = "an indirect draw has no valid aligned index buffer range";
        return false;
    }
    if (p_.room() < 16)
        reserve();
    cb_->use(*indices, false);
    const uint64_t va = indices->va + indexOffset, last = indices->va + indices->length - 1;
    p_.inc(INDEX_BUFFER_A, {uint32_t(va >> 32), uint32_t(va), uint32_t(last >> 32), uint32_t(last)});
    cb_->restart_ = indexSize == 2 ? 0xffffu : 0xffffffffu;
    p_.m(DA_PRIMITIVE_RESTART_INDEX, cb_->restart_);
    p_.m(INDEX_BUFFER_E, indexSize == 2 ? 1 : 2);
    ibVa_ = va;
    ibEnd_ = last;
    ibSize_ = indexSize;
    if (indices->hostVisible && !indices->wc && indices->cpu)
        cb_->clean(indices->cpu + indexOffset, indices->length - indexOffset);
    return indirect(primitive, true, arguments, argumentOffset, why);
}

// The state the commands inherit prepared once, as an indirect draw's, the range into the macros' scratch, then the
// commands' words by reference, once per amplification with its id between. Each command sets its topology, restart, index
// buffer and draw parameters, which the encoder holds unknown after them; patch commands their control-stage words too.
bool RenderEncoder::executeSlots(const IcbExecute &x, const char **why)
{
    RAW_ENCODE_SPAN(Draw);
    if (!pipeline_ || !x.count)
        return true;
    Pipeline *form = formFor(pipeline_);
    const Pipeline &pl = *form;
    const bool fetchesOwn = fetchesOwnColor(pl.fetchBits, pl.writeBits, pl.storageBits);
    if (const FetchRefusal refusal = fetchRefusal(fetchesOwn, pl.primitiveId, {x.primitive, 0, 1, true});
        refusal != FetchRefusal::None) {
        refuseFetch(pl, refusal, x.primitive, 0, 0, why);
        return false;
    }
    const uint64_t slotBytes = uint64_t(x.slotWords) * 4, offset = x.first * slotBytes, bytes = x.count * slotBytes;
    if (!x.slots->va || offset > x.slots->length || x.slots->length - offset < bytes) {
        *why = "indirect command buffer commands outside its memory";
        return false;
    }
    if (x.range && (!x.range->va || (x.rangeOffset & 3) || x.rangeOffset > x.range->length ||
                    x.range->length - x.rangeOffset < 8)) {
        *why = "an indirect command buffer's execution range outside its buffer or not four-byte aligned";
        return false;
    }
    called_ = true;
    indirectGeometry_ = true;
    const bool prepared = prepare(form, x.primitive, 0, 0, UINT32_MAX, x.indexed, why);
    indirectGeometry_ = false;
    const bool patches = x.primitive == icb::kPatchPrimitive;
    if (!prepared || (patches && !icbPatchLayout(why)))
        return false;
    cb_->use(*x.slots, false);
    if (x.slots->hostVisible && !x.slots->wc && x.slots->cpu)
        cb_->clean(x.slots->cpu + offset, bytes);
    for (size_t i = 0; i < x.bufferCount; i++) {
        const Buffer &b = *x.buffers[i];
        cb_->use(b, false);
        if (b.hostVisible && !b.wc && b.cpu)
            cb_->clean(b.cpu, b.length);
    }
    if (p_.room() < 16)
        reserve();
    if (x.written || x.range) {
        p_.m(WAIT_FOR_IDLE, 0);
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateConstant);
        p_.m(0x0050, 0);  // NV906F_SET_REFERENCE, as indirect(): the fetches wait for the writes before them
    }
    if (x.range) {
        cb_->use(*x.range, false);
        if (x.range->hostVisible && !x.range->wc && x.range->cpu)
            cb_->clean(x.range->cpu + x.rangeOffset, 8);
        mme::icbRangeHeader(p_);
        bodies_.push_back({bodyStart_, p_.at, 0, 0, true});
        bodies_.push_back(PushBody::resident(x.range->va + x.rangeOffset, 2, TegraGM20BPushNoPrefetch));
        bodyStart_ = p_.at;
    } else {
        mme::icbRange(p_, x.first, x.count);
    }
    for (uint32_t a = 0; a < amplifications_; a++) {
        if (amplifications_ > 1)
            icbAmplification(a);
        if (p_.at > bodyStart_)
            bodies_.push_back({bodyStart_, p_.at});
        bodies_.push_back(PushBody::resident(x.slots->va + offset, uint32_t(bytes / 4), TegraGM20BPushNoPrefetch));
        dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
        bodyStart_ = nullptr;
        reserve();
    }
    if (patches)
        icbPatchesDone();
    topology_ = restart_ = ~0u;
    ibVa_ = ibEnd_ = ~uint64_t(0);
    ibSize_ = 0;
    params_.unknown = true;
    ++cb_->draws;
    finishDraw(0, 0, true);
    return true;
}

#if SWITCH_RAW_DIAGNOSTICS
static void probeUniforms(Pipeline &pl, uint32_t stage, uint32_t slot, const uint8_t *bytes, uint64_t length)
{
    const NonFiniteUniforms found = nonFiniteUniforms(bytes, length);
    if (!found.words && (!found.halves || pl.uniformHalvesReported))
        return;
    char hex[3 * 32 + 1] = "";
    const uint64_t from = found.first & ~uint64_t(15);
    for (uint64_t at = from, n = 0; at < length && n < 32; ++at, ++n)
        snprintf(hex + 3 * n, 4, "%02x ", bytes[at]);
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
           "raw: uniform probe %{public}s stage %u slot %u: %u non-finite floats and %u halves in %llu bytes, first at "
           "+%llu; "
           "+%llu: %{public}s(mach %llu)",
           pl.uniformProbe, stage, slot, found.words, found.halves, (unsigned long long)length,
           (unsigned long long)found.first, (unsigned long long)from, hex, (unsigned long long)mach_absolute_time());
    if (found.words)
        pl.uniformProbe = nullptr;
    else
        pl.uniformHalvesReported = true;
}

namespace {
// The keyline probe's generation (each armKeylineProbe starts the next), and its armed window's lines left and end
std::atomic<uint32_t> gKeylineGeneration{1};
std::atomic<int32_t> gKeylineLines{0};
std::atomic<int64_t> gKeylineUntil{0};

int64_t keylineNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool keylineArmed()
{
    if (gKeylineLines.load(std::memory_order_relaxed) <= 0)
        return false;
    if (keylineNow() < gKeylineUntil.load(std::memory_order_relaxed))
        return true;
    gKeylineLines.store(0, std::memory_order_relaxed);
    return false;
}

// "t<slot>:<w>x<h>/f<format>/<bl|pitch>[+ml][+import]" for each bound texture of `slots` (bit i: slot i, of the first 16)
void describeTextures(char *out, size_t size, const Texture *const *textures, uint32_t slots)
{
    snprintf(out, size, "-");
    size_t at = 0;
    for (uint32_t slot = 0; slot < 16 && at + 1 < size; slot++) {
        const Texture *t = textures[slot];
        if (!(slots >> slot & 1) || !t)
            continue;
        const int n =
            snprintf(out + at, size - at, "%st%u:%ux%u/f%u/%s%s%s", at ? " " : "", slot, t->width, t->height, t->format,
                     t->blockLinear ? "bl" : "pitch", t->memoryless ? "+ml" : "", t->import ? "+import" : "");
        at += std::min(size - at - 1, size_t(std::max(n, 0)));
    }
}
}  // namespace

void armKeylineProbe()
{
    gKeylineLines.store(256, std::memory_order_relaxed);
    gKeylineUntil.store(keylineNow() + 30000, std::memory_order_relaxed);
    gKeylineGeneration.fetch_add(1, std::memory_order_relaxed);
}

// A Core Animation draw (Pipeline::caName), for each form of its state: a solid fill of its three-colour pass its first four
// draws and first twelve thin ones each probe generation, any other state its first four thin draws while the probe is
// armed. Its line: the form drawn (storage: the state's own, inside the pixel interlock; barrier: its twin, as formFor took
// it; barrier-record: the pack's barrier record standing for an unusable first one; split: its split twin; whole: its one
// program), why its storage form fell back, its colour bits, each colour attachment as the pass bound it (format/bytes a
// texel/samples/layout/size, then the load: d don't care, l load, c clear), the textures its record samples and, for a fill
// that samples none, the ones an earlier draw left at slots 0 and 1, the draw's pixels (scissor within viewport), and its
// quads as Core Animation wrote them (raw_keyline_probe.h): how many, how many thin, the first and the first thin one with
// its colour
void RenderEncoder::probeDraw(const Pipeline &bound, Pipeline &drawn, uint32_t primitive, int32_t first, uint32_t count,
                              bool indexed, const PixelBox &limit)
{
    if (glassProbeArmed())
        probeGlassDraw(bound, drawn, limit);
    if (dev_->glassMemLive_)
        glassMemSample(bound, drawn);
    const uint32_t generation = gKeylineGeneration.load(std::memory_order_relaxed);
    if (drawn.probeGeneration != generation) {
        drawn.probeGeneration = generation;
        drawn.probeLines = drawn.probeThinLines = 0;
    }
    const uint8_t anyLines = bound.fillState ? 4 : 0, thinLines = bound.fillState ? 12 : 4;
    if ((drawn.probeLines >= anyLines && drawn.probeThinLines >= thinLines) || (!bound.fillState && !keylineArmed()))
        return;
    const Device &d = *dev_;
    const PackPipeline &rec = *drawn.rec;
    const PackPipeline *packed = d.pack_.section<PackPipeline>(PackPipelines);
    const bool inPack = &rec >= packed && &rec < packed + d.pack_.bytes(PackPipelines) / sizeof(PackPipeline);
    const Bound &matrix = bufs_[1][2];
    const Bound &stream = bufs_[1][rec.vertexSlot < 31 ? rec.vertexSlot : 0];
    const uint64_t skip = first >= 0 ? uint64_t(first) * rec.vertexStride : UINT64_MAX;
    FillStream s;
    if (stream.cpu && skip < stream.size) {
        s.bytes = stream.cpu + skip;
        s.size = stream.size - skip;
    }
    s.stride = rec.vertexStride;
    s.positionOffset = drawn.positionOffset;
    s.positionComponents = drawn.positionComponents;
    s.colorOffset = inPack ? fillColorOffset(d.pack_.string(rec.keyString)) : -1;
    std::vector<uint32_t> indices;
    if (indexed && drawIndices_)
        for (uint32_t i = 0; i < std::min(drawIndexCount_, kFillScanVertices); i++)
            indices.push_back(drawIndexSize_ == 2 ? reinterpret_cast<const uint16_t *>(drawIndices_)[i]
                                                  : reinterpret_cast<const uint32_t *>(drawIndices_)[i]);
    const uint32_t vertices = indexed ? uint32_t(indices.size()) : count;
    const uint32_t group = std::min(primitive == 4 ? vertices : 6u, vertices);
    const FillScan scan = fillScan(s, indexed ? indices.data() : nullptr, vertices, group,
                                   matrix.cpu && matrix.size >= 64 ? matrix.cpu : nullptr, vp_);
    if (drawn.probeLines < anyLines)
        drawn.probeLines++;
    else if (scan.thin && drawn.probeThinLines < thinLines)
        drawn.probeThinLines++;
    else
        return;
    if (!bound.fillState && gKeylineLines.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return;
    const char *form = drawn.splitOf                     ? "split"
                       : drawn.storageBits               ? "storage"
                       : &drawn == bound.barrierTwin     ? "barrier"
                       : rec.flags & PipelineBarrierForm ? "barrier-record"
                                                         : "whole";
    char attachments[8 * 48] = "-";
    size_t used = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const Texture *t = pass_.color[j].texture;
        const size_t room = sizeof attachments - used;
        const int n =
            t ? snprintf(attachments + used, room, "%sc%u:%u/%u/%u/%s%s%s/%ux%u/%c", used ? " " : "", j, t->format,
                         t->bpp, t->samples, t->blockLinear ? "bl" : "pitch", t->comptags ? "+comp" : "",
                         memoryless_[j] ? "+ml" : "", t->width, t->height, "dlc"[pass_.color[j].load])
              : snprintf(attachments + used, room, "%sc%u:-", used ? " " : "", j);
        used += std::min(room - 1, size_t(std::max(n, 0)));
    }
    uint32_t sampledSlots = 0;
    for (uint32_t k = 0; k < rec.entries && k < kPackEntries; k++)
        if (rec.entry[k].kind == EntryTexture && rec.entry[k].stage == 2 && rec.entry[k].slot < 16)
            sampledSlots |= 1u << rec.entry[k].slot;
    char sampled[16 * 48], left[2 * 48] = "-", colour[64] = "-";
    describeTextures(sampled, sizeof sampled, textures_[1], sampledSlots);
    if (!sampledSlots)
        describeTextures(left, sizeof left, textures_[1], 3);
    const FillQuad &q = scan.thin ? scan.firstThin : scan.first;
    if (q.colored)
        snprintf(colour, sizeof colour, "%.5f,%.5f,%.5f,%.5f", q.color[0], q.color[1], q.color[2], q.color[3]);
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
           "raw: keyline draw %{public}s key %016llx fill %d form %{public}s fallback %{public}s own %d fetch %02x "
           "write %02x read %02x storage %02x storewrite %02x att %{public}s samples %{public}s left %{public}s box "
           "%u,%u-%u,%u draw %u/%u%{public}s quads %u thin %u first %.2f,%.2f-%.2f,%.2f thinquad %u "
           "%.2f,%.2f-%.2f,%.2f colour %{public}s",
           bound.caName, (unsigned long long)rec.keyHash, int(bound.fillState), form,
           kStorageFallbackNames[size_t(storageFallback(bound))],
           fetchesOwnColor(drawn.fetchBits, drawn.writeBits, drawn.storageBits), drawn.fetchBits, drawn.writeBits,
           drawn.readBits, drawn.storageBits, drawn.storageWriteBits, attachments, sampled, left, limit.x0, limit.y0,
           limit.x1, limit.y1, primitive, indexed ? drawIndexCount_ : count, indexed ? " indexed" : "", scan.quads,
           scan.thin, scan.first.box[0], scan.first.box[1], scan.first.box[2], scan.first.box[3], scan.firstThin.first,
           scan.firstThin.box[0], scan.firstThin.box[1], scan.firstThin.box[2], scan.firstThin.box[3], colour);
}

// The glass probe's line for a Core Animation draw (raw_glass_probe.h): the form drawn as probeDraw names it, each
// colour attachment as the pass bound it (format/bytes a texel/samples/layout[+comp][+ml]/size/load), the textures its
// record samples and its uniform buffers
void RenderEncoder::probeGlassDraw(const Pipeline &bound, const Pipeline &drawn, const PixelBox &limit)
{
    const PackPipeline &rec = *drawn.rec;
    const char *form = drawn.splitOf                     ? "split"
                       : drawn.storageBits               ? "storage"
                       : &drawn == bound.barrierTwin     ? "barrier"
                       : rec.flags & PipelineBarrierForm ? "barrier-record"
                                                         : "whole";
    char attachments[8 * 48] = "-", sampled[16 * 56] = "-", uniforms[8 * 32] = "-";
    size_t at = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8 && at + 1 < sizeof attachments; j++)
        if (const Texture *t = pass_.color[j].texture) {
            const int n =
                snprintf(attachments + at, sizeof attachments - at, "%sc%u:%u/%u/%u/%s%s%s/%ux%u/%c", at ? " " : "", j,
                         t->format, t->bpp, t->samples, t->blockLinear ? "bl" : "pitch", t->comptags ? "+comp" : "",
                         memoryless_[j] ? "+ml" : "", t->width, t->height, "dlc"[pass_.color[j].load]);
            at += std::min(sizeof attachments - at - 1, size_t(std::max(n, 0)));
        }
    at = 0;
    for (uint32_t k = 0; k < rec.entries && k < kPackEntries && at + 1 < sizeof sampled; k++)
        if (const PackEntry &e = rec.entry[k];
            e.kind == EntryTexture && e.stage == 2 && e.slot < 16 && textures_[1][e.slot]) {
            at += at ? size_t(snprintf(sampled + at, sizeof sampled - at, " ")) : 0;
            const int n = glassProbeTexture(sampled + at, sizeof sampled - at, e.slot, *textures_[1][e.slot]);
            at += std::min(sizeof sampled - at - 1, size_t(std::max(n, 0)));
        }
    at = 0;
    for (uint32_t k = 0; k < rec.entries && k < kPackEntries && at + 1 < sizeof uniforms; k++)
        if (const PackEntry &e = rec.entry[k]; e.kind == EntryUbo && !bufferStorage(e)) {
            const Bound &b = bufs_[e.stage][e.slot];
            at += at ? size_t(snprintf(uniforms + at, sizeof uniforms - at, " ")) : 0;
            const int n = glassProbeUniform(uniforms + at, sizeof uniforms - at, e.stage, e.slot, b.cpu, b.size);
            at += std::min(sizeof uniforms - at - 1, size_t(std::max(n, 0)));
        }
    char taps[160] = "";
    if (glassProbeBlurState(bound.caName)) {
        const Bound &u = bufs_[2][1];
        taps[0] = ' ';
        glassProbeTaps(taps + 1, sizeof taps - 1, u.cpu, u.size);
    }
    char line[1000];
    snprintf(line, sizeof line, "%s k%016llx %s att %s tex %s uni %s box %u,%u-%u,%u%s", bound.caName,
             (unsigned long long)rec.keyHash, form, attachments, sampled, uniforms, limit.x0, limit.y0, limit.x1,
             limit.y1, taps);
    if (!glassProbeFirst(cb_, line) || gGlassProbeLines.fetch_sub(1, std::memory_order_relaxed) <= 0)
        return;
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"), "raw: glass draw g%u cb %06llx %{public}s",
           gGlassProbeGeneration.load(std::memory_order_relaxed), (unsigned long long)(uintptr_t(cb_) & 0xffffff),
           line);
}

// The blur-texture probe's rows (raw_glass_mem.h) of what a Core Animation draw samples: each texture's level 0 and, for a
// mip chain, its middle level, one row each, read at the command buffer's end. Surfaces are left out (other processes write
// them), and the copy engine reads plain memory only: a compressed binding hands it compressed bytes (raw_comp.h).
void RenderEncoder::glassMemSample(const Pipeline &bound, const Pipeline &drawn)
{
    const PackPipeline &rec = *drawn.rec;
    for (uint32_t k = 0; k < rec.entries && k < kPackEntries; k++) {
        const PackEntry &e = rec.entry[k];
        if (e.kind != EntryTexture || e.stage != 2 || e.slot >= 16 || !textures_[1][e.slot])
            continue;
        const Texture &t = *textures_[1][e.slot];
        if (t.import)
            continue;
        const uint32_t levels = t.chain ? t.chain->layout.levels : 1;
        const bool compressed = t.comptags && !t.zeta && copyEngineReadsCompressed(compressedKind(t.bpp, 0, t.samples));
        const char unread = t.comptags && !compressed ? 'c'
                            : t.memoryless            ? 'm'
                            : t.zeta                  ? 'z'
                            : t.converted()           ? 'v'
                            : t.sampled               ? 'p'
                            : t.standIn               ? 's'
                            : t.blockW == 1 && t.blockH == 1 && t.bpp && t.bpp <= 16 && t.samples == 1 &&
                                    t.layers == 1 && t.type == fx::kTexTwoD && t.va
                                ? 0
                                : 'o';
        uint64_t header = 0xcbf29ce484222325ull;
        if (t.tic)
            for (uint32_t b = 0; b < 32; b++)
                header = (header ^ dev_->tic_.cpu[32 * t.tic + b]) * 0x100000001b3ull;
        const uint32_t picks[2] = {0, levels > 1 ? (levels + 1) / 2 : 0};
        for (uint32_t p = 0; p < (picks[1] ? 2u : 1u); p++) {
            Texture level;
            if (picks[p] && !chainLevelImage(t, picks[p], level))
                continue;
            const Texture &image = picks[p] ? level : t;
            GlassMemItem it;
            it.key = t.storageRange().va ? t.storageRange().va : t.va;
            it.image = image.va;
            it.width = image.width;
            it.height = image.height;
            it.format = t.format;
            it.levels = levels;
            it.level = picks[p];
            it.bpp = t.bpp;
            it.texels = std::min(kGlassMemTexels, image.width);
            it.flags =
                uint8_t((t.arena ? kGlassMemHeap : 0) | (t.blockLinear ? 0 : kGlassMemPitch) |
                        (compressed ? kGlassMemCompressed : 0) | (unread || !image.height ? kGlassMemUnsampled : 0));
            it.unread = unread ? unread : image.height ? 0 : 'o';
            const int index = dev_->glassMem->sampled(it, bound.caName, t.tic, header, dev_->commitCount_);
            if (index >= 0) {
                cb_->glassMemImages_[index] = image;
                cb_->glassMemSources_[index] = &t;
            }
        }
    }
}

// What this pass reached of each colour target it stores into (a shadow target's own image, not its pool image): its
// load, the box of its draws' pixels (the render area where they were not counted), its render area and its clears'
// extent from the origin
void RenderEncoder::glassMemReached(const std::vector<Clear> &cl)
{
    uint32_t box[4] = {0, 0, areaW_, areaH_};
    if (drew_ && !whole_ && !rects_.empty()) {
        box[0] = box[1] = UINT32_MAX;
        box[2] = box[3] = 0;
        for (const auto &r : rects_)
            for (int k = 0; k < 4; k++)
                box[k] = k < 2 ? std::min(box[k], r[k]) : std::max(box[k], r[k]);
    }
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const Texture *t = shadowOf_[j] ? shadowOf_[j] : pass_.color[j].texture;
        if (!t || memoryless_[j] || !pass_.color[j].store)
            continue;
        GlassMemReach reach{"dlc"[pass_.color[j].load], box[0], box[1], box[2], box[3], areaW_, areaH_, 0, 0};
        for (const Clear &c : cl)
            if (c.target == j && !c.x0 && !c.y0) {
                reach.clearW = std::max(reach.clearW, c.x1);
                reach.clearH = std::max(reach.clearH, c.y1);
            }
        dev_->glassMem->reached(t->va, reach, dev_->commitCount_);
    }
}

// A write that stops a row or two short of a texture's edges (raw_keyline_probe.h EdgeMiss), once per shape and at most
// 256 shapes each probe generation: what wrote (a pass's render area or its draws' scissors within their viewports, a
// copy, an upload), the texture, the rectangle written and `detail`
static void logEdgeMiss(const char *what, const Texture &t, const PixelBox &r, const char *detail)
{
    const EdgeMiss m = edgeMiss(t.width, t.height, r.x0, r.y0, r.x1, r.y1);
    if (!m.any())
        return;
    static raw::Mutex lock;
    static std::set<std::array<uint32_t, 8>> seen;
    static uint32_t seenGeneration = 0;
    {
        std::lock_guard<raw::Mutex> l(lock);
        if (const uint32_t generation = gKeylineGeneration.load(std::memory_order_relaxed);
            generation != seenGeneration) {
            seen.clear();
            seenGeneration = generation;
        }
        const std::array<uint32_t, 8> shape = {uint32_t(what[0]), t.width, t.height, t.format, r.x0, r.y0, r.x1, r.y1};
        if (seen.size() >= 256 || !seen.insert(shape).second)
            return;
    }
    os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
           "raw: edge miss %{public}s %ux%u f%u %{public}s rect %u,%u-%u,%u short top %u bottom %u left %u right %u"
           "%{public}s",
           what, t.width, t.height, t.format, t.blockLinear ? "bl" : "pitch", r.x0, r.y0, r.x1, r.y1, m.top, m.bottom,
           m.left, m.right, detail);
}
#endif

// Stage `k`'s (0 vertex, 1 fragment) root table for this draw, in the upload ring: m2v's {null, sink, an entry per buffer slot
// it reaches through the table} over the identity resident set (raw_heap_tables.h). A buffer reached so may be written.
uint64_t RenderEncoder::rootTable(const PackPipeline &r, uint32_t k)
{
    Device &d = *dev_;
    const uint32_t slots = r.rootSlots[k], entries = drawRootEntries(slots);
    RootSlot slot[32];
    for (uint32_t n = 0; kFirstSlotRoot + n < entries; n++) {
        const Bound &b = bufs_[k + 1][n];
        slot[n] = {};
        if (!(slots >> n & 1) || !b.va)
            continue;
        if (b.buffer) {
            cb_->use(*b.buffer, true);
            slot[n] = {b.buffer->va, b.buffer->length, b.offset};
        } else {
            cb_->useAddress(b.va, b.size, true);
            slot[n] = {b.va, b.size, 0};
        }
    }
    uint64_t va = 0;
    auto *table = reinterpret_cast<RootEntry *>(d.alloc(d.upload_, entries * sizeof(RootEntry), 256, va));
    writeDrawRoots(table, va, slots, slot, d.heapTables_.sinkVa(), d.heapTables_.identityVa());
    RAW_DIAGNOSTIC(d.stats.uploadBytes += entries * sizeof(RootEntry));
    return va;
}

bool RenderEncoder::prepare(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t vFirst, uint32_t vCount,
                            bool indexed, const char **why)
{
    RAW_ENCODE_SPAN(Prepare);
    if (pl->strideTwin && bindStridePastStream(*pl->rec))
        pl = pl->strideTwin;
    if (tessellates(*pl->rec) != (primitive == icb::kPatchPrimitive)) {
        *why = "primitives drawn with a tessellation state";
        cb_->refusal = {};
        return false;
    }
    if (const char *refused = drawnWordsRefusal(*pl->rec)) {
        *why = refused;
        cb_->refusal = {};
        return false;
    }
    // A record of a vertex function that chooses its slice writes Layer below the pass's length (arrayLengthWord) and
    // one that chooses its viewport ViewportIndex below the encoder's count (viewportCountWord); a record of one layer
    // draws layer 0, and a record made before the viewport word viewport 0
    if (const char *missing = layeredDrawMissing(pl->rec->arrayLengthWord, pass_.layers, kLayerDropped)) {
        *why = missing;
        cb_->refusal = {};
        return false;
    }
    if (views_.viewports > 1 && pl->viewportIndex && !pl->rec->viewportCountWord) {
        *why = "a vertex function choosing among several viewports: its record draws viewport 0";
        cb_->refusal = {RefViewports, views_.viewports, 0, 0};
        return false;
    }
    const uint32_t count = vCount;
    const bool firstDraw = !drew_;
    drew_ = true;
    ++passDraws_;
    readBits_ |= pl->readBits | pl->fetchBits;
    // the pixels it may write: its scissor within its viewport's pixels (m2v vk_scissors, viewport_pixels), over each
    // viewport its vertex function may choose
    uint32_t x[2], y[2];
    viewportPixels(vp_, {sc_[0], sc_[1], sc_[0] + sc_[2], sc_[1] + sc_[3]}, areaW_, areaH_, x, y);
    for (uint32_t j = 1; pl->viewportIndex && j < views_.viewports; j++) {
        uint32_t xj[2], yj[2];
        viewportPixels(views_.vp[j], j < views_.scissors ? views_.scissor[j] : ScissorBox{0, 0, areaW_, areaH_}, areaW_,
                       areaH_, xj, yj);
        if (xj[1] <= xj[0] || yj[1] <= yj[0])
            continue;
        if (x[1] <= x[0] || y[1] <= y[0]) {
            x[0] = xj[0], x[1] = xj[1], y[0] = yj[0], y[1] = yj[1];
            continue;
        }
        x[0] = std::min(x[0], xj[0]), x[1] = std::max(x[1], xj[1]);
        y[0] = std::min(y[0], yj[0]), y[1] = std::max(y[1], yj[1]);
    }
    RAW_ENCODE_BEGIN(geometrySpan, Geometry);
    // only a pass's first draw is bounded by its vertices, for the cover test below (its vertex bytes the recording's
    // cached copy, RMCommandBuffer::_shadow); every other decision takes the draw's pixel limit
    DrawGeometry g;
    if (!firstDraw || (pl->storageBits && !(pl->fetchBits & ~pl->storageBits)))
        g.limit = {x[0], y[0], x[1], y[1]};
    else
        g = geometry(*pl, primitive, first, indexed ? drawIndexCount_ : count, {x[0], y[0], x[1], y[1]});
    auto geometryAnalysis = geometryCache_.analysis(g, bindingRevision_, cb_->geometryBudget_, &cb_->geometryPool_);
    RAW_ENCODE_BEGIN(variantSpan, Variant);
    // a turned draw of a turned variant's state takes the variant over the generic program's split twin
    Pipeline *twin = indirectGeometry_ || (rot_.turned() && pl->turnedVariant) ? pl : splitTwin(pl, geometryAnalysis);
    const Pipeline *launchOnly = nullptr;
    bool generic = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (twin->splitOf && cb_->splitProbe_ == SplitProbe::LaunchOnly)
        launchOnly = twin;
    generic = twin->splitOf && (cb_->splitProbe_ == SplitProbe::Generic || launchOnly);
#endif
    if (!generic)
        pl = twin;
    // a variant has no split of its own: a draw large enough for the generic program's split twin keeps the twin
    if (rot_.turned() && pl->turnedTwin)
        pl = pl->turnedTwin;
    else if (!pl->splitOf && !generic)
        pl = dev_->variant(*pl, bindingFacts(*pl) | (rot_.turned() ? AssumeTurned : 0u));
    RAW_DIAGNOSTIC(logUniformTuple(*pl));
    RAW_ENCODE_END(variantSpan);
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
    RAW_ENCODE_END(geometrySpan);
    const Texture *zeta = pass_.depthStencil.texture;
    if (compatible_ != pl) {
        for (uint32_t j = 0; j < 8; j++) {  // the pass's attachments are the formats the state was made for
            const Texture *t = j < pass_.count ? pass_.color[j].texture : nullptr;
            if ((t ? t->format : 0) != pl->colorFormats[j] && !(t && pl->colorFormats[j] == 0)) {
                *why = "a state whose color formats differ from the pass's";
                cb_->refusal = {RefColorFormats, j, t ? t->format : 0, pl->colorFormats[j]};
                return false;
            }
        }
        if (!fragmentTestsMatch(pl->rec->storageTests, ds_, zeta ? zeta->format : 0)) {
            *why = "an ordered storage variant whose depth/stencil tests differ from the draw";
            cb_->refusal = {RefDepthStencil, pl->rec->storageTests,
                            uint8_t(fragmentTests(ds_, zeta ? zeta->format : 0)), 0};
            return false;
        }
        // A pass that declares no attachment tests depth and stencil against its hidden one (PassDesc::hiddenZeta), which the
        // pass is given when its recorded states need it; the pipeline was made for the attachment the pass declares
        const Texture *declared = hiddenZeta_ ? nullptr : zeta;
        if (pl->depthFormat != (declared ? declared->format : 0) || (!zeta && testsHiddenZeta(ds_))) {
            *why = "a depth/stencil state or format the pass's depth/stencil attachment does not take";
            cb_->refusal = {RefDepthStencil, pl->depthFormat, declared ? declared->format : 0, ds_.active()};
            return false;
        }
        if (pl->samples != samples_ || !sampleShadingMasks(samples_, pl->samplePasses, compatibleSampleMasks_)) {
            *why = "a state whose samples a pixel or per-sample shading the pass's attachments do not take";
            cb_->refusal = {RefSamples, pl->samples, samples_, pl->samplePasses};
            return false;
        }
        if (rot_.turned() && pl->turnRefused) {
            *why = rotatedRefusalText(RotatedRefusal::Variant);
            cb_->refusal = {RefRotated, uint32_t(RotatedRefusal::Variant), rot_.turn};
            return false;
        }
        compatible_ = pl;
    }
    if (!prepareShaderTests(*pl, why))
        return false;
    if (p_.room() < 1024 + pl->methods.size() + (cb_->extrasLive_ ? cb_->extrasLive_->restore.size() : 0) +
                        (views_.dirtyViewports | views_.dirtyScissors ? 24 * kMaxViewports : 0))
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
    const bool dsChanged = dsDirty_;
    if (dsDirty_) {
        RAW_ENCODE_SPAN(Bind);
        const bool depth = zeta && zetaHasDepth(zeta->format), stencil = zeta && zetaHasStencil(zeta->format);
        if (shaderTestsActive_)
            emitDepthStencil(p, shaderDepthStencilRegs(depth), cb_->ds_);
        else if (bindDepthStencil(p, bodies_, bodyStart_,
                                  dsState_->regs(depth, stencil, stencilRef_[0], stencilRef_[1]), cb_->ds_,
                                  bindsBySegment() ? dsState_->segment(depth, stencil) : StateSegment())) {
            cb_->segmentBinds_++;
            RAW_DIAGNOSTIC(cb_->counts_.segmentBinds++);
        }
        dsDirty_ = false;
    }
    if (zcull_.candidate && (dsChanged || pl != zcullProgram_)) {
        zcullProgram_ = pl;
        if (zcullInvalidates(zcull_, ds_, zeta && zetaHasDepth(zeta->format), zeta && zetaHasStencil(zeta->format),
                             pl->fragmentDepth, shaderTestsActive_ != 0)) {
            p.m(kInvalidateZcull, 0);
            RAW_DIAGNOSTIC(d.stats.zcullInvalidations++);
        }
    }
    uint32_t *root = cb_->rootPush_;
    const PackPipeline &r = *pl->rec;
    // set 0 first (it may refuse the draw before anything is written): the layout's entries only
    if (!pl->ubosNamed) {
        *why = "a program reads a set-0 uniform buffer its record does not name";
        cb_->refusal = {};
        return false;
    }
    if (pl->heapMissing) {
        *why = "no memory for the tables a state reads argument buffers through";
        cb_->refusal = {};
        return false;
    }
    if (pl->tessReadsHeap) {
        *why = "a tessellation program that reads argument buffers through set 1";
        cb_->refusal = {};
        return false;
    }
    if ((r.spillEntry[0] && !pl->spill[0]) || (r.spillEntry[1] && !pl->spill[1])) {
        *why = "an argument table whose words its record does not hold";
        cb_->refusal = {};
        return false;
    }
    const uint32_t setBytes = renderSetBytes(r.setBytes);
    if (setBytes > kRenderSetBytes) {
        *why = "a set 0 past the raw path's 512 bytes";
        cb_->refusal = {};
        return false;
    }
    RAW_ENCODE_BEGIN(descriptorSpan, Descriptors);
    const auto *prepared = descriptors_.resolve(
        pl, movedEntries(r), pl->memoryEntries,
        [&](PreparedDescriptorWords &prepared, uint32_t moved) {
            RAW_ENCODE_COUNT(DescriptorBuilds);
            uint32_t *set = prepared.words.data();
            for (uint32_t i = 0; i < r.entries; i++) {
                if (!(moved >> i & 1))
                    continue;
                const PackEntry &e = r.entry[i];
                uint32_t *at = set + e.offset / 4;
                if (e.kind == EntrySpillTable)
                    continue;
                if (e.kind == EntryColorTable) {
                    at[0] = uint32_t(d.colorTable_.va);
                    at[1] = uint32_t(d.colorTable_.va >> 32);
                    at[2] = kColorTableBytes;
                    at[3] = 0;
                    continue;
                }
                if (e.kind == EntryTextureTable || e.kind == EntrySamplerTable) {
                    const bool textures = e.kind == EntryTextureTable;
                    const uint64_t va = textures ? d.heapTables_.textureTableVa() : d.heapTables_.samplerTableVa();
                    at[0] = uint32_t(va);
                    at[1] = uint32_t(va >> 32);
                    at[2] = textures ? d.heapTables_.textureTableBytes() : d.heapTables_.samplerTableBytes();
                    at[3] = 0;
                    continue;
                }
                if (e.kind == EntryUbo || e.kind == EntryTessBuffer) {
                    const Bound b = e.kind == EntryUbo ? bufs_[e.stage][e.slot] : tessBuffer(e.slot);
                    cb_->buffersWritten_ |= bufferStorage(e);
                    if (b.buffer)
                        cb_->use(*b.buffer, bufferStorage(e));
                    else if (!b.named || bufferStorage(e))
                        cb_->useAddress(b.va, b.size, bufferStorage(e));
                    BufferDescriptor desc;
                    if (!bufferDescriptor(e, b.va, b.size, desc)) {
                        *why = bufferStorage(e)
                                   ? "a storage buffer bound at an offset that is not a multiple of 16, with no "
                                     "residual push word"
                                   : "a uniform buffer bound at an offset that is not a multiple of 256, with no "
                                     "residual push word";
                        cb_->refusal = {RefUboOffset, uint32_t(e.stage) << 8 | e.slot, desc.residual,
                                        uint32_t(std::min<uint64_t>(b.offset, ~0u))};
                        return false;
                    }
                    // a word that held a residual takes the slot's next one even where it is 0: the set keeps its
                    // words from the draws before
                    if (const int word = bufferResidualWord(e); spilledWord(word)) {
                        prepared.spilled[e.stage == 1 ? 0 : 1].set(word, desc.residual);
                    } else if (word >= 0 && (desc.residual || (prepared.residualMask >> word & 1))) {
                        prepared.residuals[word] = desc.residual;
                        prepared.residualMask |= 1u << word;
                    }
                    if (b.buffer && b.buffer->hostVisible && !b.buffer->wc)
                        cb_->clean(b.buffer->cpu + b.offset, std::min<uint64_t>(desc.range - desc.residual, b.size));
#if SWITCH_RAW_DIAGNOSTICS
                    if (pl->uniformProbe && b.cpu)
                        probeUniforms(*pl, e.stage, e.slot, b.cpu,
                                      std::min<uint64_t>(desc.range - desc.residual, b.size));
#endif
                    at[0] = uint32_t(desc.base);
                    at[1] = uint32_t(desc.base >> 32);
                    at[2] = b.va ? desc.range : 0;
                    at[3] = 0;
                    continue;
                }
                RAW_ENCODE_SPAN(Textures);
                uint32_t tic = 0, tsc = 0;
                if (e.kind == EntryDepthTest || e.kind == EntryStencilTest) {
                    const bool depth = e.kind == EntryDepthTest;
                    const Texture *t = depth ? shaderDepth_ : shaderStencil_;
                    const uint32_t view = depth ? r.shaderTests.depthTic : r.shaderTests.stencilTic;
                    if (!t || t->samples != samples_ || texelBytes(view) != t->bpp || !(tic = passViewTic(*t, view))) {
                        *why = "an unavailable native shader depth/stencil view";
                        cb_->refusal = {RefDepthStencil, e.kind, t ? t->format : 0, samples_};
                        return false;
                    }
                    cb_->use(*t, true);
                } else if (e.kind == EntryTexture) {
                    const Texture *t = boundTexture(e);
                    if (t && !t->tic) {
                        *why = "a texture the raw path cannot sample";
                        cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
                        return false;
                    }
                    // a two-plane texture's header pair where the program reads it so, else its stand-in
                    const bool xrSlot = e.stage == 2 && (pl->xrSlots >> e.slot & 1);
                    const bool pair = t && t->xrPlanes && xrSlot;
                    if (!pair && xrSlot) {
                        *why = "a state that reads a two-plane texture's header pair drawn without one";
                        cb_->refusal = {RefNoTic, e.slot, t ? t->format : 0, t ? t->width : 0};
                        return false;
                    }
                    const bool written = handleStage(e) == 1 && (pl->writtenSlots >> e.slot & 1);
                    // tic is the image a draw samples: a stand-in's, a sampled shadow's or a plane pair's is not the
                    // texture's own memory
                    const bool heldApart = t && (t->standIn || t->sampled || pair);
                    if (written && t && (heldApart || t->format != pl->rec->writtenFormat[e.slot])) {
                        *why = heldApart ? "a written texture the raw path holds through another image"
                                         : "a written texture of another format than its program stores";
                        cb_->refusal = {RefWrittenTexture, e.slot, t->format, pl->rec->writtenFormat[e.slot]};
                        return false;
                    }
                    if (pair) {
                        cb_->use(*t->luma, false);
                        cb_->use(*t->chroma, false);
                        RAW_DIAGNOSTIC(dev_->stats.xrPairBinds++);
                    } else if (t && written) {
                        cb_->use(*t, true);
                    } else if (t) {
                        cb_->use(t->standIn ? *t->standIn : t->sampled ? *t->sampled : *t, false);
                    }
                    tic = !t ? 0 : pair ? t->tic : t->standIn ? t->standIn->tic : t->tic;
                } else if (e.kind == EntryFetch) {
                    const ColorAttachment &a = pass_.color[e.slot];
                    const Texture *t = a.texture;
                    const bool storage = t && (pl->storageBits >> e.slot & 1);
                    if (storage && storageViewMismatch(texelBytes(pl->storageTic[e.slot]), pl->rec->storageSamples, *t,
                                                       dev_->storageCompressedOk()) != StorageFallback::None) {
                        *why = "an incompatible framebuffer storage view";
                        cb_->refusal = {RefStorageView, e.slot, t->format, t->width};
                        return false;
                    }
                    if (t) {
                        cb_->use(*t, pl->storageWriteBits >> e.slot & 1);
                        tic = fetchTic(a, storage ? pl->storageTic[e.slot] : 0, layeredRecord(pl->rec->arrayLengthWord),
                                       why);
                        if (!tic) {
                            cb_->refusal = {storage ? RefStorageView : RefNoTic, e.slot, t->format, t->width};
                            return false;
                        }
                    }
                } else if (e.kind == EntryTexture3d) {
                    // a texture of another type there is no binding of the argument: m2v writes its neutral, raw an empty handle
                    const Texture *bound = boundTexture(e);
                    const Texture *t = bound && bound->type == fx::kTexThreeD ? bound : nullptr;
                    if (t && !t->tic) {
                        *why = "a texture the raw path cannot sample";
                        cb_->refusal = {RefNoTic, e.slot, t->format, t->width};
                        return false;
                    }
                    const bool written = handleStage(e) == 1 && (pl->writtenSlots >> e.slot & 1);
                    if (written && t && t->format != pl->rec->writtenFormat[e.slot]) {
                        *why = "a written texture of another format than its program stores";
                        cb_->refusal = {RefWrittenTexture, e.slot, t->format, pl->rec->writtenFormat[e.slot]};
                        return false;
                    }
                    if (t)
                        cb_->use(*t, written);
                    tic = t ? t->tic : 0;
                }
                const Texture *mapped = e.kind == EntryTexture ? boundTexture(e) : nullptr;
                const uint8_t map = mapped ? mapped->addressMap : 0;
                if (e.samplerKind == SamplerSlot) {
                    const Sampler *s = boundSampler(e);
                    const bool compares = entryCompares(e);
                    if (s && map && compares) {
                        *why = "a planar texture sampled by comparison";
                        return false;
                    }
                    tsc = s          ? (map        ? s->mapped[map]
                                        : compares ? s->compareTsc
                                                   : s->tsc)
                          : compares ? d.neutralCompareTsc_
                                     : d.neutralTsc_;
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
            return fillSpills(*pl, moved, prepared, set, why);
        },
        [&](const DescriptorWords &words) {
            RAW_ENCODE_COUNT(DescriptorUploads);
            uint64_t address = 0;
            uint8_t *upload = d.alloc(d.upload_, setBytes, 256, address);
            memcpy(upload, words.data(), setBytes);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            RAW_DIAGNOSTIC(cb_->counts_.uploadBytes += setBytes);
#endif
            return address;
        },
        setBytes);
    RAW_ENCODE_END(descriptorSpan);
    if (!prepared)
        return false;
    moved_ = MovedBindings{{}, {}, {}, false};
    spillDirty_ = false;
    const uint32_t *set = prepared->words.data();
    const auto &residuals = prepared->residuals;
    const uint32_t residualMask = prepared->residualMask;
    const uint64_t setVa = prepared->address;
    prepared_ = pl;
    preparedSet_ = setVa;
    RAW_ENCODE_BEGIN(bindSpan, Bind);
    writeRaster(p, rasterFor(primitive));
    const bool dynamicBlend = r.blendColorWord || r.halfBlendColorWord;
    const bool slotRoots = r.rootSlots[0] || r.rootSlots[1];
    const bool patches = tessellates(r);
    const bool drawnWordsDirty = ((r.amplificationIdWord || r.amplificationCountWord || pl->amplificationLayerWord ||
                                   pl->amplificationViewportWord) &&
                                  amplificationDirty_) ||
                                 r.pulledStride[0][1] || r.arrayLengthWord || r.viewportCountWord;
    if (pl != emitted_ || residualMask || residualsPushed_ || (dynamicBlend && blendColorDirty_) ||
        r.shaderTests.flags || slotRoots || (patches && tessWordsDirty_) || drawnWordsDirty) {
        if (pl != emitted_) {
            blendColorDirty_ |= pl->constantBlend || dynamicBlend;
            if (cb_->extrasLive_ && cb_->extrasLive_ != pl) {
                p.raw(cb_->extrasLive_->restore.data(), uint32_t(cb_->extrasLive_->restore.size()));
                cb_->extrasLive_ = nullptr;
                cb_->heldSegment_.lose();
            }
            // the iterated blend unit refuses a draw binding more than one colour target (class error 0x59)
            const uint32_t targets = colorTargets();
            const bool narrow = pl->freeForm.rung == FetchIteratedBlend && targets > 1;
            if (narrow != narrowed_) {
                p.m(CT_SELECT, targetSelect(narrow ? 1 : targets));
                narrowed_ = narrow;
            }
            bindState(p, *pl);
            clipHeld_ = pl->viewportClip;
            clampRangeHeld_ = clampRangeHeld_ && !pl->namesViewportClipRange;
            // a vertex program's Layer selects the slice where it writes one (NVK: the last pre-rasterization stage's)
            if (const uint32_t rtLayer = pl->writesLayer ? kRtLayerFromShader : 0; rtLayer != cb_->rtLayer_) {
                p.m(NVB197_SET_RT_LAYER, rtLayer);
                cb_->rtLayer_ = rtLayer;
            }
            RAW_DIAGNOSTIC(cb_->counts_.blendKillBinds += pl->blendKill);
            if (!pl->restore.empty())
                cb_->extrasLive_ = pl;
        }
        // the state's push words into the root table, as m2v's vkCmdPushConstants ranges reach it through NVK: the words its
        // ranges cover (the rest keep what an earlier state wrote), the draw's residuals in theirs, one LOAD_CONSTANT_BUFFER per
        // run of words that change
        uint32_t want[32];
        memcpy(want, pl->push, sizeof want);
        if (patches) {
            controlWords(r, want);
            tessWordsDirty_ = false;
        }
        drawnWords(*pl, want);
        for (uint32_t m = residualMask; m; m &= m - 1)
            want[__builtin_ctz(m)] = residuals[__builtin_ctz(m)];
        if (!blendWords(r, why))
            return false;
        patchBlendPush(r, blendColor_, blendWords_, want);
        patchShaderTestPush(r.shaderTests, ds_, stencilRef_, depthRange_, false, want);
        for (uint32_t k = 0; k < 2; k++)
            if (const uint8_t word = r.rootTableWord[k]; r.rootSlots[k] && word && word <= 31) {
                const uint64_t table = rootTable(r, k);
                want[word - 1] = uint32_t(table);
                want[word] = uint32_t(table >> 32);
            }
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
    RAW_ENCODE_END(bindSpan);
    if (!writeClipControl(p, *pl, why))
        return false;
    if (samplePasses_ != pl->samplePasses) {
        p.m(LOAD_CB_OFFSET, kRootSampleInfo + 8);
        p.nonInc(LOAD_CB, compatibleSampleMasks_, 4);
        samplePasses_ = pl->samplePasses;
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
        switch_gm20b_viewport_state v = viewportState(viewport_);
        turnViewport(rot_, v);
        kit(p, switch_gm20b_viewport, 0u, &v);
        viewportDirty_ = false;
    }
    if (scissorDirty_) {
        const Rect s = planeRect(scissor_[0] & 0xffff, scissor_[1] & 0xffff, scissor_[0] >> 16, scissor_[1] >> 16);
        kit(p, switch_gm20b_scissor, 0u, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
        scissorDirty_ = false;
    }
    if (views_.dirtyViewports | views_.dirtyScissors)
        emitViewports(p);
    // NVK's SET_BLEND_CONST_RED..ALPHA, the floats' bits, written where a draw blends with them
    if ((pl->constantBlend || dynamicBlend) && blendColorDirty_) {
        p.incArray(BLEND_CONST_RED, blendColor_, 4);
        blendColorDirty_ = false;
    }
    bindStreams(p, *pl, indexed, vFirst, count);
    bool selected = false;
    if (descriptorBinding_.changed(pl, setVa)) {
        const HeapSet *heap = pl->heapSet;
        const uint64_t heapVa = heap ? heap->memory.va : 0;
        // NVK's root table sets[0] and sets[1]: set 0, and set 1 for a heap array a program reads past its constant buffer
        const uint32_t s[8] = {uint32_t(setVa),        uint32_t(setVa >> 32),  setBytes, 0, uint32_t(heapVa),
                               uint32_t(heapVa >> 32), heap ? r.heapBytes : 0, 0};
        p.m(LOAD_CB_OFFSET, 0x38);
        p.nonInc(LOAD_CB, s, heap ? 8 : 4);
        if (pl->heapSlot[0] || pl->heapSlot[1]) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(r.heapBytes), uint32_t(heapVa >> 32), uint32_t(heapVa)});
            if (pl->heapSlot[0])
                p.m(BIND_GROUP_CB + 0x20 * 0, 1 | uint32_t(pl->heapSlot[0]) << 4);
            if (pl->heapSlot[1])
                p.m(BIND_GROUP_CB + 0x20 * 4, 1 | uint32_t(pl->heapSlot[1]) << 4);
            selected = true;
        }
        const bool setRead = pl->setSlot[0] || pl->setSlot[1] || pl->setSlot[2] || pl->setSlot[3];
        if (setRead) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(setBytes), uint32_t(setVa >> 32), uint32_t(setVa)});
            for (uint32_t k = 0; k < 4; k++)
                if (pl->setSlot[k])
                    p.m(BIND_GROUP_CB + 0x20 * kSetGroups[k], 1 | uint32_t(pl->setSlot[k]) << 4);
            selected = true;
        }
    }
    // NVK_MME_BIND_CBUF_DESC's bindings, read from cached CPU words rather than the WC upload.
    for (uint8_t u = 0; u < pl->uboCount; u++) {
        const Pipeline::UboCbuf &c = pl->ubos[u];
        const uint32_t *desc = set + r.entry[c.entry].offset / 4;
        if (!descriptorBinding_.uboMoved(u, desc))
            continue;
        if (desc[2]) {
            p.inc(CB_SELECTOR_A, {cbufBindBytes(desc[2]), desc[1], desc[0]});
            p.m(BIND_GROUP_CB + 0x20 * c.group, 1 | uint32_t(c.slot) << 4);
            selected = true;
        } else {
            p.m(BIND_GROUP_CB + 0x20 * c.group, uint32_t(c.slot) << 4);
        }
    }
    if (selected)
        p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(d.root_.va >> 32), uint32_t(d.root_.va)});
    if (pl->splitOf)
        launchSplit(p, *pl, set);
    else if (launchOnly)
        launchSplit(p, *launchOnly, set);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (cb_->splitCapturing_ && twin->splitOf && !indexed)
        captureSplit(*pl, geometryAnalysis, primitive, vFirst, count);
#endif
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
    RAW_ENCODE_BEGIN(barrierSpan, Barriers);
    const AttachmentAccess access{pl->readBits, pl->writeBits, uint8_t(pl->fetchBits & ~pl->storageBits),
                                  pl->storageBits};
    if (firstDraw && !indirectGeometry_ && !pl->dropsPixels) {
        const uint8_t opaque = pl->writeBits & ~(pl->readBits | pl->fetchBits | pl->storageBits);
        if (opaque && geometryAnalysis.covers(primitive == 3, areaW_, areaH_))
            covered_ = opaque;
    }
    const AttachmentBarrier needed = access.after(attachmentWrites_.rop, attachmentWrites_.storage);
    if (needed != AttachmentBarrier::None) {
        if (needed == AttachmentBarrier::Pixel && !written_.meets(geometryAnalysis)) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            RAW_DIAGNOSTIC(dev_->stats.fetchBarriersElided++);
#endif
#ifdef SWITCH_RAW_HARNESS_MARKS
            // the host frame harness's builds only (test/build.sh): nvdec names this mark ELIDED, and eqcheck takes a
            // barrier NVK has and raw left out only where it is
            p.m(NO_OPERATION, 0x0e11de);
#endif
        } else {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            RAW_DIAGNOSTIC(dev_->stats.fetchBarriers++);
#endif
            if (needed == AttachmentBarrier::Shader) {
                RAW_DIAGNOSTIC(d.stats.storageBarriers++);
                tiledCacheDrain(p, d.k_.info());
                p.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
                p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
            } else {
                fetchBarrier(p);
            }
            attachmentWrites_.synchronized(needed);
            written_.clear();
        }
    }
    RAW_ENCODE_END(barrierSpan);
#if SWITCH_RAW_DIAGNOSTICS
    if (pipeline_ && pipeline_->caName)
        probeDraw(*pipeline_, *pl, primitive, first, count, indexed, {x[0], y[0], x[1], y[1]});
#endif
    RAW_DIAGNOSTIC(cb_->counts_.interlockDraws += pl->storageBits != 0);
#if SWITCH_RAW_DIAGNOSTICS
    if (pl->rec && pl->storageBits)
        dev_->stats.fetch.interlockKeys.note(pl->rec->keyHash);
    if (pl->rec && pl->genericState)
        dev_->stats.fetch.genericKeys.note(pl->rec->keyHash);
#endif
    RAW_DIAGNOSTIC(cb_->counts_.interlocked += (pl->fetchBits & pl->storageBits) != 0);
    RAW_DIAGNOSTIC(cb_->counts_.interlockedBlends += pl->storageBlendBits != 0);
    attachmentWrites_.rop |= pl->writeBits;
    attachmentWrites_.storage |= pl->storageWriteBits;
    if (pl->writeBits)
        written_.add(geometryAnalysis);
    // Reuse a bound already established for split/coverage/fetch decisions. Admission never triggers new CPU geometry
    // reads; unknown geometry uses the clipped viewport/scissor extent rather than the whole render target.
    drawBox_ = geometryAnalysis.cachedBox();
    drawPixels_ = drawBox_.empty() ? 0 : uint64_t(drawBox_.x1 - drawBox_.x0) * (drawBox_.y1 - drawBox_.y0);
    return pl->freeForm.matrixSlot == kNoUniform || matrixConstants(*pl, why);
}

// A colour matrix form's draw: its iterated blend program and constants from the matrix uniform the draw binds, which
// the GPU's macro reads through a non-prefetched GPFIFO entry over the uniform's bytes (the CPU reads none of them). A
// uniform of the rows alone (a QuartzCore whose matrix has no clamp) is followed by a clamp pair of 0 in an entry of its
// own: the macro's method data runs on across the entries, each but the last incomplete, which the kernel takes only
// before a non-prefetched entry.
bool RenderEncoder::matrixConstants(const Pipeline &pl, const char **why)
{
    static_assert(kMatrixWords == SWITCH_GM20B_MACRO_VCM_ARGUMENTS, "the colour matrix macro's arguments");
    const Bound &b = bufs_[2][pl.freeForm.matrixSlot];
    const uint32_t words = pl.freeForm.matrixWords;
    if (!b.va || uint64_t(pl.freeForm.matrixOffset) + 4 * words > b.size || ((b.va + pl.freeForm.matrixOffset) & 3)) {
        *why = "a colour matrix form's draw binds no uniform buffer that holds the matrix";
        cb_->refusal = {};
        return false;
    }
    if (b.buffer && b.buffer->hostVisible && !b.buffer->wc && b.cpu)
        cb_->clean(b.cpu + pl.freeForm.matrixOffset, 4 * words);
    mme::matrixHeader(p_);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    const uint32_t clampWords = kMatrixWords - words;
    uint64_t clampVa = 0;
    if (clampWords) {
        auto *clamp = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 4 * clampWords, 4, clampVa));
        std::fill(clamp, clamp + clampWords, 0u);
    }
    matrixCall(bodies_, bodyStart_, p_.at, b.va + pl.freeForm.matrixOffset, words, clampVa, clampWords);
    bodyStart_ = nullptr;
    reserve();
    return true;
}

// What the region test reads of a draw: Core Animation's transform (Pipeline::caTransform) over a triangle list or strip
// of one instance, with its matrix, positions and indices where the CPU maps them; anything else is its scissor alone
uint32_t RenderEncoder::movedEntries(const PackPipeline &rec) const
{
    if (moved_.all)
        return ~0u;
    uint32_t moved = 0;
    for (uint32_t i = 0; i < rec.entries; i++) {
        const PackEntry &e = rec.entry[i];
        const bool buffer = e.kind == EntryUbo && e.stage < 3 && e.slot < 32 && (moved_.buffers[e.stage] >> e.slot & 1);
        const bool texture = (e.kind == EntryTexture || e.kind == EntryTexture3d) && e.slot < kStageTextures &&
                             (moved_.textures[handleStage(e)] >> e.slot & 1);
        const bool sampler = e.kind != EntryUbo && e.samplerKind == SamplerSlot && e.sampler < kStageSamplers &&
                             (moved_.samplers[handleStage(e)] >> e.sampler & 1);
        const bool spill =
            e.kind == EntrySpillTable && e.stage < 3 && (moved_.buffers[e.stage] || (e.stage == 2 && spillDirty_));
        moved |= uint32_t(buffer || texture || sampler || spill) << i;
    }
    return moved;
}

// The extended blend colour's words (raw_blend_color.h) where `r` reads them, made again after the colour moved
bool RenderEncoder::blendWords(const PackPipeline &r, [[maybe_unused]] const char **why)
{
    if (!r.blendColorWord || !blendWordsDirty_)
        return true;
    float color[4];
    memcpy(color, blendColor_, sizeof color);
    blendColorWords(color, blendWords_);
    blendWordsDirty_ = false;
    return true;
}

// `pl`'s argument tables (raw_spill_table.h) whose entries moved: the record's words with the spilled bind offsets'
// residuals and a fragment table's dynamic words, uploaded where they differ from the encoder's last upload, named in set 0
bool RenderEncoder::fillSpills(const Pipeline &pl, uint32_t moved, PreparedDescriptorWords &prepared, uint32_t *set,
                               const char **why)
{
    const PackPipeline &r = *pl.rec;
    Device &d = *dev_;
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t entry = r.spillEntry[k], count = r.spillWords[k];
        if (!entry || entry > r.entries || !(moved >> (entry - 1) & 1))
            continue;
        uint32_t words[kSpillWordsMax];
        composeSpill(pl.spill[k], count, prepared.spilled[k], words);
        if (k == 1) {
            if (!blendWords(r, why))
                return false;
            patchBlendPush(r, blendColor_, blendWords_, words, kRootPushWords, count);
            patchShaderTestPush(r.shaderTests, ds_, stencilRef_, depthRange_, false, words, kRootPushWords, count);
        }
        const uint64_t va = spillUploads_[k].resolve(words, count, [&](const uint32_t *from, uint32_t n) {
            RAW_ENCODE_COUNT(DescriptorUploads);
            uint64_t address = 0;
            uint8_t *upload = d.alloc(d.upload_, spillRange(n), 256, address);
            memcpy(upload, from, n * sizeof(uint32_t));
            return address;
        });
        spillDescriptor(va, count, set + r.entry[entry - 1].offset / 4);
    }
    return true;
}

DrawGeometry RenderEncoder::geometry(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count,
                                     const PixelBox &limit)
{
    DrawGeometry g;
    g.limit = limit;
    g.viewport = {vp_[0], vp_[1], vp_[2], vp_[3]};
    g.first = first;
    g.count = count;
    g.indices = drawIndices_;
    g.indexSize = drawIndices_ ? drawIndexSize_ : 0;
    g.strip = primitive == 4;
    const Bound &m = bufs_[1][2];
    const Bound &v = pl.rec->vertexSlot < 31 ? bufs_[1][pl.rec->vertexSlot] : bufs_[1][0];
    g.bounded = !indirectGeometry_ && pl.caTransform && pl.positionComponents && (primitive == 3 || primitive == 4) &&
                m.cpu && m.size >= 64 && v.cpu && pl.rec->vertexSlot < 31;
    g.bounded &= !g.indices || drawIndexCpuSafe_ || drawIndexBuffer_;
    g.owner = cb_;
    g.inputs = {m.buffer, v.buffer, g.indices && !drawIndexCpuSafe_ ? drawIndexBuffer_ : nullptr};
    g.acquire = [](void *owner, const void *input, const uint8_t *cpu, uint64_t bytes) {
        return static_cast<CommandBuffer *>(owner)->acquireGeometry(*static_cast<const Buffer *>(input), cpu, bytes);
    };
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
    kit(p, switch_gm20b_pixel_shader_barrier);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void RenderEncoder::countRules(const Pipeline &pl)
{
    if (pl.fetchBits && anyWritten_) {
        RAW_DIAGNOSTIC(dev_->stats.fetchBarriersAnyColor++);
        anyWritten_ = false;
    }
    anyWritten_ |= pl.writeBits != 0;
    const uint8_t stored = pl.fetchBits & pl.writeBits, ropWrites = pl.writeBits & ~pl.fetchBits;
    if ((pl.fetchBits & ropWritten_) || (ropWrites & storageWritten_)) {
        RAW_DIAGNOSTIC(dev_->stats.fetchBarriersInterlocked++);
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
using CopySide = CopySurface;

// z: a 3D texture's plane, or an array's layer (its own 2D image, layerBytes on: NVK's array_stride step)
CopySide sideOf(const Texture &t, uint32_t x, uint32_t y, uint32_t z)
{
    if (!t.blockLinear)
        return {t.va + uint64_t(y) * t.pitch + uint64_t(x) * t.bpp, t.pitch};
    if (t.import && t.import->kernelTiled())
        return {t.va, t.pitch, x + t.surfaceX, y + t.surfaceY, t.import->layout.height, true, t.blockHeight};
    if (t.layers > 1)
        return {t.va + z * t.layerBytes, t.pitch, x, y, t.height, true, t.blockHeight};
    return {t.va, t.pitch, x, y, t.height, true, t.blockHeight, t.depth, z, t.blockDepth};
}

// SET_REMAP_COMPONENTS of a texel copy: DST_X..W = SRC_X..W, COMPONENT_SIZE, NUM_SRC_COMPONENTS, NUM_DST_COMPONENTS
uint32_t texelRemap(uint32_t bpp)
{
    return copyTexelRemap(bpp);
}

// SET_REMAP_COMPONENTS of one-, two- or four-byte components (`size`): DST_X..W each SRC_X..W (0..3) or NO_WRITE (6), the
// source's and destination's component counts (clb0b5)
constexpr uint32_t kRemapNoWrite = NVB0B5_SET_REMAP_COMPONENTS_DST_X_NO_WRITE;

constexpr uint32_t remapOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t src, uint32_t dst)
{
    return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (src - 1) << 20 | (dst - 1) << 24;
}

// `w` x `h` texels by nouveau_copy_rect (raw_kit.h): a remap of components (`bytes` of each texel moved). `ordered`: after
// every earlier copy has completed (one that reads what the copy before it wrote)
void copyRemapped(Push &p, const CopySide &s, const CopySide &d, uint32_t w, uint32_t h, uint32_t remap, uint32_t bytes,
                  [[maybe_unused]] Device::Stats &stats, bool ordered = false)
{
    if (!w || !h || !bytes)
        return;
    RAW_DIAGNOSTIC(stats.ceOps++);
    RAW_DIAGNOSTIC(stats.ceBytes += uint64_t(w) * bytes * h);
    emitCopy(p, s, d, w, h, remap, ordered);
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

#if SWITCH_RAW_DIAGNOSTICS
// The blur-texture probe's reads, after everything else the command buffer does: its draws' writes reach memory (the tiled
// cache's flush, the wait for idle) before the copy engine reads each row into a ring slot, read at a later begin() once
// this submission's fence passed. A full ring (no begin() between eight submissions) drops the record.
void CommandBuffer::glassMemTail()
{
    Device &d = *dev_;
    GlassMemCb record =
        d.glassMem->close(d.commitCount_, mach_absolute_time(), d.stats.memEvents.load(std::memory_order_relaxed));
    glassMemSlot_ = -1;
    for (uint32_t i = 0; i < record.count; i++) {
        GlassMemItem &it = record.items[i];
        if (const Texture *t = glassMemSources_[i]; t && t->arena && !t->view && !t->span.valid())
            it.flags |= kGlassMemAliased | kGlassMemUnsampled;
    }
    const GlassMemTailPlan plan = glassMemTailPlan(d.glassMemLive_, record);
    if (!plan.ring)
        return;
    record.launches = plan.launches;
    if (!d.glassMemRing_.va) {
        if (!d.k_.allocControl(uint64_t(kGlassMemSlots) * kGlassMemSlotBytes, d.glassMemRing_,
                               ControlUse::TransferSlot))
            return;
        memset(d.glassMemRing_.cpu, 0, uint64_t(kGlassMemSlots) * kGlassMemSlotBytes);
    }
    int slot = -1;
    for (uint32_t s = 0; s < kGlassMemSlots && slot < 0; s++)
        if (!d.glassMemBusy_[s])
            slot = int(s);
    if (slot < 0)
        return;
    const uint32_t words = 24 + record.count * 256;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, words * 4, 256, va));
    Push p{start, start + words};
    tiledCacheDrain(p, d.k_.info());
    p.m(WAIT_FOR_IDLE, 0);
    const uint64_t base = d.glassMemRing_.va + uint64_t(slot) * kGlassMemSlotBytes;
    const uint64_t cookie = base + uint64_t(kGlassMemItems) * kGlassMemItemBytes, begun = cookie + kGlassMemStartAt;
    p.inc(HOST_SEMAPHORE_A, {uint32_t(begun >> 32) & 0xff, uint32_t(begun), glassMemCookie(record.serial),
                             HOST_SEMAPHORE_RELEASE_16_WFI});
    memset(d.glassMemRing_.cpu + uint64_t(slot) * kGlassMemSlotBytes, kGlassMemPoison,
           uint64_t(record.count) * kGlassMemItemBytes);
    for (uint32_t i = 0; i < record.count; i++) {
        const GlassMemItem &it = record.items[i];
        if (it.flags & kGlassMemUnsampled)
            continue;
        const Texture &image = glassMemImages_[i];
        const uint64_t row = base + uint64_t(i) * kGlassMemItemBytes;
        copySides(p, sideOf(image, it.inX, it.inY, 0), {row, it.texels * it.bpp}, it.texels, 1, it.bpp, d.stats);
        if (it.outside)
            copySides(p, sideOf(image, it.outX, it.outY, 0), {row + kGlassMemRowBytes, it.texels * it.bpp}, it.texels,
                      1, it.bpp, d.stats);
        for (uint32_t q = 0; it.quarters && q < 2; q++)
            copySides(p, sideOf(image, it.inX, it.height * (1 + 2 * q) / 4, 0),
                      {row + (2 + q) * kGlassMemRowBytes, it.texels * it.bpp}, it.texels, 1, it.bpp, d.stats);
    }
    p.inc(HOST_SEMAPHORE_A, {uint32_t(cookie >> 32) & 0xff, uint32_t(cookie), glassMemCookie(record.serial),
                             HOST_SEMAPHORE_RELEASE_16_WFI});
    checkRoom(p, "the glass memory probe's reads");
    segment(start, p.at);
    if (d.k_.exactJobs())
        surfaces_.add(d.glassMemRing_.handle, true);
    d.glassMemCbs_[slot] = record;
    glassMemSlot_ = slot;
}

// At begin(), where the probe is armed or was: armed again after a pause it starts with no history (what was written
// meanwhile went unseen); still armed, the command buffer starts its record; disarmed, the rows in flight are read out
// and nothing else runs until it is armed again.
void Device::glassMemFrame()
{
    const uint64_t now = mach_absolute_time();
    bool on = glassMem && !glassMemStopped.load(std::memory_order_relaxed) && glassMemArming.on(now);
    if (!on) {
        glassMemOn.store(false, std::memory_order_relaxed);
        on = glassMem && !glassMemStopped.load(std::memory_order_relaxed) && glassMemArming.on(now);
        if (on)
            glassMemOn.store(true, std::memory_order_relaxed);
    }
    if (on && !glassMemLive_)
        glassMem->reset();
    glassMemLive_ = on;
    glassMemRead(now);
    if (on)
        glassMem->begin();
}

// The probe's records whose rows landed, oldest first (a reused texture's row is judged against the one its last write
// left): a line for each that found something, or for each while HW Test armed it, and a summary every 5 s, each with
// what the probe's own reads cost. A killed submission stops the probe; a record a reopened channel's fence never
// reaches is dropped after 120 command buffers.
void Device::glassMemRead(uint64_t now)
{
    const os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
    const bool every = glassMemArming.every(now);
    for (;;) {
        int next = -1;
        for (uint32_t s = 0; s < kGlassMemSlots; s++)
            if (glassMemBusy_[s] && (next < 0 || glassMemCbs_[s].serial < glassMemCbs_[next].serial))
                next = int(s);
        if (next < 0)
            break;
        if (!done(glassMemCbs_[next].seqno)) {
            if (commitCount_ - glassMemCbs_[next].serial <= 120)
                break;
            glassMemBusy_[next] = false;
            glassMemBusyCount_--;
            continue;
        }
        glassMemBusy_[next] = false;
        glassMemBusyCount_--;
        if (lost(glassMemCbs_[next].seqno)) {
            if (!glassMemStopped.exchange(true))
                os_log_error(log,
                             "raw: glassmem: command buffer %llu carrying the probe's reads was killed: the probe is "
                             "off for this process",
                             (unsigned long long)glassMemCbs_[next].serial);
            continue;
        }
        uint8_t rows[kGlassMemSlotBytes];
        memcpy(rows, glassMemRing_.cpu + uint64_t(next) * kGlassMemSlotBytes, sizeof rows);
        const uint32_t cookie = glassMemCookie(glassMemCbs_[next].serial);
        StampRelease begun, ended;
        memcpy(&ended, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes, sizeof ended);
        memcpy(&begun, rows + uint64_t(kGlassMemItems) * kGlassMemItemBytes + kGlassMemStartAt, sizeof begun);
        const uint64_t probeNs = begun.payload == cookie && ended.payload == cookie && ended.ptimer >= begun.ptimer
                                     ? ptimerNanoseconds(ended.ptimer - begun.ptimer)
                                     : 0;
        for (const std::string &line : glassMem->finish(glassMemCbs_[next], rows, every, probeNs))
            os_log(log, "raw: glassmem %{public}s", line.c_str());
    }
    if (!glassMemLive_ || every || now - glassMemSummaryAt_ < glassProbeTicks(5000))
        return;
    const GlassMemCounts &c = glassMem->counts;
    const uint64_t cbs = c.cbs.load() - glassMemSummaryCbs_, per = cbs ? cbs : 1;
    if (cbs)
        os_log(log,
               "raw: glassmem totals cbs %llu rebuilt %llu reused %llu zero-reused %llu nf %llu chg %llu mv %llu ev "
               "%llu probe ce %.1f/cb gpu %.0fus/cb",
               (unsigned long long)c.cbs.load(), (unsigned long long)c.rebuilt.load(),
               (unsigned long long)c.reused.load(), (unsigned long long)c.black.load(),
               (unsigned long long)c.nonFinite.load(), (unsigned long long)c.changed.load(),
               (unsigned long long)c.moved.load(), (unsigned long long)stats.memEvents.load(),
               double(c.launches.load() - glassMemSummaryLaunches_) / double(per),
               double(c.probeNs.load() - glassMemSummaryNs_) / 1000.0 / double(per));
    glassMemSummaryAt_ = now;
    glassMemSummaryCbs_ = c.cbs.load();
    glassMemSummaryLaunches_ = c.launches.load();
    glassMemSummaryNs_ = c.probeNs.load();
}

// A GPU-memory event, numbered: the render server's lines are what a reused texture's /e<n> counts
void Device::glassMemEvent(const char *what, uint64_t bytes)
{
    const uint64_t n = ++stats.memEvents;
    if (glassMemOn.load(std::memory_order_relaxed))
        os_log(os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw"),
               "raw: glassmem event %llu %{public}s %llu KiB", (unsigned long long)n, what,
               (unsigned long long)(bytes >> 10));
}
#endif

bool RenderEncoder::prepareShaderTests(const Pipeline &pipeline, const char **why)
{
    const uint8_t flags = pipeline.rec->shaderTests.flags;
    if (!flags) {
        endShaderTests();
        return true;
    }
    const Texture *zeta = pass_.depthStencil.texture;
    ShaderTestShape copy;
    if (!zeta || !shaderTestShape(zeta->format, samples_, planeArea().x1, planeArea().y1, flags, copy)) {
        *why = "a shader depth/stencil test whose attachment cannot be copied exactly";
        cb_->refusal = {RefDepthStencil, zeta ? zeta->format : 0, flags, samples_};
        return false;
    }
    const uint8_t missing = flags & ~shaderTestsActive_;
    if (!missing)
        return true;
    auto obtain = [&](Texture *&slot, uint32_t format, uint32_t tag) {
        if (slot)
            return true;
        Texture shape;
        shape.width = zeta->width;
        shape.height = zeta->height;
        shape.format = format;
        shape.samples = samples_;
        if (!(slot = dev_->lastBacking(shape, tag, true, false))) {
            RAW_ENCODE_LOCK(lock, dev_->lock_);
            slot = dev_->backing(shape, tag, true, false);
        }
        if (!slot)
            return false;
        cb_->backings_.push_back(slot);
        cb_->use(*slot, true);
        invalidateDescriptors();
        return true;
    };
    constexpr uint32_t kR32Float = 55, kR8Uint = 13, kRG8Unorm = 30;
    if (((missing & kShaderTestDepth) && !obtain(shaderDepth_, kR32Float, Device::kShaderDepthTag)) ||
        ((missing & kShaderTestStencil) && !obtain(shaderStencil_, kR8Uint, Device::kShaderStencilTag)) ||
        ((missing & kShaderTestStencil) && copy.packedStencil &&
         !obtain(shaderStencilTemp_, kRG8Unorm, Device::kShaderStencilTempTag))) {
        *why = "no native backing for shader depth/stencil tests";
        cb_->refusal = {RefDepthStencil, zeta->format, flags, samples_};
        return false;
    }
    copyShaderTests(missing, false);
    shaderTestsActive_ |= missing;
    dsDirty_ = true;
    return true;
}

void RenderEncoder::copyShaderTests(uint8_t flags, bool toAttachment)
{
    if (p_.room() < 256)
        reserve();
    const Texture &zeta = *pass_.depthStencil.texture;
    ShaderTestShape shape;
    shaderTestShape(zeta.format, samples_, planeArea().x1, planeArea().y1, flags, shape);
    auto side = [](const Texture &texture) {
        auto result = sideOf(texture, 0, 0, 0);
        result.height = sampleRows(texture);
        return result;
    };
    const CopySide attachment = side(zeta);
    cb_->use(zeta, toAttachment);
    tiledCacheDrain(p_, dev_->k_.info());
    p_.m(WAIT_FOR_IDLE, 0);
    if (toAttachment)
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
    const auto image = [&](ShaderTestImage which) {
        switch (which) {
        case ShaderTestImage::depth:
            return side(*shaderDepth_);
        case ShaderTestImage::stencil:
            return side(*shaderStencil_);
        case ShaderTestImage::temporary:
            return side(*shaderStencilTemp_);
        case ShaderTestImage::attachment:
            return attachment;
        }
        return attachment;
    };
    const auto copies = shaderTestTransfers(flags, shape.packedStencil, toAttachment);
    for (uint8_t at = 0; at < copies.count; ++at) {
        const auto &copy = copies.transfers[at];
        copyRemapped(p_, image(copy.source), image(copy.destination), shape.width, shape.height, copy.remap, copy.bytes,
                     dev_->stats, copy.ordered);
    }
    RAW_DIAGNOSTIC(dev_->stats.compZetaCopies += zeta.comptags);
    p_.inc(CE_LINE_LENGTH_IN, {0, 0}, SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, (kLaunchPitch & ~3u) | 2u | (1u << 10), SUBC_COPY);
    p_.m(INVALIDATE_SHADER_CACHES, kInvalidateData);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
}

void RenderEncoder::endShaderTests()
{
    if (!shaderTestsActive_)
        return;
    copyShaderTests(shaderTestsActive_, true);
    shaderTestsActive_ = 0;
    dsDirty_ = true;
}

// Clear [x0, x1) x [y0, y1) of target `target` to `v` (its format's clear words), with depth (bit 0) and stencil (bit 1)
// of `zsBits`, in each of `layers` layers, whatever the render enable says
static void clearRect(Push &p, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const uint32_t v[4], uint32_t target,
                      uint32_t zsBits, uint32_t layers)
{
    switch_gm20b_clear_state s = {};
    for (uint32_t i = 0; i < 4; i++)
        s.value[i] = v[i];
    s.xmin = uint16_t(x0);
    s.xmax = uint16_t(x1);
    s.ymin = uint16_t(y0);
    s.ymax = uint16_t(y1);
    s.target = target;
    s.depth = zsBits & 1;
    s.stencil = zsBits & 2;
    s.layers = layers;
    s.layer_macro = mme::kClearLayers;
    kit(p, switch_gm20b_clear, &s);
}

void RenderEncoder::clearRegion(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const float rgba[4])
{
    const Texture &t = *pass_.color[0].texture;
    uint32_t v[4] = {};
    clearWords(t.format, rgba, v, t.compressed);
    dev_->clearZbc(t, !x0 && !y0 && x1 >= t.width && y1 >= t.height, v);
    if (p_.room() < 64)
        reserve();
    clearRect(p_, x0, y0, x1, y1, v, 0, 0, std::max(pass_.layers, 1u));
    called_ = true;
}

// A pass of `layers` layers binds that many from the target's own (pitch memory: one)
static void colorTarget(Push &p, uint32_t index, const Texture &t, uint32_t layers)
{
    const uint32_t rows = t.import && t.import->kernelTiled() ? t.import->layout.height : sampleRows(t);
    ColorTargetDesc d = colorTargetDesc(t, formatInfo(t.format)->rt, rows);
    d.layers = layers;
    d.depth = t.type == fx::kTexThreeD;
    d.blockDepth = t.blockDepth;
    const switch_gm20b_target k = kitTarget(d);
    kit(p, switch_gm20b_color_target, index, &k, t.comptags);
}

// A slot with no target below the pass's last one, as NVK writes it: FORMAT DISABLED, VA 0, compressed. Left as it was,
// the slot would still name the target of an earlier pass, which CT_SELECT's count makes the hardware render to.
static void disabledTarget(Push &p, uint32_t index, uint32_t layers)
{
    switch_gm20b_target k = {};
    k.width = 64;
    k.layers = layers;
    kit(p, switch_gm20b_color_target, index, &k, true);
}

uint32_t Device::attachmentTic(Texture &t, const Texture &part, uint32_t level, uint32_t slice)
{
    if (level && !slice && t.chain && t.type == fx::kTexTwoD && part.format == t.format)
        return levelTic(t, level);
    // above every key viewTic keeps in views_ (word 0, a sample grid below bit 48)
    const uint64_t key = 1ull << 63 | uint64_t(part.format) << 32 | uint64_t(slice) << 4 | level;
    if (!t.tic)
        return 0;
    if (const uint32_t seen = views_.find(t.tic, key))
        return seen;
    const FormatInfo *f = formatInfo(part.format);
    uint32_t w[8];
    if (!f || !attachmentHeader(part, f->tic0, f->srgb, w))
        return 0;
    RAW_ENCODE_LOCK(l, lock_);
    const auto made = partTics_.equal_range(t.tic);
    for (auto m = made.first; m != made.second; ++m)
        if (m->second.first == key) {
            views_.keep(t.tic, key, m->second.second);
            return m->second.second;
        }
    const uint32_t tic = allocTic();
    if (!tic)
        return 0;
    memcpy(tic_.cpu + 32 * tic, w, 32);  // visible at the next submission's header invalidate
    partTics_.emplace(t.tic, std::make_pair(key, tic));
    views_.keep(t.tic, key, tic);
    return tic;
}

uint32_t Device::levelTic(Texture &t, uint32_t level)
{
    const FormatInfo *f = formatInfo(t.format);
    if (!t.chain || !f || level >= t.chain->layout.levels)
        return 0;
    uint32_t &slot = t.chain->levelTic[level];
    if (const uint32_t made = __atomic_load_n(&slot, __ATOMIC_ACQUIRE))
        return made;
    RAW_ENCODE_LOCK(l, lock_);
    uint32_t tic = slot;
    if (!tic && (tic = allocTic())) {
        uint32_t w[8];
        fx::mipChainTic(t.chain->layout, t.chain->va, f->tic0, f->srgb, level, 1, w);
        memcpy(tic_.cpu + 32 * tic, w, 32);  // visible at the next submission's header invalidate
        __atomic_store_n(&slot, tic, __ATOMIC_RELEASE);
    }
    return tic;
}

uint32_t RenderEncoder::passViewTic(const Texture &t, uint32_t word0)
{
    AttachmentView v;
    v.word0 = word0;
    for (uint8_t i = 0; i < passViewCount_; ++i)
        if (passViews_[i].ownTic == t.tic && passViews_[i].view == v)
            return passViews_[i].tic;
    const uint32_t tic = dev_->viewTic(t, word0, ViewAccess::Storage);
    if (tic && passViewCount_ < passViews_.size())
        passViews_[passViewCount_++] = {t.tic, v, tic};
    return tic;
}

uint32_t RenderEncoder::passViewTic(const Texture &owner, const AttachmentView &v, const char **why)
{
    for (uint8_t i = 0; i < passViewCount_; ++i)
        if (passViews_[i].ownTic == owner.tic && passViews_[i].view == v)
            return passViews_[i].tic;
    const uint32_t tic = dev_->attachmentViewTic(owner, v, why);
    if (tic && passViewCount_ < passViews_.size())
        passViews_[passViewCount_++] = {owner.tic, v, tic};
    return tic;
}

uint32_t RenderEncoder::fetchTic(const ColorAttachment &a, uint32_t storage, bool layeredRecord, const char **why)
{
    const Texture &t = *a.texture;
    const bool layered = pass_.layers > 1;
    if (t.tic && (storage ? !layeredRecord : !layered))
        return storage ? passViewTic(t, storage) : t.tic;
    const FormatInfo *f = formatInfo(t.format);
    if (!f) {
        *why = "framebuffer fetch from a target the raw path cannot sample";
        return 0;
    }
    AttachmentView v;
    v.word0 = storage ? storage : f->tic0;
    v.srgb = !storage && f->srgb;
    v.arrayed = storage ? layeredRecord : layered;
    v.layers = v.arrayed ? pass_.layers : 1;
    v.part = a.image != nullptr;
    v.level = uint8_t(a.level);
    v.slice = a.slice;
    return passViewTic(v.part ? *a.image : t, v, why);
}

uint32_t Device::viewTic(const Texture &t, uint32_t word0, ViewAccess access)
{
    uint8_t sxLog2 = 0, syLog2 = 0;
    if (access == ViewAccess::Storage && t.samples > 1) {
        fx::MipChain grid;
        if (!fx::msLayout(t.width, t.height, t.samples, t.bpp, grid))
            return 0;
        sxLog2 = grid.sxLog2;
        syLog2 = grid.syLog2;
    }
    const uint64_t key = word0 | uint64_t(sxLog2) << 32 | uint64_t(syLog2) << 40;
    if (!t.tic)
        return 0;
    if (const uint32_t seen = views_.find(t.tic, key))
        return seen;
    RAW_ENCODE_LOCK(l, lock_);
    const auto views = viewTics_.equal_range(t.tic);
    for (auto v = views.first; v != views.second; ++v)
        if (v->second.first == key) {
            views_.keep(t.tic, key, v->second.second);
            return v->second.second;
        }
    const uint32_t tic = allocTic();
    if (!tic)
        return 0;
    uint32_t own[8], w[8];
    memcpy(own, tic_.cpu + 32 * t.tic, 32);
    viewHeader(own, word0, w, sxLog2, syLog2);
    memcpy(tic_.cpu + 32 * tic, w, 32);  // visible at the next submission's header invalidate
    viewTics_.emplace(t.tic, std::make_pair(key, tic));
    views_.keep(t.tic, key, tic);
    return tic;
}

uint32_t Device::attachmentViewTic(const Texture &owner, const AttachmentView &v, const char **why)
{
    RAW_ENCODE_LOCK(l, lock_);
    constexpr const char *kNoHeader = "framebuffer fetch from a target the raw path cannot sample";
    if (!owner.tic) {
        *why = kNoHeader;
        return 0;
    }
    const auto views = attachmentViewTics_.equal_range(owner.tic);
    for (auto it = views.first; it != views.second; ++it)
        if (it->second.first == v)
            return it->second.second;
    uint32_t w[8];
    bool headed = false;
    if (v.part) {
        fx::MipChain c;
        uint64_t base = 0;
        headed = attachmentImage(owner, c, base) && attachmentViewHeader(c, base, owner.type == fx::kTexThreeD, v, w);
    } else {
        uint32_t own[8];
        memcpy(own, tic_.cpu + 32 * owner.tic, 32);
        headed = ownViewHeader(own, v, w);
    }
    if (!headed) {
        *why = kNoHeader;
        return 0;
    }
    const uint32_t tic = allocTic();
    if (!tic) {
        *why = "no room for the framebuffer storage view";
        return 0;
    }
    memcpy(tic_.cpu + 32 * tic, w, 32);  // visible at the next submission's header invalidate
    attachmentViewTics_.emplace(owner.tic, std::make_pair(v, tic));
    return tic;
}

static bool chainLevelView(const Texture &t, uint32_t level, uint32_t rtFormat, Texture &out)
{
    ColorTargetDesc d;
    if (!t.chain || !chainLevelDesc(t.chain->layout, t.chain->va, level, rtFormat, d))
        return false;
    out = Texture();
    out.storage = t.storageRange();
    out.va = d.va;
    out.width = t.chain->layout.level[level].width;
    out.height = d.height;
    out.format = t.format;
    out.pitch = d.pitch;
    out.bpp = t.bpp;
    out.blockLinear = true;
    out.blockHeight = d.blockHeight;
    out.layerBytes = t.chain->layout.layers > 1 ? t.chain->layout.arrayStride : d.layerBytes;
    out.layers = t.chain->layout.layers;
    if (t.type == fx::kTexThreeD) {  // the level's planes in its own blocks' depth (the copy engine's 3D addressing)
        out.type = fx::kTexThreeD;
        out.depth = t.chain->layout.level[level].depth;
        out.blockDepth = t.chain->layout.level[level].zLog2;
    }
    out.blockW = t.blockW;
    out.blockH = t.blockH;
    out.texelWidth = std::max(1u, t.texelWidth >> level);
    out.texelHeight = std::max(1u, t.texelHeight >> level);
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

TextureTransferResult Device::transferTexture(const Texture &texture, uint32_t level, uint32_t slice,
                                              const uint32_t origin[3], const uint32_t size[3], void *bytes,
                                              uint64_t row, uint64_t image, bool write, std::shared_ptr<void> owner)
{
    Texture view;
    const Texture *selected = &texture;
    if (texture.chain) {
        if (!chainLevelImage(texture, level, view))
            return TextureTransferResult::Unsupported;
        selected = &view;
    } else if (level) {
        return TextureTransferResult::Unsupported;
    }
    const Texture &t = *selected;
    if (!t.va || t.zeta || t.samples > 1 || t.converted() || t.blockW > 1 || t.blockH > 1 ||
        xrForm(t.format) != XrForm::None || !size[0] || !size[1] || !size[2] || origin[0] > t.width ||
        size[0] > t.width - origin[0] || origin[1] > t.height || size[1] > t.height - origin[1])
        return TextureTransferResult::Unsupported;
    const bool volume = t.type == fx::kTexThreeD;
    if (volume ? (slice || origin[2] > t.depth || size[2] > t.depth - origin[2])
               : (slice >= std::max(1u, t.layers) || origin[2] || size[2] != 1))
        return TextureTransferResult::Unsupported;
    TextureTransfer region;
    region.texture = sideOf(t, origin[0], origin[1], volume ? origin[2] : slice);
    region.width = size[0];
    region.height = size[1];
    region.planes = size[2];
    region.bpp = t.bpp;
    if (!transfers_.copy(k_, set_, lock_, region, bytes, row, image, write, std::move(owner)))
        return TextureTransferResult::Failed;
    RAW_DIAGNOSTIC((write ? stats.textureUploads : stats.textureReadbacks).fetch_add(1, std::memory_order_relaxed));
#if SWITCH_RAW_DIAGNOSTICS
    if (write && glassMemOn.load(std::memory_order_relaxed))
        glassMem->wrote(texture.storageRange().va ? texture.storageRange().va : texture.va,
                        commitCount_.load(std::memory_order_relaxed), stats.memEvents.load(std::memory_order_relaxed));
#endif
    return TextureTransferResult::Complete;
}

bool Device::copyCallerPages(const Texture &t, std::shared_ptr<void> owner)
{
    if (!t.callerPages)
        return true;
    if (!transfers_.copyBytes(k_, set_, lock_, t.callerPages->bo.va, t.imagePitchVa, t.storage.bytes, std::move(owner)))
        return false;
    RAW_DIAGNOSTIC(stats.textureUploads.fetch_add(1, std::memory_order_relaxed));
    return true;
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
void RenderEncoder::emitSwizzle(Push &p, uint32_t word)
{
    kit(p, switch_gm20b_viewport_swizzle, 0u, word);
}

void RenderEncoder::timestampAtStart(uint64_t va, counters::Point point)
{
    if (startStampCount_ == startStamps_.size())
        return;
    startStamps_[startStampCount_++] = {va, point};
    cb_->useAddress(va, counters::kRecordBytes, true);
}

void RenderEncoder::continuePass(uint8_t stores)
{
    // Clear reach is a Metal encoder property, including its empty-draw behavior.
    // Later encoders must not enlarge or elide the first encoder's clear.
    if (!clearsSealed_) {
        clears(initialClears_);
        clearsSealed_ = true;
    }
    for (uint32_t i = 0; i < 8; ++i)
        pass_.color[i].store = stores & (1u << i);
    pipeline_ = nullptr;
    for (auto &stage : bufs_)
        for (auto &binding : stage)
            binding = {};
    for (auto &stage : textures_)
        std::fill(std::begin(stage), std::end(stage), nullptr);
    for (auto &stage : samplers_)
        std::fill(std::begin(stage), std::end(stage), nullptr);
    invalidateDescriptors();
    setDepthStencil(nullptr);
    setStencilReference(0, 0);
    setViewport(0, 0, areaW_, areaH_, 0, 1);
    setScissor(0, 0, areaW_, areaH_);
    raster_ = {};
    depthClamp_ = false;
    setDepthBias(0, 0, 0);
    const float blend[4] = {};
    setBlendColor(blend);
    RAW_DIAGNOSTIC(dev_->stats.fusedPasses++);
}

#if SWITCH_RAW_DIAGNOSTICS
// The pass's stored colours that it leaves a row or two of (logEdgeMiss): outside its render area, or outside every one of its
// draws' scissors within their viewports; a Clear load reaches past both
void RenderEncoder::probeEdges() const
{
    PixelBox drawn{areaW_, areaH_, 0, 0};
    for (const auto &r : rects_)
        drawn = {std::min(drawn.x0, r[0]), std::min(drawn.y0, r[1]), std::max(drawn.x1, r[2]),
                 std::max(drawn.y1, r[3])};
    const bool drawnKnown = drew_ && !whole_ && !rects_.empty();
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const ColorAttachment &a = pass_.color[j];
        if (!a.texture || !a.store || memoryless_[j] || a.load == ColorAttachment::Clear)
            continue;
        char detail[96];
        snprintf(detail, sizeof detail, " c%u of %u load %c area %ux%u drew %d", j, pass_.count, "dlc"[a.load], areaW_,
                 areaH_, int(drew_));
        logEdgeMiss("pass.area", *a.texture, {0, 0, areaW_, areaH_}, detail);
        if (drawnKnown)
            logEdgeMiss("pass.draws", *a.texture, drawn, detail);
    }
}
#endif

void RenderEncoder::end()
{
    RAW_ENCODE_SPAN(PassEnd);
    Device &d = *dev_;
    endShaderTests();
    if (p_.room() < 64 + kSampleWords + 2 * kMaxViewports + kDrawStampWords)
        reserve();
    writeRaster(p_, Raster());
    unclampDepthRange(p_);
    if (rot_.turned()) {
        emitSwizzle(p_, kSwizzleIdentity);
        for (uint32_t turned = views_.swizzled & ~1u; turned; turned &= turned - 1)
            kit(p_, switch_gm20b_viewport_swizzle, uint32_t(__builtin_ctz(turned)), kSwizzleIdentity);
    }
    // the draw parameters the next encoder starts from: the root table's and the base registers, 0
    if (params_.unknown || params_.root || params_.baseInstance) {
        const uint32_t zero[2] = {};
        p_.m(LOAD_CB_OFFSET, 0);
        p_.nonInc(LOAD_CB, zero, 2);
        p_.m(GLOBAL_BASE_INSTANCE_INDEX, 0);
    }
    if (params_.unknown || params_.baseVertex) {
        p_.m(GLOBAL_BASE_VERTEX_INDEX, 0);
        p_.m(VERTEX_ID_BASE, 0);
    }
    if (samples_ > 1)
        sampleWords(p_, 1, d.root_.va);
    // the 3D engine's work outside the pass (the blit encoder's draws) meets no iterated blend and the pass's targets
    if (const Pipeline *live = cb_->extrasLive_; live && live->freeForm.rung == FetchIteratedBlend) {
        p_.raw(live->restore.data(), uint32_t(live->restore.size()));
        cb_->extrasLive_ = nullptr;
    }
    if (narrowed_) {
        p_.m(CT_SELECT, targetSelect(colorTargets()));
        narrowed_ = false;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const bool flush = cb_->forms_.keepsTiledFlush();
    if (drew_ && !flush && tiledCache(d.k_.info()))
        cb_->taken_.flushes++;
#else
    constexpr bool flush = true;
#endif
    const bool measured = measurePass(drew_ && flush);
    if (drew_ && flush)
        tiledCacheDrain(p_, d.k_.info());
    if (measured)
        emitDrawStamp(p_, d.stamps_.va, candidate_.slot, true, candidate_.payload);
    if (attachmentWrites_.storage)
        p_.m(INVALIDATE_SHADER_CACHES, kFlushData | kInvalidateData);
    checkRoom(p_, "a pass body");
    if (p_.at > bodyStart_)
        bodies_.push_back({bodyStart_, p_.at});
    d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - d.push_.bo.cpu);  // give back the unused reservation
    // pass prologue: the full barrier m2v puts between passes, the targets, then the clears
    RAW_ENCODE_BEGIN(prologueSpan, PassPrologue);
    if (!clearsSealed_)
        clears(initialClears_);
    const std::vector<Clear> &cl = initialClears_;
    const uint32_t layers = std::max(pass_.layers, 1u);
#if SWITCH_RAW_DIAGNOSTICS
    if (d.glassMemLive_)
        glassMemReached(cl);
#endif
#if SWITCH_RAW_DIAGNOSTICS
    probeEdges();
#endif
    const std::vector<uint32_t> &restore = extrasAtBegin_ ? extrasAtBegin_->restore : std::vector<uint32_t>();
    const uint32_t room = 160 + 8 * 32 + uint32_t(cl.size()) * 48 + kZetaTargetWords + kZcullRegionWords +
                          kZetaClearWords + 16 + kSampleWords + kTurnWordsMethods + uint32_t(restore.size()) +
                          kSpanMarkWords + startStampCount_ * counters::kTimestampWords + kDrawStampWords;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, room * 4, 256, va));
    Push p{start, start + room};
    p.raw(restore.data(), uint32_t(restore.size()));
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    probePassPrologue(p, cb_->forms_, cb_->lastEngine_, cb_->taken_);
    cb_->lastEngine_ = ProbeEngine::ThreeD;
#else
    kit(p, switch_gm20b_pass_barrier, false);
#endif
    for (uint8_t i = 0; i < startStampCount_; i++)
        counters::emit(p, startStamps_[i]);
    if (rot_.turned()) {
        emitSwizzle(p, rot_.swizzle());
        turnWordsInto(p, d.root_.va, rot_);
    }
    // ahead of the clears: a clear reaches every sample of its rectangle's pixels through it
    if (samples_ > 1)
        sampleWords(p, samples_, d.root_.va, pass_.positions, rot_);
    // shadow targets' pool images: their texels over the render area first, as NVK's tiled shadow loads them (a load that
    // keeps them: Load, and a DontCare the clears leave; a Clear clears it all), then the barrier to the pass
    bool copied = false;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (const Texture *o = shadowOf_[j]) {
            if (pass_.color[j].load == ColorAttachment::Clear || attachmentStartsCleared(j, areaW_, areaH_, cl))
                continue;
            cb_->use(*o, false);
            cb_->use(*pass_.color[j].texture, true);
            const Rect a = planeArea();
            copySides(p, sideOf(*o, a.x0, a.y0, 0), sideOf(*pass_.color[j].texture, a.x0, a.y0, 0), a.x1 - a.x0,
                      a.y1 - a.y0, o->bpp, d.stats);
            RAW_DIAGNOSTIC(d.stats.compCopies += pass_.color[j].texture->comptags);
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
        const Rect r = planeRect(c.x0, c.y0, c.x1, c.y1);
        colorTarget(p, 0, *t, layers);
        kit(p, switch_gm20b_render_area, 1u, r.x1, r.y1);
        p.m(ZT_SELECT, 0);
        uint32_t v[4] = {};
        clearWords(pass_.color[c.target].texture->format, c.value, v,
                   !called_ && pass_.color[c.target].texture->compressed);
        d.clearZbc(*t, !r.x0 && !r.y0 && r.x1 >= t->width && r.y1 >= t->height, v);
        clearRect(p, r.x0, r.y0, r.x1, r.y1, v, 0, 0, layers);
        own = true;
    }
    if (own) {
        kit(p, switch_gm20b_pass_barrier, false);
    }
    uint32_t count = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (pass_.color[j].texture)
            count = j + 1;
    for (uint32_t j = 0; j < count; j++) {
        if (const Texture *t = pass_.color[j].texture)
            colorTarget(p, j, *t, layers);
        else
            disabledTarget(p, j, layers);
    }
    // targets map to themselves (NVK writes all eight slots of the map; the hardware reads the first `count`), drawn from
    // (0, 0) over the render area
    kit(p, switch_gm20b_render_area, count, planeArea().x1, planeArea().y1);
    const DepthStencilAttachment &zs = pass_.depthStencil;
    zetaTargetWords(p, zs.texture, layers);
    // ahead of the depth clear, which sets the region; over the plane the pass renders (a turned drawable's own axes)
    const Rect plane = planeArea();
    if (ZcullRegion region; zcull_.candidate && zcullRegion(d.zcullInfo_, plane.x0, plane.y0, plane.x1 - plane.x0,
                                                            plane.y1 - plane.y0, region)) {
        zcullRegionWords(p, region, zs.texture->format, zcull_.dir);
        RAW_DIAGNOSTIC(d.stats.zcullPasses++);
    } else if (d.zcullOn_) {
        zcullOffWords(p);
    }
    // NVK clears depth and stencil with the first color the pass clears over its render area, else on their own
    uint32_t zsBits = zs.texture ? zetaClearBits(zs.clearDepth && zetaHasDepth(zs.texture->format),
                                                 zs.clearStencil && zetaHasStencil(zs.texture->format))
                                 : 0;
    zetaClearValues(p, zsBits & 1, zs.depth, zsBits & 2, zs.stencil);
    if (zsBits & 1)
        d.clearZbcDepth(*zs.texture, areaW_ >= zs.texture->width && areaH_ >= zs.texture->height, zs.depth);
    for (const Clear &c : cl) {
        if (c.own)
            continue;
        uint32_t v[4] = {};
        const Texture *t = pass_.color[c.target].texture;
        clearWords(t->format, c.value, v, !called_ && t->compressed);
        const Rect r = planeRect(c.x0, c.y0, c.x1, c.y1);
        d.clearZbc(*t, !r.x0 && !r.y0 && r.x1 >= t->width && r.y1 >= t->height, v);
        const bool area = !c.x0 && !c.y0 && c.x1 == areaW_ && c.y1 == areaH_;
        clearRect(p, r.x0, r.y0, r.x1, r.y1, v, c.target, area ? zsBits : 0, layers);
        zsBits = area ? 0 : zsBits;
    }
    if (zsBits) {
        const Rect a = planeArea();
        p.inc(CLEAR_RECT_H, {a.x1 << 16 | a.x0, a.y1 << 16 | a.y0});
        p.m(CLEAR_SURFACE_CONTROL, 0x10);
        p.m(RENDER_ENABLE_OVERRIDE, 1);
        if (layers > 1)
            mme::clearLayers(p, zsBits, layers);
        else
            p.m(CLEAR_SURFACE, zsBits);
        p.m(RENDER_ENABLE_OVERRIDE, 0);
    }
    if (measured)
        emitDrawStamp(p, d.stamps_.va, candidate_.slot, false, candidate_.payload);
    RAW_DIAGNOSTIC(markSpan(p, prologueMark_, SpanKind::Prologue));
    checkRoom(p, "a pass prologue");
    cb_->segment(start, p.at);
    d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p.at) - d.push_.bo.cpu);
    RAW_ENCODE_END(prologueSpan);
    [[maybe_unused]] const uint64_t bodyWords = appendBodies(cb_->segments_, bodies_, d.push_.bo.cpu, d.push_.bo.va);
    RAW_DIAGNOSTIC(d.stats.pushBytes += (bodyWords + uint64_t(p.at - start)) * 4);
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
            cb_->use(*pass_.color[j].texture, false);
            cb_->use(*o, true);
            const Rect r = planeRect(0, 0, w, h);
            copySides(q, sideOf(*pass_.color[j].texture, r.x0, r.y0, 0), sideOf(*o, r.x0, r.y0, 0), r.x1 - r.x0,
                      r.y1 - r.y0, o->bpp, d.stats);
            RAW_DIAGNOSTIC(d.stats.compCopies += pass_.color[j].texture->comptags);
        }
        checkRoom(q, "a pass's shadow stores");
        cb_->segment(at, q.at);
        d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(q.at) - d.push_.bo.cpu);
        RAW_DIAGNOSTIC(d.stats.pushBytes += uint64_t(q.at - at) * 4);
    }
}

// The pass's clears, before its draws: each Clear load, and each DontCare load m2v answers with a clear (metal2vulkan
// clear_reach.rs, from Metal's measured behaviour). The pass reaches no tile without a draw call, every tile with one and a
// stored Clear-loaded attachment, else the tiles (32 x 32 pixels) its drawing draws' scissors within their viewports touch. A
// DontCare-loaded memoryless color some draw reads (or that is stored) clears to zero where the pass reaches anything; a stored
// DontCare-loaded color some draw reads (blended into, written through a partial mask, fetched) clears to zero over what the
// pass reaches: the whole render area, or the reached tiles' runs along each row of tiles. Any other DontCare load loads.
void RenderEncoder::clears(std::vector<Clear> &out)
{
    out.clear();

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
    std::vector<bool> &tiles = tiles_;
    tiles.clear();
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
                const Rotation frame = rot_.turned() ? rot_ : frameOf(*a.texture);
                const bool called = called_ || pass_.drawn;
                rw = called ? std::min((w + kTile - 1) / kTile * kTile, frame.width) : frame.width;
                rh = called ? std::min((h + kTile - 1) / kTile * kTile, frame.height) : frame.height;
            }
            Clear c{j, 0, 0, rw, rh, a.clear};
            c.own = rw != w || rh != h;
            RAW_DIAGNOSTIC(dev_->stats.clears++);
            if (!c.own && (covered_ >> j & 1)) {
                RAW_DIAGNOSTIC(dev_->stats.clearsElided++);
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
    cb_->loops_.closeSegment(stream());  // a new reservation: the copies so far become a segment
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_->alloc(dev_->push_, 1 << 14, 256, va));
    p_ = Push{start_, start_ + (1 << 12)};
    cb_->loops_.openSegment(stream());
}

void BlitEncoder::copyBytes(uint64_t src, uint64_t dst, uint64_t bytes)
{
    if (bytes) {
        cb_->useAddress(src, bytes, false);
        cb_->useAddress(dst, bytes, true);
    }
    cb_->buffersWritten_ = true;
    for (uint64_t done = 0; done < bytes;) {
        room();
        const uint32_t n = uint32_t(std::min<uint64_t>(bytes - done, 1u << 17));
        p_.inc(CE_OFFSET_IN_UPPER,
               {uint32_t((src + done) >> 32), uint32_t(src + done), uint32_t((dst + done) >> 32), uint32_t(dst + done)},
               SUBC_COPY);
        p_.inc(CE_LINE_LENGTH_IN, {n, 1}, SUBC_COPY);
        p_.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);
        RAW_DIAGNOSTIC(dev_->stats.ceOps++);
        RAW_DIAGNOSTIC(dev_->stats.ceBytes += n);
        done += n;
    }
}

// SET_REMAP_COMPONENTS: DST_X..W CONST_A (4), COMPONENT_SIZE ONE (0) or FOUR (3) bytes, one source and destination
// component; LAUNCH_DMA: PIPELINED, FLUSH, pitch both sides, REMAP
namespace {
constexpr uint32_t kFillConstA = 0x4444, kFillMaxDim = 1u << 15;
constexpr uint32_t kFillLaunch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 10;
}  // namespace

void BlitEncoder::fillRun(uint64_t at, uint32_t width, uint32_t height, [[maybe_unused]] uint32_t unit)
{
    room();
    p_.inc(CE_OFFSET_IN_UPPER + 8, {uint32_t(at >> 32), uint32_t(at)}, SUBC_COPY);
    p_.inc(CE_LINE_LENGTH_IN, {width, height}, SUBC_COPY);
    p_.m(CE_LAUNCH_DMA, kFillLaunch | (height > 1 ? 1u << 9 : 0), SUBC_COPY);
    RAW_DIAGNOSTIC(dev_->stats.ceOps++);
    RAW_DIAGNOSTIC(dev_->stats.ceBytes += uint64_t(width) * height * unit);
}

void BlitEncoder::fillWords(uint64_t dst, uint64_t bytes, uint32_t word)
{
    if (bytes)
        cb_->useAddress(dst, bytes, true);
    cb_->buffersWritten_ = true;
    if (bytes < 4)
        return;
    room();
    p_.m(CE_SET_REMAP_CONST_A, word, SUBC_COPY);
    p_.m(CE_SET_REMAP_COMPONENTS, kFillConstA | 3u << 16, SUBC_COPY);
    p_.inc(CE_PITCH_IN, {kFillMaxDim * 4, kFillMaxDim * 4}, SUBC_COPY);
    for (uint64_t at = dst, left = bytes & ~uint64_t(3); left >= 4;) {
        const uint64_t words = left / 4;
        const uint32_t width = uint32_t(std::min<uint64_t>(words, kFillMaxDim)),
                       height =
                           words >= kFillMaxDim ? uint32_t(std::min<uint64_t>(words / kFillMaxDim, kFillMaxDim)) : 1;
        fillRun(at, width, height, 4);
        at += uint64_t(width) * height * 4;
        left -= uint64_t(width) * height * 4;
    }
}

void BlitEncoder::fillBytes(uint64_t dst, uint64_t bytes, uint8_t value)
{
    if (bytes)
        cb_->useAddress(dst, bytes, true);
    cb_->buffersWritten_ = true;
    auto bytesOf = [&](uint64_t at, uint64_t n) {
        if (!n)
            return;
        room();
        p_.m(CE_SET_REMAP_CONST_A, value, SUBC_COPY);
        p_.m(CE_SET_REMAP_COMPONENTS, kFillConstA, SUBC_COPY);
        fillRun(at, uint32_t(n), 1, 1);
    };
    const uint64_t end = dst + bytes, first = std::min((dst + 3) & ~uint64_t(3), end),
                   last = std::max(end & ~uint64_t(3), first);
    bytesOf(dst, first - dst);
    if (last > first)
        fillWords(first, last - first, uint32_t(value) * 0x01010101u);
    bytesOf(last, end - last);
}

void BlitEncoder::copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width,
                           uint32_t lines, uint32_t bpp)
{
    cb_->buffersWritten_ = true;
    if (!width || !lines || !bpp)
        return;
    cb_->useAddress(src, rowStorageBytes(srcPitch, width, lines, bpp), false);
    cb_->useAddress(dst, rowStorageBytes(dstPitch, width, lines, bpp), true);
    room();
    copyRows(p_, src, srcPitch, dst, dstPitch, width, lines, bpp, dev_->stats);
}

void BlitEncoder::copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                             uint32_t dy, uint32_t dz, uint32_t w, uint32_t h)
{
    cb_->use(src, false);
    cb_->use(dst, true);
    room(32);
    copySides(p_, sideOf(src, sx, sy, sz), sideOf(dst, dx, dy, dz), w, h, src.bpp, dev_->stats);
#if SWITCH_RAW_DIAGNOSTICS
    char detail[64];
    snprintf(detail, sizeof detail, " from %ux%u f%u at %u,%u", src.width, src.height, src.format, sx, sy);
    logEdgeMiss("copy", dst, {dx, dy, dx + w, dy + h}, detail);
#endif
}

void BlitEncoder::fillTexels(const Texture &t, uint32_t z, uint32_t w, uint32_t h, const CopyFill &fill)
{
    cb_->use(t, true);
    room(40);
    p_.m(CE_SET_REMAP_CONST_A, fill.constA, SUBC_COPY);
    p_.m(CE_SET_REMAP_CONST_B, fill.constB, SUBC_COPY);
    const CopySide side = sideOf(t, 0, 0, z);
    copyRemapped(p_, side, side, w, h, fill.remap, uint32_t(fill.componentBytes) * fill.components, dev_->stats);
}

void BlitEncoder::copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z,
                                   uint32_t w, uint32_t h, bool toTexture, uint8_t aspect)
{
    if (t.xrPlanes && t.luma)  // plane 0's words alone (raw_xr.h bufferTexelBytes): the alpha plane stays as it is
        return copyBufferTexels(va, pitch, *t.luma, x, y, z, w, h, toTexture, aspect);
    cb_->buffersWritten_ |= !toTexture;
    const uint32_t element = t.zeta && aspect == kAspectStencil ? 1
                             : t.zeta && aspect == kAspectDepth ? 4
                                                                : bufferTexelBytes(t.format, t.bpp);
    if (w && h)
        cb_->useAddress(va, rowStorageBytes(pitch, w, h, element), !toTexture);
    cb_->use(t, toTexture);
#if SWITCH_RAW_DIAGNOSTICS
    if (toTexture && !t.zeta)
        logEdgeMiss("upload", t, {x, y, x + w, y + h}, "");
#endif
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
        cb_->use(s, true);
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
    cb_->useAddress(t.planes.luma, rowStorageBytes(t.planes.lumaPitch, t.width, t.height, 1), false);
    cb_->useAddress(t.planes.chroma, rowStorageBytes(t.planes.chromaPitch, t.width, t.height, 2), false);
    cb_->use(t, true);
    RAW_DIAGNOSTIC(dev_->stats.ceOps += 2);
    RAW_DIAGNOSTIC(dev_->stats.ceBytes +=
                   uint64_t(t.width) * t.height * 4 * 2);  // (each launch writes the image's rows, through NO_WRITE)
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

void BlitEncoder::timestamp(uint64_t va, bool afterCopies)
{
    room(counters::kTimestampWords);
    cb_->useAddress(va, counters::kRecordBytes, true);
    counters::emitCopy(p_, va, afterCopies);
}

void BlitEncoder::generate(Texture &t, uint32_t fmt2d, uint32_t first, uint32_t last,
                           [[maybe_unused]] uint32_t unproven)
{
    Texture::Chain *c = t.chain;
    if (!c || !fmt2d || first >= last || last >= c->layout.levels)
        return;
    cb_->useAddress(c->va, c->layout.bytes, true);
    room(uint32_t(fx::generateLayersWords(c->layout, first, last)));
    fx::emitGenerateLayers(p_, c->layout, c->va, fmt2d, first, last);
    RAW_DIAGNOSTIC(dev_->stats.chainGenerates++);
    RAW_DIAGNOSTIC(dev_->stats.mipTwodLevels += last - first);
    RAW_DIAGNOSTIC(dev_->stats.mipTwodUnproven += unproven);
}

void BlitEncoder::end()
{
    checkRoom(p_, "a blit encoder");
    cb_->loops_.closeSegment(stream());
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    RAW_DIAGNOSTIC(dev_->stats.pushBytes += uint64_t(p_.at - start_) * 4);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
bool Device::probeCompressible(const tgm_layout &bl, Texture &t)
{
    const uint32_t kind = compressedKind(t.bpp, 0);
    Bo bo;
    if (k_.allocCompressible(compBytes(bl.bytes), bo) != KERN_SUCCESS)
        return false;
    if (k_.bindCompressed(bo, kind) != KERN_SUCCESS) {
        k_.free(bo);
        return false;
    }
    t.va = bo.va;
    t.pitch = bl.row_bytes;
    t.blockLinear = true;
    t.blockHeight = bl.y_log2;
    t.layerBytes = bl.bytes;
    t.comptags = true;
    if (!(t.tic = allocTic())) {
        k_.free(bo);
        return false;
    }
    probeCompressed[bo.va] = bo;
    writeTic(t);
    return true;
}

static ProbeSurface probeSurface(const Texture &t, uint32_t fmt2d, uint32_t width, uint32_t height)
{
    ProbeSurface s;
    s.va = t.va;
    s.fmt2d = fmt2d;
    s.pitch = t.pitch;
    s.width = width;
    s.height = height;
    s.blockLinear = t.blockLinear;
    s.blockHeight = t.blockHeight;
    return s;
}

void BlitEncoder::probe(const ProbeOp &op, const Texture *read, const Texture &written)
{
    room(uint32_t(probeOpWords(op)) + kProbeBracketWords);
    if (read)
        cb_->use(*read, false);
    cb_->use(written, true);
    if (op.bracketed) {
        tiledCacheDrain(p_, dev_->k_.info());
        p_.m(WAIT_FOR_IDLE, 0);
    }
    emitProbeOp(p_, op);
    if (op.bracketed)
        probeOpAfter(p_);
    cb_->taken_.ops++;
}

bool CommandBuffer::probeResolve(const Texture &multisampled, const Texture &into)
{
    fx::MipChain grid;
    const uint32_t fmt2d = probeTwodFormat(into.format);
    ProbeOp op;
    if (!fmt2d ||
        !fx::msLayout(multisampled.width, multisampled.height, multisampled.samples, multisampled.bpp, grid) ||
        !probeResolveOp(
            probeSurface(multisampled, fmt2d, multisampled.width << grid.sxLog2, multisampled.height << grid.syLog2),
            grid.sxLog2, grid.syLog2, probeSurface(into, fmt2d, into.width, into.height), op))
        return false;
    BlitEncoder *b = blit();
    b->probe(op, &multisampled, into);
    b->end();
    taken_.resolves++;
    return true;
}
#endif

}  // namespace raw
