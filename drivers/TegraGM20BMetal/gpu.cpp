// SPDX-License-Identifier: MIT
// See gpu.h. Method numbers: NVIDIA's class headers as Mesa ships them (src/nouveau/headers/nvidia/classes, MIT). The texture
// header fill follows Mesa NIL's nvb097_fill_image_view_desc (pitch), the sampler fill Mesa NVK's nvk_sampler_get_header, the
// copies NVK's nouveau_copy_rect and nvk_cmd_copy_memory_ce (all MIT), restricted to what Core Animation uses. r7-raw wrote the
// first version (render encoder, pack replay) and checked it against NVK's stream; r8-rawca added the lifetimes, the pool, the
// imports, the copies and the abortable command buffer.
#include "lock.h"
#include "creation_cost.h"
#include "gpu.h"
#include "pack_window.h"
#include "mme.h"
#include "owned.h"
#include "cbuf.h"
#include "ycbcr_store.h"
#include "slm.h"
#include "stage_switches.h"
#include "srgb_table.h"
#include "stamps.h"
#include "tiled_cache.h"

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#if SWITCH_METAL_DIAGNOSTICS
#include <mach/mach_time.h>
#endif
#include <mach/vm_page_size.h>
#include <os/log.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

namespace {
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

// Texture header word 0 per MTLPixelFormat as NVK writes it (pack/mkformats.py over the twin, 11-04), and the color target format
// NVK writes for a target of it (0 where m2v's NVK renders none): both NIL's rows (test/tic_words_test.py).
// The integer formats' are NIL's rows (nil_formats.csv: the class's RU*/RS* target, UINT or SINT components, alpha ONE_INT).
// A row commented with NIL's format ("Name: PIPE") is that row by NIL's rules (test/tic_words_test.py), its target the
// class's.
const FormatInfo kFormats[] = {
    {1, 0xf7, 0x2002491d, 1, false},     // A8Unorm
    {10, 0xf3, 0x7012491d, 1, false},    // R8Unorm
    {11, 0, 0x7012491d, 1, true},        // R8Unorm_sRGB
    {12, 0xf4, 0x7011249d, 1, false},    // R8Snorm: R8_SNORM
    {13, 0xf6, 0x6014921d, 1, false},    // R8Uint (NIL's R8_UINT: UINT components, alpha ONE_INT)
    {14, 0xf5, 0x60136d9d, 1, false},    // R8Sint
    {20, 0xee, 0x7012491b, 2, false},    // R16Unorm
    {22, 0xef, 0x7011249b, 2, false},    // R16Snorm: R16_SNORM
    {23, 0xf1, 0x6014921b, 2, false},    // R16Uint
    {24, 0xf0, 0x60136d9b, 2, false},    // R16Sint
    {25, 0xf2, 0x7017ff9b, 2, false},    // R16Float
    {30, 0xea, 0x70d24918, 2, false},    // RG8Unorm
    {31, 0, 0x70d24918, 2, false},       // RG8Unorm_sRGB (its bytes, sampled through its stand-in, srgb_pair.h)
    {32, 0xeb, 0x70d12498, 2, false},    // RG8Snorm: R8G8_SNORM
    {33, 0xed, 0x60d49218, 2, false},    // RG8Uint
    {34, 0xec, 0x60d36d98, 2, false},    // RG8Sint
    {40, 0xe8, 0x74e24915, 2, false},    // B5G6R5Unorm
    {41, 0, 0x272a4913, 2, false},       // A1BGR5Unorm
    {42, 0, 0x272a4912, 2, false},       // ABGR4Unorm
    {44, 0, 0x54e24912, 2, false},       // BGRA4Unorm: B4G4R4A4_UNORM (rendered through its stand-in)
    {43, 0xe9, 0x54e24914, 2, false},    // BGR5A1Unorm
    {53, 0xe4, 0x6014920f, 4, false},    // R32Uint
    {54, 0xe3, 0x60136d8f, 4, false},    // R32Sint
    {55, 0xe5, 0x7017ff8f, 4, false},    // R32Float
    {60, 0xda, 0x70d2490c, 4, false},    // RG16Unorm
    {62, 0xdb, 0x70d1248c, 4, false},    // RG16Snorm: R16G16_SNORM
    {63, 0xdd, 0x60d4920c, 4, false},    // RG16Uint
    {64, 0xdc, 0x60d36d8c, 4, false},    // RG16Sint
    {65, 0xde, 0x70d7ff8c, 4, false},    // RG16Float
    {70, 0xd5, 0x58d24908, 4, false},    // RGBA8Unorm (rendered: a 4:2:0 code texture's image)
    {71, 0xd6, 0x58d24908, 4, true},     // RGBA8Unorm_sRGB
    {72, 0xd7, 0x58d12488, 4, false},    // RGBA8Snorm: R8G8B8A8_SNORM
    {73, 0xd9, 0x58d49208, 4, false},    // RGBA8Uint
    {74, 0xd8, 0x58d36d88, 4, false},    // RGBA8Sint
    {80, 0xcf, 0x54e24908, 4, false},    // BGRA8Unorm
    {81, 0xd0, 0x54e24908, 4, true},     // BGRA8Unorm_sRGB
    {90, 0xd1, 0x58d24909, 4, false},    // RGB10A2Unorm (rendered: a 10-bit code texture's image)
    {91, 0xd2, 0x58d49209, 4, false},    // RGB10A2Uint
    {92, 0xe0, 0x78d7ffa1, 4, false},    // RG11B10Float
    {84, 0, 0x2b1a4908, 4, false},       // ARGB8Unorm (A8B8G8R8's header read rotated, its alpha byte first; rendered
    {85, 0, 0x2b1a4908, 4, true},        // through its stand-in) and ARGB8Unorm_sRGB
    {93, 0, 0x78d7ffa0, 4, false},       // RGB9E5Float
    {94, 0xdf, 0x54e24909, 4, false},    // BGR10A2Unorm
    {103, 0xcd, 0x60d49204, 8, false},   // RG32Uint
    {104, 0xcc, 0x60d36d84, 8, false},   // RG32Sint
    {105, 0xcb, 0x70d7ff84, 8, false},   // RG32Float
    {110, 0xc6, 0x58d24903, 8, false},   // RGBA16Unorm
    {112, 0xc7, 0x58d12483, 8, false},   // RGBA16Snorm: R16G16B16A16_SNORM
    {113, 0xc9, 0x58d49203, 8, false},   // RGBA16Uint
    {114, 0xc8, 0x58d36d83, 8, false},   // RGBA16Sint
    {115, 0xca, 0x58d7ff83, 8, false},   // RGBA16Float (rendered: a 4:2:0 texture's converted image)
    {123, 0xc2, 0x58d49201, 16, false},  // RGBA32Uint
    {124, 0xc1, 0x58d36d81, 16, false},  // RGBA32Sint
    {125, 0xc0, 0x58d7ff81, 16, false},  // RGBA32Float (rendered: an sRGB 4:2:0 texture's image)
    {263, 0, 0x6014920f, 4, false},      // Unspecialized (4-byte texels of no format of their own, held as R32Uint's)
    // RGBX: NIL's X rows, the fourth channel read as one and rendered to targets that hold none
    {300, 0xf9, 0x78d24908, 4, false},   // RGBX8Unorm: R8G8B8X8_UNORM
    {301, 0xfa, 0x78d24908, 4, true},    // RGBX8Unorm_sRGB
    {302, 0xd7, 0x78d12488, 4, false},   // RGBX8Snorm
    {303, 0xd9, 0x68d49208, 4, false},   // RGBX8Uint
    {304, 0xd8, 0x68d36d88, 4, false},   // RGBX8Sint
    {313, 0xc9, 0x68d49203, 8, false},   // RGBX16Uint: R16G16B16X16_UINT
    {314, 0xc8, 0x68d36d83, 8, false},   // RGBX16Sint
    {315, 0xce, 0x78d7ff83, 8, false},   // RGBX16Float
    {323, 0xc5, 0x68d49201, 16, false},  // RGBX32Uint: R32G32B32X32_UINT
    {324, 0xc4, 0x68d36d81, 16, false},  // RGBX32Sint
    {325, 0xc3, 0x78d7ff81, 16, false},  // RGBX32Float
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
    // 4:2:2: two texels a 4-byte element sharing their chroma, sampled as NIL's B8G8R8G8 and G8B8G8R8 rows decode them
    {240, 0, 0x74e24923, 4, false, 2, 1},  // GBGR422: G8B8_G8R8_UNORM
    {241, 0, 0x74e24922, 4, false, 2, 1},  // BGRG422: B8G8_R8G8_UNORM
    // the extended-range formats' codes (xr.h), as m2v's NVK renders and fetches them: RG32_UINT, RGBA16_UINT, R32_UINT
    {550, 0xcd, 0x60d49204, 8, false},  // RGB10A8_2P_XR10
    {551, 0, 0x60d49204, 8, false},     // RGB10A8_2P_XR10_sRGB (its codes; a pass renders it through its stand-in)
    {552, 0xc9, 0x58d49203, 8, false},  // BGRA10_XR
    {553, 0, 0x58d49203, 8, false},     // BGRA10_XR_sRGB (its codes; a pass renders it through its stand-in)
    {554, 0xe4, 0x6014920f, 4, false},  // BGR10_XR
    {555, 0, 0x6014920f, 4, false},     // BGR10_XR_sRGB (its codes; a pass renders it through its stand-in)
};

const PackProgram kNoPixelProgram = {};
const uint8_t kNoPixelHeader[0x50] = {};

struct ScopedUnlock {
    gm20b::Mutex &m;

    explicit ScopedUnlock(gm20b::Mutex &mutex) __attribute__((no_thread_safety_analysis)) : m(mutex)
    {
        m.unlock();
    }

    ~ScopedUnlock() __attribute__((no_thread_safety_analysis))
    {
        m.lock();
    }

    ScopedUnlock(const ScopedUnlock &) = delete;
    ScopedUnlock &operator=(const ScopedUnlock &) = delete;
};

template <typename Pred>
void waitUnderLock(gm20b::Mutex &m, gm20b::CondVar &cv, uint32_t &waiters, Pred &&pred)
    __attribute__((no_thread_safety_analysis))
{
    if (pred())
        return;
    ++waiters;
    std::unique_lock<gm20b::Mutex> adopted(m, std::adopt_lock);
    cv.wait(adopted, std::forward<Pred>(pred));
    adopted.release();
    --waiters;
}
}  // namespace

PlacedProgram noPixelProgram()
{
    PlacedProgram p;
    p.record = &kNoPixelProgram;
    p.header = kNoPixelHeader;
    return p;
}

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
                SWITCH_METAL_DIAGNOSTIC(stats.ringWaits++);
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
    SWITCH_METAL_DIAGNOSTIC(stats.ringsOutgrown++);
    r.outgrown.push_back(r.bo);
    r.bo = larger;
    r.head = r.since = r.clear = 0;
    r.inflight.clear();
    at = 0;
    return true;
}

uint64_t Device::pushVa(const uint32_t *word) const
{
    const auto *at = reinterpret_cast<const uint8_t *>(word);
    for (const Bo &b : push_.outgrown)
        if (at >= b.cpu && at < b.cpu + b.bytes)
            return b.va + uint64_t(at - b.cpu);
    return push_.bo.va + uint64_t(at - push_.bo.cpu);
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
    {
        std::unique_lock<gm20b::Mutex> l(lock_);
        stopping_ = true;
        closing_ = true;
        packWindowCv_.wait(l, [&] {
            return packInitDone_.load(std::memory_order_acquire);
        });
    }
    transfers_.close(k_, lock_);
    // the render server keeps its device until it exits; a host test's device: the kernel frees what the connection held
    grow_.notify_all();
    grown_.notify_all();
    if (grower_.joinable())
        grower_.join();
    compStop();
    releaseParkedChunksOnClose();
    {
        DeferredFrees deferred(*this);
        std::unique_lock<gm20b::Mutex> l(lock_);
        arenaReady_.wait(l, [&] {
            return std::none_of(arenas_.begin(), arenas_.end(), [](const Arena &a) {
                return a.busy;
            });
        });
        for (Arena &a : arenas_)
            if (a.used && a.kernelBo && a.purged && a.bo.handle) {
                if (a.boParked) {
                    SWITCH_METAL_DIAGNOSTIC(stats.parkedHeaps--);
                    SWITCH_METAL_DIAGNOSTIC(stats.parkedHeapBytes -= a.bytes);
                }
                deferred.add({a.bo});
                a.bo = {};
                a.boParked = false;
            }
        dropKeptImports(deferred);
        std::vector<Bo> nativeBos;
        reapNativeSurfaces(nativeBos, true);
        for (const Bo &bo : nativeBos)
            deferred.add({bo});
    }
    if (const uint32_t ch = channel_.load(std::memory_order_relaxed))
        k_.closeChannel(ch);
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
        if (im[i].subc == 0 && im[i].mthd == NVB197_SET_ZCULL_STATS)
            continue;
        const uint32_t d = relocated(im[i], here);
        p.m(im[i].mthd, d, im[i].subc);
    }
    const uint32_t *base = pack_.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack_.bytes(PackBaseline) / 8; i++)
        if (!ownedMethod(base[2 * i]) && base[2 * i] != NVB197_SET_ZCULL_STATS)
            p.m(base[2 * i], base[2 * i + 1]);
    stageSwitchesOn(p);
    p.m(NVB197_SET_ZCULL_STATS, NVB197_SET_ZCULL_STATS_ENABLE_FALSE);
    mme::install(p, root_.va, zero_.va);
    tiledCacheInit(p, k_.info());
    tiledCacheSize_ = kTiledCacheDefaultSize;
    // the depth/stencil registers (the runtime's, as NVK leaves them for Metal's default state in a pass without the attachment)
    {
        DepthStencilRegs unknown;
        for (uint32_t &v : unknown.v)
            v = ~0u;
        emitDepthStencil(p, depthStencilRegs(DepthStencilDesc(), 0, 0, false, false), unknown);
        ds_ = unknown;
    }
    heldSegment_.lose();
    clampRangeHeld_ = false;
    // root table selected for LOAD_CONSTANT_BUFFER from here on (draws reselect it after binding set 0 as a cbuf); NVK's init
    // above zeroed it, so one sample's layout goes in now, as every pass but a multisampled one leaves it
    sampleWords(p, 1, root_.va);
}

bool Device::open(const char *packPath, uint64_t stack, std::string &err, Role role, bool asyncPackWindow)
{
    role_ = role;
    sizes_ = kSizes[role == Role::RenderServer ? 0 : 1];
    k_.observeFrees(set_);
    openFailure_ = OpenFailure::Structural;
    SWITCH_METAL_CREATION_STAMP(loadBegan);
    if (!pack_.load(packPath, stack, err))
        return false;
    SWITCH_METAL_PROFD(gPackLoadTicks = mach_absolute_time() - loadBegan);
    openFailure_ = OpenFailure::Transient;
    if (!k_.open(err))
        return false;
    if (k_.info().cls_eng3d != 0xb197 || k_.info().cls_copy != 0xb0b5) {
        openFailure_ = OpenFailure::Structural;
        err = "not a MAXWELL_B with MAXWELL_DMA_COPY_A";
        return false;
    }
    ZcullRegion probe;
    zcullOn_ = k_.zcullInfo(zcullInfo_) == KERN_SUCCESS && zcullRegion(zcullInfo_, 0, 0, 1, 1, probe);
    return init(err, asyncPackWindow);
}

// The interlock's ticket words (nak_nir_lower_fs_interlock.c interlock_ticket_word): word (SR_AFFINITY & 0xff) * 256 +
// (ticket & 0xff), as NVIDIA's own programs index them. Nothing bounds the affinity byte below 256, and a word past the
// buffer would spin on and add into the control memory mapped after it without a fault
constexpr uint32_t kTicketBytes = 256 * 256 * 4;

void Device::waitPackWindow() const
{
    std::unique_lock<gm20b::Mutex> l(lock_);
    packWindowCv_.wait(l, [&] {
        return packWindowReady_.load(std::memory_order_acquire);
    });
}

void Device::runAsyncPackWindow()
{
    std::string packErr;
    const bool ok = finishPackWindow(packErr);
    if (!ok)
        os_log_error(gm20b::log(), "SwitchMetal: pack window init failed: %{public}s", packErr.c_str());
    bool warmServer = false;
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        packWindowOk_ = ok;
        packWindowReady_.store(true, std::memory_order_release);
        warmServer = ok && role_ == Role::RenderServer && !closing_;
    }
    packWindowCv_.notify_all();
    if (warmServer)
        warmRenderServer();
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        packInitDone_.store(true, std::memory_order_release);
    }
    packWindowCv_.notify_all();
}

bool Device::finishPackWindow(std::string &err)
{
    if (!initPackWindow(err))
        return false;
    const PackHeader &h = *pack_.header();
    const uint64_t page = k_.info().page_size;
    uint64_t mapped = 0;
    for (const PackSection &section : h.section)
        mapped = std::max(mapped, (section.offset + section.bytes + page - 1) / page * page);
    const uint64_t heapStart = h.section[PackHeap].offset + packWindowBytes_;
    packPages_.reset(heapStart, mapped, mapped, packWindowBytes_);
    const uint64_t packEnd = std::max(packWindowBytes_, packPages_.windowEnd());
    heapCopiesAt_ = heapPlaced_ = (packEnd + kHeapStep - 1) & ~(kHeapStep - 1);
    heapBound_.store(heapCopiesAt_, std::memory_order_relaxed);
    heapStoresAt_ = heapCopiesAt_ + kHeapCopiesWindow;
    tier0Pipelines_.reset(uint32_t(pack_.tier0Entries(PackPipelines, sizeof(PackPipeline))));
    if (packWindowBytes_ && h.tier0SlmPerWarp) {
        std::lock_guard<gm20b::Mutex> l(lock_);
        (void)slmNeed(h.tier0SlmPerWarp);
    }
    return true;
}

void Device::warmRenderServer()
{
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        if (closing_)
            return;
    }
    if (!ensureChannel())
        return;
    {
        DeferredFrees deferred(*this);
        std::unique_lock<gm20b::Mutex> l(lock_);
        if (closing_)
            return;
        if (!packWindowBytes_)
            heapGrow(heapCopiesAt_ + std::min<uint64_t>(pack_.bytes(PackHeap) + kHeapSlack, kHeapAtOpen));
        Span first;
        if (blAlloc(l, 1, 0, first))
            blFree(first, deferred);
        uint32_t chunk;
        newChunk(sizes_.chunk, false, l, chunk);
    }
    warm();
#if SWITCH_METAL_DIAGNOSTICS
    os_log(gm20b::log(), "SwitchMetal: shader heap %llu KB, each program placed at its state's first use",
           (unsigned long long)((heapBound_.load(std::memory_order_relaxed) - heapCopiesAt_) >> 10));
#endif
}

// Every process's: the header pools, the root table, the zero page, the interlock tickets and the shader heap's reservation,
// which textures, samplers and states need before any submission. The render server's channel, heap and first chunks too.
bool Device::init(std::string &err, bool asyncPackWindow)
{
    if (!k_.allocControl(kTicCount * 32, tic_, ControlUse::TextureHeaders) ||
        !k_.allocControl(kTscCount * 32, tsc_, ControlUse::SamplerHeaders) ||
        !k_.allocControl(0x1000, root_, ControlUse::RootTable) ||
        !k_.allocControl(0x10000, zero_, ControlUse::ZeroPage) ||
        !k_.allocControl(kTicketBytes, tickets_, ControlUse::InterlockTickets) ||
        !k_.allocControl(kColorTableBytes, colorTable_, ControlUse::ColorTable) ||
        !k_.allocControl(cbufBindBytes(kSrgbEncodeEntries * 2), srgbEncode_, ControlUse::ColorTable) ||
        !k_.allocControl(kStampBytes, stamps_, ControlUse::Stamps)) {
        err = k_.error();
        return false;
    }
    // a use of every job: any of them may read it through a buffer slot bound to nothing, which no draw names
    set_.add(k_, zero_, TegraGM20BSurfaceRead, true);
    set_.add(k_, {{&tic_, TegraGM20BSurfaceRead},
                  {&tsc_, TegraGM20BSurfaceRead},
                  {&root_, TegraGM20BSurfaceWrite},
                  {&tickets_, TegraGM20BSurfaceWrite},
                  {&colorTable_, TegraGM20BSurfaceRead},
                  {&srgbEncode_, TegraGM20BSurfaceRead},
                  {&stamps_, TegraGM20BSurfaceWrite}});
#if SWITCH_METAL_DIAGNOSTICS
    static std::once_flag ticketsLogged;
    std::call_once(ticketsLogged, [this] {
        os_log(gm20b::log(),
               "SwitchMetal: interlock tickets mapped %{public}s at 0x%llx",
               tickets_.gpuVolatile ? "volatile (GPU L2 bypassed)" : "cached (GPU L2)",
               (unsigned long long)tickets_.va);
    });
#endif
    memset(tic_.cpu, 0, tic_.bytes);
    memset(tsc_.cpu, 0, tsc_.bytes);
    memset(root_.cpu, 0, root_.bytes);
    memset(zero_.cpu, 0, zero_.bytes);
    memset(tickets_.cpu, 0, tickets_.bytes);
    colorTableWords(reinterpret_cast<uint32_t *>(colorTable_.cpu));
    static constexpr uint16_t kSrgbEncode[kSrgbEncodeEntries] = {
#include "pack/srgb_encode.h"
    };
    memset(srgbEncode_.cpu, 0, srgbEncode_.bytes);
    memcpy(srgbEncode_.cpu, kSrgbEncode, sizeof kSrgbEncode);
    memset(stamps_.cpu, 0, stamps_.bytes);
#if SWITCH_METAL_PROF
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
        openFailure_ = OpenFailure::Structural;
        err = "the pack's constexpr samplers do not fit its entries' sampler byte";
        return false;
    }
    for (uint32_t i = 0; i < pack_.header()->samplers; i++) {
        memcpy(tsc_.cpu + 32 * nextTsc_, cs + 8 * i, 32);
        constexprWord0_[constexprCount_] = cs[8 * i];
        constexprTsc_[constexprCount_] = nextTsc_++;
        if (!placeConstexprMaps(constexprCount_++, cs + 8 * i)) {
            err = "no sampler header left for the pack's constexpr samplers under planar address maps";
            return false;
        }
    }
    constexprMappedCount_ = constexprCount_;
    const uint32_t initCount = uint32_t(pack_.bytes(PackInit) / sizeof(PackInitMethod));
    const PackInitMethod *im = pack_.section<PackInitMethod>(PackInit);
    for (uint32_t i = 0; i < initCount; i++) {
        if (im[i].subc == 0 && im[i].mthd >= 0x1e04 && im[i].mthd <= 0x1e18) {
            const uint32_t idx = (im[i].mthd - 0x1e04) / 4;
            blendPerTarget_[idx] = im[i].data;
            blendPerTargetSeen_ |= uint8_t(1u << idx);
        }
    }
    // SET_CT_WRITE(i), as the baseline leaves it where a state's registers do not set it
    const uint32_t *base = pack_.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack_.bytes(PackBaseline) / 8; i++) {
        if (base[2 * i] >= CT_WRITE && base[2 * i] < CT_WRITE + 32)
            ctWrite_[(base[2 * i] - CT_WRITE) / 4] = base[2 * i + 1];
        else if (base[2 * i] >= 0x1e04 && base[2 * i] <= 0x1e18) {
            const uint32_t idx = (base[2 * i] - 0x1e04) / 4;
            blendPerTarget_[idx] = base[2 * i + 1];
            blendPerTargetSeen_ |= uint8_t(1u << idx);
        }
    }
    segmentHeap_.setGrow([this](uint64_t bytes, SegmentChunk &chunk) {
        if (closing_)
            return false;
        ++offLockChunkOps_;
        Bo bo;
        bool ok = false;
        {
            ScopedUnlock ul(lock_);
            ok = k_.allocControl(bytes, bo, ControlUse::StateSegments);
            if (ok)
                set_.add(k_, bo, TegraGM20BSurfaceRead);
        }
        if (ok && closing_) {
            ScopedUnlock ul(lock_);
            k_.free(bo);
            ok = false;
        }
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        if (!ok)
            return false;
        chunk = {bo.cpu, bo.va, bo.bytes};
        return true;
    });
    if (asyncPackWindow) {
        packWindowReady_.store(false, std::memory_order_relaxed);
        packInitDone_.store(false, std::memory_order_relaxed);
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), this, [](void *ctx) {
            static_cast<Device *>(ctx)->runAsyncPackWindow();
        });
        return true;
    }
    if (!finishPackWindow(err))
        return false;
    if (role_ == Role::Client)
        return true;
    if (!ensureChannel()) {
        err = channelError_;
        return false;
    }
    // at the render server's start, not under its first frames: the heap's first part, the mip chains' first chunk and the
    // first pool chunk
    {
        DeferredFrees deferred(*this);
        std::unique_lock<gm20b::Mutex> l(lock_);
        if (!packWindowBytes_)
            heapGrow(heapCopiesAt_ + std::min<uint64_t>(pack_.bytes(PackHeap) + kHeapSlack, kHeapAtOpen));
        Span first;
        if (blAlloc(l, 1, 0, first))
            blFree(first, deferred);
        uint32_t chunk;
        newChunk(sizes_.chunk, false, l, chunk);
    }
    return true;
}

bool Device::ensureChannel()
{
    if (channelReady_.load(std::memory_order_acquire))
        return true;
    std::lock_guard<gm20b::UnfairMutex> l(channelLock_);
    if (channelReady_.load(std::memory_order_relaxed))
        return true;
    if (!channelRefusal_.mayAsk(k_))
        return false;
    bool gaveBack = false;
    if (!openChannel(channelError_, gaveBack)) {
        channelRefusal_.refused(gaveBack);
        return false;
    }
    channelRefusal_.made();
    generations_.opened();
    channelReady_.store(true, std::memory_order_release);
    return true;
}

// channelLock_ held: the rings, the channel and its context with the 2D engine's state (fx). False: what it made is given
// back, so a later call starts over; `gaveBack`: a channel among it.
bool Device::openChannel(std::string &err, bool &gaveBack)
{
    ensurePackWindow();
    if (!packWindowOk_) {
        err = "no GPU VA for the shader heap's program window";
        gaveBack = false;
        return false;
    }
    uint32_t ch = 0;
    const volatile uint32_t *fence = &kNoFence;
    auto undo = [&](bool created) {
        err = k_.error();
        gaveBack = created;
        if (created)
            k_.closeChannel(ch);
        progressVa_ = 0;
        progressSeq_ = 0;
        k_.free(push_.bo);
        k_.free(upload_.bo);
        push_ = Ring();
        upload_ = Ring();
        return false;
    };
    if (!k_.allocControl(sizes_.push, push_.bo, ControlUse::Ring) ||
        !k_.allocControl(sizes_.upload, upload_.bo, ControlUse::Ring) || !k_.channel(kEngines, ch, fence))
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
    if (!job(view, ch, &seg, 1, resources, JobVisibility::External, seq))
        return undo(true);
    channel_.store(ch, std::memory_order_release);
    fence_.store(fence, std::memory_order_release);
    progressVa_ = 0;
    progressSeq_ = 0;
    k_.channelProgress(ch, progressVa_);
    submittedThrough(seq);
    retire(push_, seq);
    return true;
}

bool Device::initPackWindow(std::string &err)
{
    heap_.bytes = kProgramWindow;
    const PackHeader &h = *pack_.header();
    if (k_.supportsPackImage() && packWindowEligible(h, kProgramWindow)) {
        TegraGM20BAbiPackCpuRange cpuRanges[kPackWindowMaxCpuRanges] = {};
        uint32_t cpuRangeCount = 0;
        if (role_ == Role::RenderServer) {
            const auto *base = reinterpret_cast<const uint8_t *>(pack_.header());
            cpuRangeCount = packWindowCollectCpuRanges(base, h, uint64_t(vm_page_size), cpuRanges);
            for (uint32_t i = 0; i < cpuRangeCount; ++i)
                madvise(reinterpret_cast<void *>(uintptr_t(cpuRanges[i].address)), size_t(cpuRanges[i].bytes),
                        MADV_SEQUENTIAL);
        }
#if SWITCH_METAL_DIAGNOSTICS
        const uint64_t mapBegan = mach_absolute_time();
#endif
        TegraGM20BAbiPackImageMapReply reply = {};
        const int kr = k_.packImageMap(cpuRanges, cpuRangeCount, reply);
        for (uint32_t i = 0; i < cpuRangeCount; ++i)
            madvise(reinterpret_cast<void *>(uintptr_t(cpuRanges[i].address)), size_t(cpuRanges[i].bytes), MADV_NORMAL);
        const os_log_t log = gm20b::log();
        const char *proc = getprogname();
        const char *who = proc ? proc : "unknown";
        if (kr != KERN_SUCCESS) {
            os_log_error(log, "SwitchMetal: %{public}s: pack image map failed (0x%x): using per-piece placement", who,
                         unsigned(kr));
        } else if (reply.status == TegraGM20BPackStatusRefused) {
            os_log_error(log, "SwitchMetal: %{public}s: pack image refused (status %u): using per-piece placement", who,
                         reply.status);
        } else if (!packWindowMatchesReply(h, reply, TGM_PACK_WINDOW_VA, kProgramWindow)) {
            os_log_error(log,
                         "SwitchMetal: %{public}s: pack image mismatch (status %u, va 0x%llx, tier0 %llu/%llu, heap "
                         "%llu/%llu): using per-piece placement",
                         who, reply.status, (unsigned long long)reply.gpu_va,
                         (unsigned long long)reply.tier0_heap_bytes, (unsigned long long)h.tier0[PackHeap],
                         (unsigned long long)reply.heap_bytes, (unsigned long long)h.section[PackHeap].bytes);
        } else {
            heap_.va = heap_.vaBase = reply.gpu_va;
            packWindowVa_ = reply.gpu_va;
            packWindowBytes_ = reply.tier0_heap_bytes;
            packWindowCpu_ = reinterpret_cast<const uint8_t *>(uintptr_t(reply.cpu_address));
#if SWITCH_METAL_DIAGNOSTICS
            static mach_timebase_info_data_t tb = [] {
                mach_timebase_info_data_t t = {};
                mach_timebase_info(&t);
                return t;
            }();
            const uint64_t mapNs = tb.denom ? (mach_absolute_time() - mapBegan) * tb.numer / tb.denom : 0;
            if (reply.waited_ns >= 1000000ull)
                os_log(log, "SwitchMetal: %{public}s: waited %llu ms at device open for the pack image", who,
                       (unsigned long long)(reply.waited_ns / 1000000ull));
            os_log(log, "SwitchMetal: %{public}s: pack window mapped in %llu us (%llu MiB tier 0)", who,
                   (unsigned long long)(mapNs / 1000ull), (unsigned long long)(reply.tier0_heap_bytes >> 20));
            if (reply.cpu_wired_bytes)
                os_log(log, "SwitchMetal: pack window: %llu MiB of CPU ranges wired in %llu ms",
                       (unsigned long long)(reply.cpu_wired_bytes >> 20), (unsigned long long)(mapNs / 1000000ull));
#endif
            return true;
        }
    }
    heap_.va = heap_.vaBase = k_.reserveVa(kProgramWindow);
    if (!heap_.va) {
        err = "no GPU VA for the shader heap's program window";
        return false;
    }
    return true;
}

bool Device::packWindowProgram(uint32_t prog, uint32_t &program, uint64_t &data) const
{
    if (!packWindowBytes_ || !pack_.header() || prog >= pack_.header()->programs)
        return false;
    const PackProgram &p = pack_.section<PackProgram>(PackPrograms)[prog];
    if (!packWindowContainsProgram(p, packWindowBytes_))
        return false;
    program = p.program;
    data = p.dataBytes ? packWindowVa_ + p.heapOffset + p.dataOffset : 0;
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
    if (packWindowProgram(prog, program, data))
        return true;
    const PackProgram &p = pack_.section<PackProgram>(PackPrograms)[prog];
    auto it = placedCold_.find(prog);
    if (it == placedCold_.end()) {
        uint64_t at = 0;
        const uint8_t *file = reinterpret_cast<const uint8_t *>(pack_.header());
        const uint64_t fileOffset = pack_.header()->section[PackHeap].offset + p.heapOffset;
        const bool inPlace = packWindowBytes_ != 0 || p.bytes >= k_.info().page_size ||
                             packPages_.bound(fileOffset, p.bytes + kHeapSlack);
        if (!(inPlace && placeInPlace(packPages_, file, fileOffset, p.bytes, false, at)) &&
            !heapPlace(pack_.section<uint8_t>(PackHeap) + p.heapOffset, p.bytes, at))
            return false;
        const auto [insertedIt, inserted] = placedCold_.emplace(prog, uint32_t(at));
        it = insertedIt;
        if (inserted)
            SWITCH_METAL_DIAGNOSTIC(stats.programsPlaced++);
    }
    program = it->second + (p.program - p.heapOffset);
    data = p.dataBytes ? heap_.va + it->second + p.dataOffset : 0;
    return true;
}

// lock_ held (released across the kernel allocation and resource-set update): an area whose warps get `perWarp` bytes of
// local memory or more; the next submission gives it to the channel (CommandBuffer::commit). False: no memory for it.
bool Device::slmNeed(uint64_t perWarp)
{
    if (perWarp <= slmPerWarp_.load(std::memory_order_acquire))
        return true;
    waitUnderLock(lock_, heapReady_, heapWaiters_, [&] {
        return !slmGrowing_;
    });
    if (perWarp <= slmPerWarp_.load(std::memory_order_acquire))
        return true;
    if (closing_)
        return false;
    slmGrowing_ = true;
    ++offLockChunkOps_;
    const SlmArea a = slmArea(perWarp, k_.info().tpc_count);
    Bo area;
    bool ok = false;
    {
        ScopedUnlock ul(lock_);
        ok = k_.allocGpu(a.bytes, area);
        if (ok)
            set_.add(k_, area, TegraGM20BSurfaceWrite, true);
    }
    if (ok && closing_) {
        ScopedUnlock ul(lock_);
        k_.free(area);
        ok = false;
    }
    if (ok) {
        if (slm_.bytes)
            slmOutgrown_.push_back(slm_);
        slm_ = area;
        slmVa_.store(area.va, std::memory_order_relaxed);
        slmPerWarp_.store(perWarp, std::memory_order_release);
    }
    slmGrowing_ = false;
    if (heapWaiters_)
        heapReady_.notify_all();
    if (--offLockChunkOps_ == 0)
        chunkReady_.notify_all();
    return ok;
}

// lock_ held (released across host page allocation, import, bind and resource-set update): the heap bound from its start
// through `end`, the range past what is bound as one more part of host memory imported and bound at the window's VAs.
// False: past the window, or no memory or the kernel refused the part.
bool Device::heapGrow(uint64_t end)
{
    if (end <= heapBound_.load(std::memory_order_relaxed))
        return true;
    if (end > heapStoresAt_)
        return false;
    while (heapBound_.load(std::memory_order_relaxed) < end) {
        waitUnderLock(lock_, heapReady_, heapWaiters_, [&] {
            return !heapGrowing_;
        });
        const uint64_t curBound = heapBound_.load(std::memory_order_relaxed);
        if (end <= curBound)
            return true;
        if (end > heapStoresAt_ || closing_)
            return false;
        // a client's parts double from a quarter step, as its chunks do
        const uint64_t step = role_ == Role::Client
                                  ? std::clamp(curBound - heapCopiesAt_, kHeapStep / kClientHeapFirstShare, kHeapStep)
                                  : kHeapStep;
        HeapPart part;
        part.offset = curBound;
        part.bytes = std::min(heapStoresAt_ - curBound, (end - curBound + step - 1) & ~(step - 1));
        const uint64_t bindVa = heap_.va + part.offset;
        heapGrowing_ = true;
        ++offLockChunkOps_;
        vm_address_t cpu = 0;
        bool ok = false;
        {
            ScopedUnlock ul(lock_);
            if (vm_allocate(mach_task_self(), &cpu, part.bytes, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
                part.cpu = reinterpret_cast<uint8_t *>(cpu);
                if (k_.importPages(part.cpu, part.bytes, part.bo, 0, true) &&
                    k_.bindImportAt(part.cpu, part.bo, bindVa, kPteKindPitch)) {
                    set_.add(k_, part.bo, TegraGM20BSurfaceRead, true);
                    ok = true;
                } else {
                    vm_deallocate(mach_task_self(), cpu, part.bytes);
                }
            }
        }
        if (ok && closing_) {
            ScopedUnlock ul(lock_);
            k_.free(part.bo);
            vm_deallocate(mach_task_self(), cpu, part.bytes);
            ok = false;
        }
        if (ok) {
            heapParts_.push_back(part);
            heapBound_.store(curBound + part.bytes, std::memory_order_release);
        }
        heapGrowing_ = false;
        if (heapWaiters_)
            heapReady_.notify_all();
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        if (!ok)
            return false;
    }
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
    for (;;) {
        uint64_t at = (heapPlaced_ + 0xFF) & ~uint64_t(0xFF);
        for (const HeapPart *p = heapPartAt(at); p && at + size > p->offset + p->bytes; p = heapPartAt(at))
            at = p->offset + p->bytes;
        if (at + size + kHeapSlack > heapBound_.load(std::memory_order_relaxed)) {
            if (!heapGrow(at + size + kHeapSlack))
                return false;
            continue;
        }
        const HeapPart *p = heapPartAt(at);
        uint8_t *cpu = p->cpu + (at - p->offset);
        memcpy(cpu, bytes, size);
        cleanToMemory(cpu, size);
        SWITCH_METAL_CREATION_COUNT(copies, 1);
        SWITCH_METAL_CREATION_COUNT(copiedBytes, size);
        offset = at;
        heapPlaced_ = at + size;
        heapEpoch_.fetch_add(1, std::memory_order_release);
        return true;
    }
}

// lock_ held (released across per-grain BoImport and VmBind): a slot of `size` bytes at `fileOffset` of the read-only
// mapping `file` run in place: the grains it and the instruction prefetch past its end lie in imported and bound at their
// window offsets where they are not yet, its window offset in `offset`. The kernel wires the pages read-only (BoImport's
// read-only retry; the GPU maps them read-only), cleans them at the import (an import of no surface is never `covered`),
// and the bind's TLB and L2 invalidate makes the GPU's first walk read them. The pack's pages need nothing more: no process
// can write the file, and this device's import keeps them wired, so no line of them is dirty after that clean. `written`: a
// slot lookup's mapping (runSlotsFrom), which may be the shader compiler's store: the compiler writes it while it is mapped,
// maybe after the import of a grain the slot lies in, so the slot is cleaned here.
// The imports are held as long as the device and are never a job's use or a set member: no job writes them, the CPU never
// changes a published slot, the kernel refuses a read-only import as a member, and a connection's close kills its channels
// before it unbinds and unwires anything (drainResources). False: not inside the file's region, or the kernel refused an
// import (the slot is then copied).
bool Device::placeInPlace(ProgramPages &pages, const uint8_t *file, uint64_t fileOffset, uint32_t size, bool written,
                          uint64_t &offset)
{
    std::vector<ProgramPages::Grain> grains;
    if (!pages.missing(fileOffset, uint64_t(size) + kHeapSlack, grains))
        return false;
#if SWITCH_METAL_DIAGNOSTICS
    const bool initialEmpty = grains.empty();
#endif
    while (!grains.empty()) {
        const ProgramPages::Grain r = grains.front();
        const std::pair<const uint8_t *, uint64_t> key{file, r.offset};
        if (importingGrains_.count(key)) {
            waitUnderLock(lock_, heapReady_, heapWaiters_, [&] {
                return !importingGrains_.count(key);
            });
            if (!pages.missing(fileOffset, uint64_t(size) + kHeapSlack, grains))
                return false;
            continue;
        }
        if (closing_)
            return false;
        importingGrains_.insert(key);
        ++offLockChunkOps_;
        const uint64_t windowVa = heap_.va + pages.windowOffset(r.offset);
        void *cpu = const_cast<uint8_t *>(file + r.offset);
        Bo bo;
        bool imported = false;
        bool bound = false;
        {
            ScopedUnlock ul(lock_);
            SWITCH_METAL_CREATION_SPAN(pieceTicks);
            SWITCH_METAL_CREATION_COUNT(pieces, 1);
            imported = k_.importPages(cpu, r.bytes, bo, 0, true);
            bound = imported && k_.bindImportAt(cpu, bo, windowVa, kPteKindPitch);
        }
        const bool closing = closing_;
        if (bound && closing) {
            ScopedUnlock ul(lock_);
            k_.free(bo);
            bound = false;
        }
        importingGrains_.erase(key);
        if (bound) {
            inPlaceImports_.push_back(bo);
            pages.bind(r);
        }
        if (heapWaiters_)
            heapReady_.notify_all();
        if (--offLockChunkOps_ == 0)
            chunkReady_.notify_all();
        if (!imported) {
            os_log_error(
                gm20b::log(),
                "SwitchMetal: the kernel refused %llu KiB of program pages at file offset 0x%llx: copied instead",
                (unsigned long long)(r.bytes >> 10), (unsigned long long)r.offset);
            return false;
        }
        if (!bound) {
            if (!closing)
                os_log_error(
                    gm20b::log(),
                    "SwitchMetal: the program pages at file offset 0x%llx would not bind in the window: copied instead",
                    (unsigned long long)r.offset);
            return false;
        }
        if (!pages.missing(fileOffset, uint64_t(size) + kHeapSlack, grains))
            return false;
    }
#if SWITCH_METAL_DIAGNOSTICS
    std::atomic<uint64_t> &placed = written ? stats.storeSlotsPlaced : stats.packSlotsInBoundGrains;
    if (written || initialEmpty)
        placed.store(placed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
#endif
    if (written) {
        cleanToMemory(file + fileOffset, size);
        SWITCH_METAL_DIAGNOSTIC(
            stats.slotCleans.store(stats.slotCleans.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed));
    }
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
    return placeInPlace(region->pages, mapping, uint64_t(at - mapping), size, true, offset);
}

void Device::runSlotsFrom(SlotLookup lookup)
{
    std::lock_guard<gm20b::Mutex> l(lock_);
    slotLookup_ = std::move(lookup);
}

// lock_ held: the CPU's view of window offset `at`: the pack's or a store's mapping, or the heap part holding it (null:
// none)
const uint8_t *Device::heapCpu(uint64_t at) const
{
    if (at < packWindowBytes_)
        return packWindowCpu_ ? packWindowCpu_ + at : pack_.section<uint8_t>(PackHeap) + at;
    if (packPages_.holdsWindow(at))
        return reinterpret_cast<const uint8_t *>(pack_.header()) + packPages_.fileOffset(at);
    for (const StoreRegion &r : storeRegions_)
        if (r.pages.holdsWindow(at))
            return r.mapping + r.pages.fileOffset(at);
    const HeapPart *part = heapPartAt(at);
    return part ? part->cpu + (at - part->offset) : nullptr;
}

}  // namespace gm20b
