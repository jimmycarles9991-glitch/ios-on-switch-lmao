// SPDX-License-Identifier: MIT
// See raw_gpu.h. Method numbers: NVIDIA's class headers as Mesa ships them (src/nouveau/headers/nvidia/classes, MIT). The texture
// header fill follows Mesa NIL's nvb097_fill_image_view_desc (pitch), the sampler fill Mesa NVK's nvk_sampler_get_header, the
// copies NVK's nouveau_copy_rect and nvk_cmd_copy_memory_ce (all MIT), restricted to what Core Animation uses. r7-raw wrote the
// first version (render encoder, pack replay) and checked it against NVK's stream; r8-rawca added the lifetimes, the pool, the
// imports, the copies and the abortable command buffer.
#include "raw_follow.h"
#include "raw_gpu.h"
#include "raw_target.h"
#include "TegraGM20BLayout.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <os/log.h>
#include <pthread.h>
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
    OGL_SET_CULL = 0x1918,
    OGL_SET_FRONT_FACE = 0x191c,
    OGL_SET_CULL_FACE = 0x1920,
    PIXEL_SHADER_INTERLOCK_CONTROL = 0x1224,
    DEPTH_TEST = 0x12cc,
    STENCIL_TEST = 0x1380,
    ANTI_ALIAS_ALPHA_CONTROL = 0x153c,
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
constexpr uint64_t kPushRing = 2 << 20, kUploadRing = 4 << 20;
constexpr uint64_t kChunk = 16 << 20, kDedicated = 8 << 20;
constexpr size_t kDedicatedSpares = 2;
// The shader heap: the programs of the states a process uses, placed at first use (Device::place); the pack holds tens of MB
// of programs, of which the render server uses a few hundred
constexpr uint64_t kHeapCap = 8 << 20;
constexpr uint32_t kMaxSegments = TegraGM20BMaxPushes;
// NVK's copies of pitch memory (nouveau_copy_rect: LAUNCH_DMA PIPELINED, FLUSH_ENABLE, SRC/DST_MEMORY_LAYOUT PITCH, MULTI_LINE_ENABLE)
constexpr uint32_t kLaunchPitch = 1u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 9;

uint32_t fbits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// Texture header word 0 per MTLPixelFormat as NVK writes it (pack/mkformats.py over the twin, 11-04), and the render target
// format NVK writes where the pack renders to it (BGRA8 0xcf; R32Float 0xe5, Core Animation's third attachment in two states).
const FormatInfo kFormats[] = {
    {1, 0, 0x2002491d, 1, false},        // A8Unorm
    {10, 0, 0x7012491d, 1, false},       // R8Unorm
    {11, 0, 0x7012491d, 1, true},        // R8Unorm_sRGB
    {20, 0, 0x7012491b, 2, false},       // R16Unorm
    {25, 0, 0x7017ff9b, 2, false},       // R16Float
    {30, 0, 0x70d24918, 2, false},       // RG8Unorm
    {40, 0, 0x74e24915, 2, false},       // B5G6R5Unorm
    {41, 0, 0x272a4913, 2, false},       // A1BGR5Unorm
    {42, 0, 0x272a4912, 2, false},       // ABGR4Unorm
    {43, 0, 0x54e24914, 2, false},       // BGR5A1Unorm
    {55, 0xe5, 0x7017ff8f, 4, false},    // R32Float
    {60, 0, 0x70d2490c, 4, false},       // RG16Unorm
    {65, 0, 0x70d7ff8c, 4, false},       // RG16Float
    {70, 0xd5, 0x58d24908, 4, false},    // RGBA8Unorm (rendered: a 4:2:0 code texture's image)
    {71, 0, 0x58d24908, 4, true},        // RGBA8Unorm_sRGB
    {80, 0xcf, 0x54e24908, 4, false},    // BGRA8Unorm
    {81, 0, 0x54e24908, 4, true},        // BGRA8Unorm_sRGB
    {90, 0xd1, 0x58d24909, 4, false},    // RGB10A2Unorm (rendered: a 10-bit code texture's image)
    {92, 0, 0x78d7ffa1, 4, false},       // RG11B10Float
    {93, 0, 0x78d7ffa0, 4, false},       // RGB9E5Float
    {94, 0, 0x54e24909, 4, false},       // BGR10A2Unorm
    {105, 0, 0x70d7ff84, 8, false},      // RG32Float
    {110, 0, 0x58d24903, 8, false},      // RGBA16Unorm
    {115, 0xca, 0x58d7ff83, 8, false},   // RGBA16Float (rendered: a 4:2:0 texture's converted image)
    {125, 0xc0, 0x58d7ff81, 16, false},  // RGBA32Float (rendered: an sRGB 4:2:0 texture's image)
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
}  // namespace

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
    // [at, at + bytes) may still be read by an earlier submission: retire what the GPU finished (the fence page, no call) and
    // block in the kernel (WaitFence: the fence interrupt) for what overlaps. Sized so a Core Animation frame never waits.
    for (auto it = r.inflight.begin(); it != r.inflight.end();) {
        if (it->seqno && done(it->seqno)) {
            it = r.inflight.erase(it);
            continue;
        }
        if (it->begin < at + bytes && at < it->end && it->seqno) {
            stats.ringWaits++;
            k_.wait(channel_, it->seqno);
            it = r.inflight.erase(it);
            continue;
        }
        ++it;
    }
    if (at < r.head && r.since < r.head) {  // wrapped inside one command buffer: keep the part before the wrap
        r.inflight.push_back({r.since, r.bo.bytes, 0});
        r.since = 0;
    }
    r.head = at + bytes;
    va = r.bo.va + at;
    return r.bo.cpu + at;
}

void Device::retire(Ring &r, uint32_t seqno)
{
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
    for (uint32_t i = 0; i < initCount; i++) {
        uint32_t d = im[i].data;
        switch (im[i].reloc) {
        case RelocHeapHi:
            d = uint32_t(heap_.va >> 32);
            break;
        case RelocHeapLo:
            d = uint32_t(heap_.va);
            break;
        case RelocZeroHi:
            d = uint32_t(zero_.va >> 32);
            break;
        case RelocZeroLo:
            d = uint32_t(zero_.va);
            break;
        case RelocRootHi:
            d = uint32_t(root_.va >> 32);
            break;
        case RelocRootLo:
            d = uint32_t(root_.va);
            break;
        case RelocTicHi:
            d = uint32_t(tic_.va >> 32);
            break;
        case RelocTicLo:
            d = uint32_t(tic_.va);
            break;
        case RelocTicMax:
            d = kTicCount - 1;
            break;
        case RelocTscHi:
            d = uint32_t(tsc_.va >> 32);
            break;
        case RelocTscLo:
            d = uint32_t(tsc_.va);
            break;
        case RelocTscMax:
            d = kTscCount - 1;
            break;
        default:
            break;
        }
        p.m(im[i].mthd, d, im[i].subc);
    }
    const uint32_t *base = pack_.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack_.bytes(PackBaseline) / 8; i++)
        p.m(base[2 * i], base[2 * i + 1]);
    // the depth/stencil registers (the runtime's, as NVK leaves them for Metal's default state in a pass without the attachment)
    {
        DepthStencilRegs unknown;
        for (uint32_t &v : unknown.v)
            v = ~0u;
        emitDepthStencil(p, depthStencilRegs(DepthStencilDesc(), 0, 0, false), unknown);
        ds_ = unknown;
    }
    // root table selected for LOAD_CONSTANT_BUFFER from here on (draws reselect it after binding set 0 as a cbuf); NVK's init
    // above zeroed it, so sample 0's location (the pixel centre, nvk_root_descriptor_table.draw.sample_locations) goes in now
    p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(root_.va >> 32), uint32_t(root_.va)});
    p.m(LOAD_CB_OFFSET, 0x1c);
    p.nonInc(LOAD_CB, (const uint32_t[]){0x88}, 1);
    // the pixel interlock's ticket buffer (nvk_root_descriptor_table.interlock_ticket_buffer; NAK's lowering waits on and
    // releases its words, 256 per GPC): zero, as this channel's ticket dispenser starts. NVK's init above may name the probe's.
    {
        const uint32_t a[2] = {uint32_t(tickets_.va), uint32_t(tickets_.va >> 32)};
        p.m(LOAD_CB_OFFSET, kRootInterlockTickets);
        p.nonInc(LOAD_CB, a, 2);
    }
}

bool Device::open(const char *packPath, uint64_t stack, std::string &err)
{
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

bool Device::init(std::string &err)
{
    const uint64_t heapBytes = std::min<uint64_t>(pack_.bytes(PackHeap), kHeapCap);
    if (!k_.alloc(heapBytes + 0x1000, true, heap_) || !k_.alloc(kTicCount * 32, true, tic_) ||
        !k_.alloc(kTscCount * 32, true, tsc_) || !k_.alloc(0x1000, true, root_) || !k_.alloc(0x10000, true, zero_) ||
        !k_.alloc(0x1000, true, tickets_) || !k_.alloc(kPushRing, true, push_.bo) ||
        !k_.alloc(kUploadRing, true, upload_.bo)) {
        err = k_.error();
        return false;
    }
    memset(tic_.cpu, 0, tic_.bytes);
    memset(tsc_.cpu, 0, tsc_.bytes);
    memset(root_.cpu, 0, root_.bytes);
    memset(zero_.cpu, 0, zero_.bytes);
    memset(tickets_.cpu, 0, tickets_.bytes);
    if (!k_.channel(kEngines, channel_, fence_)) {
        err = k_.error();
        return false;
    }
    // texture header 0: NIL's null descriptor (block linear over the zero page), what NVK keeps at index 0
    uint32_t *t0 = reinterpret_cast<uint32_t *>(tic_.cpu);
    t0[0] = 0x2491d;
    t0[1] = uint32_t(zero_.va);
    t0[2] = 0x600000 | uint32_t(zero_.va >> 32);
    t0[4] = 0xe2840000;
    t0[5] = 0x80000000;
    t0[7] = 0x80000001;
    // m2v's neutral sampler, which it binds for a sampler slot the function reads and the encoder left unbound (Vulkan defaults:
    // nearest, clamp to edge, mip nearest, no LOD clamp), then the constexpr samplers the pack's functions use
    {
        const uint32_t neutral[8] = {0x26092, 0x291, 0xfff000, 0, 0, 0, 0, 0};
        memcpy(tsc_.cpu + 32 * nextTsc_, neutral, 32);
        neutralTsc_ = nextTsc_++;
    }
    const uint32_t *cs = pack_.section<uint32_t>(PackSamplers);
    for (uint32_t i = 0; i < pack_.header()->samplers; i++) {
        memcpy(tsc_.cpu + 32 * nextTsc_, cs + 8 * i, 32);
        constexprTsc_.push_back(nextTsc_++);
    }

    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, contextWords() * 4, 256, va));
    Push p{start, start + contextWords()};
    emitContext(p);
    TegraGM20BAbiPush seg = {va, uint32_t(p.at - start), 0};
    uint32_t seq = 0;
    if (!k_.exec(channel_, &seg, 1, 0, seq)) {
        err = k_.error();
        return false;
    }
    lastSeqno_ = seq;
    retire(push_, seq);
    if (!k_.wait(channel_, seq)) {
        err = k_.error();
        return false;
    }
    // SET_CT_WRITE(i), as the baseline leaves it where a state's registers do not set it
    const uint32_t *base = pack_.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack_.bytes(PackBaseline) / 8; i++)
        if (base[2 * i] >= CT_WRITE && base[2 * i] < CT_WRITE + 32)
            ctWrite_[(base[2 * i] - CT_WRITE) / 4] = base[2 * i + 1];
    pipelines_.resize(pack_.header()->pipelines);
    placed_.assign(pack_.header()->programs, ~0u);
    // the 2D engine's state and its mip-chain self-test (raw/fx); chains on, their first chunk now too (wired here, at the
    // render server's start, not under the first mipmapped texture's frame)
    selfTest();
    if (chainsOn_) {
        std::lock_guard<std::mutex> l(lock_);
        Span first;
        if (blAlloc(1, 0, first))
            blFree(first);
    }
    // the first pool chunk now, at the render server's start, not under its first frame
    uint32_t chunk;
    newChunk(kChunk, false, nullptr, chunk);
    return true;
}

// lock_ held: program `prog`'s slot in the heap, copied out of the pack the first time a state uses it, its
// SET_PIPELINE_PROGRAM value and its shader data's GPU address (0: none). A slot is a few KiB copied once per process from the
// pack's pages, so the CPU copies it (the copy engine would want the pack's pages imported, wired, for the whole process); the
// next submission's prologue invalidates the shader and constant caches before any draw reads it. False: the heap is full (that
// state stays m2v's).
bool Device::place(uint32_t prog, uint32_t &program, uint64_t &data)
{
    const PackProgram &p = pack_.section<PackProgram>(PackPrograms)[prog];
    if (placed_[prog] == ~0u) {
        if (heapPlaced_ + p.bytes > heap_.bytes - 0x1000)
            return false;
        memcpy(heap_.cpu + heapPlaced_, pack_.section<uint8_t>(PackHeap) + p.heapOffset, p.bytes);
        placed_[prog] = uint32_t(heapPlaced_);
        heapPlaced_ += p.bytes;
        stats.programsPlaced++;
    }
    program = placed_[prog] + (p.program - p.heapOffset);
    data = p.dataBytes ? heap_.va + placed_[prog] + p.dataOffset : 0;
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
// as push words with its programs placed in the heap
Pipeline *Device::pipeline(uint64_t keyHash, PackForm form, bool caTransform)
{
    const PackPipeline *rec = pack_.find(keyHash, form);
    if (!rec)
        return nullptr;
    std::lock_guard<std::mutex> l(lock_);
    Pipeline &pl = pipelines_[rec - pack_.section<PackPipeline>(PackPipelines)];
    if (pl.rec)
        return &pl;
    const PackPipeline &pp = *rec;
    const PackProgram *progs = pack_.section<PackProgram>(PackPrograms);
    const uint32_t *blob = pack_.section<uint32_t>(PackBlob);
    uint32_t vs = 0, fs = 0;
    uint64_t vsData = 0, fsData = 0;
    if (!place(pp.vs, vs, vsData) || !place(pp.fs, fs, fsData)) {
        stats.heapFull++;
        return nullptr;
    }
    std::vector<uint32_t> words(64 + 16 + pp.blobDwords * 2);
    Push w{words.data(), words.data() + words.size()};
    uint32_t writes[8];
    memcpy(writes, ctWrite_, sizeof writes);
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[pp.blob + k], v = blob[pp.blob + k + 1];
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
        if (blob[pp.blob + k] == PIXEL_SHADER_INTERLOCK_CONTROL)
            interlocked = blob[pp.blob + k + 1] & 3;
    w.m(0x2044, vs);  // SET_PIPELINE_PROGRAM(VERTEX)
    w.m(0x2144, fs);  // SET_PIPELINE_PROGRAM(PIXEL)
    pl.setSlot[0] = bindProgramCbufs(w, progs[pp.vs], 0, vsData, root_.va);
    pl.setSlot[1] = bindProgramCbufs(w, progs[pp.fs], 4, fsData, root_.va);
    pl.methods.assign(words.data(), w.at);
    memcpy(pl.push, pp.pushWords, sizeof pl.push);
    pl.pushMask = pp.pushMask;
    // the color formats of its key ("c<i>=<format>:<writeMask>:<blending>:..."), and which colors its draws read as they
    // were (m2v linear_attachment::writes: a written color blended into, or written through a mask that keeps channels)
    for (const char *at = pack_.string(pp.keyString); (at = strstr(at, " c")); at += 2) {
        unsigned c = 0, f = 0, mask = 0, blend = 0, factor[6] = {};
        const int n = sscanf(at, " c%u=%u:%u:%u:%u:%u:%*u:%u:%u", &c, &f, &mask, &blend, &factor[0], &factor[1],
                             &factor[2], &factor[3]);
        if (n >= 4 && c < 8) {
            pl.colorFormats[c] = f;
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
    if (const char *a0 = strstr(pack_.string(pp.keyString), " a0=");
        a0 && sscanf(a0, " a0=%u:%u:%u:%u:%u", &format, &offset, &buffer, &stride, &step) == 5 && format >= 29 &&
        format <= 31 && buffer == pp.vertexSlot && stride == pp.vertexStride && step == 1) {
        pl.positionComponents = uint8_t(format - 27);
        pl.positionOffset = uint16_t(offset);
    }
    pl.caTransform = caTransform;
    if (const char *ds = strstr(pack_.string(pp.keyString), "ds="))
        sscanf(ds, "ds=%u", &pl.depthFormat);
    // SPH word 0 bit 15 KILLS_PIXELS, bit 608 OMAP_SAMPLE_MASK (cla097sph.h); SET_DEPTH_TEST, SET_STENCIL_TEST and
    // SET_ANTI_ALIAS_ALPHA_CONTROL's alpha to coverage in bit 0
    uint32_t sph[20];
    memcpy(sph, pack_.section<uint8_t>(PackHeap) + progs[pp.fs].program, sizeof sph);
    bool tests = false;
    for (uint32_t k = 0; k < pp.blobDwords; k += 2) {
        const uint32_t m = blob[pp.blob + k], v = blob[pp.blob + k + 1];
        tests |= (m == DEPTH_TEST || m == STENCIL_TEST || m == ANTI_ALIAS_ALPHA_CONTROL) && (v & 1);
    }
    const char *key = pack_.string(pp.keyString);
    pl.dropsPixels =
        (sph[0] >> 15 & 1) || (sph[19] & 1) || tests || strncmp(key, "samples=1 ", 10) || !strstr(key, " ds=0/0 ");
    for (uint32_t k = 0; k < pp.entries; k++)
        if (pp.entry[k].kind == EntryFetch && pp.entry[k].slot < 8)
            pl.fetchBits |= uint8_t(1u << pp.entry[k].slot);
    if (interlocked)
        pl.storageBits = pl.fetchBits;
    pl.rec = rec;
    return &pl;
}

// ---- device: memory ----

// A span of `bytes` (page multiple) from the pool: first fit in a chunk, else a new chunk (one BoImport of fresh anonymous
// memory; its pages are wired and their zeros cleaned to memory by the kext: ~10 ms for 16 MiB on the device, r7l). A span of more
// than kDedicated gets a chunk of its own, given back whole when the span is freed. The shared chunks grow ahead of need: once their
// free bytes fall below kLowMark the grower thread makes the next one, so this (the render thread's resource creation) finds room;
// one that does not waits for a chunk the grower is making, or makes one itself (Stats::poolStalls: a burst outran the mark).
constexpr uint64_t kLowMark =
    kChunk;  // a whole chunk ahead: a burst of a chunk's worth finds room while the next is wired

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
            return stopping_ || growing_;
        });
        if (stopping_)
            return;
        uint32_t c;
        newChunk(kChunk, false, &l, c);  // failing, the render thread's next allocation tries itself
        growing_ = false;
        grown_.notify_all();
    }
}

bool Device::poolAlloc(uint64_t bytes, Span &s)
{
    bytes = (bytes + pageSize() - 1) & ~uint64_t(pageSize() - 1);
    s = {};
    if (bytes >
        kDedicated) {  // its own chunk, made now (rare: no Core Animation texture of the 720p screen is that large)
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
                if (poolFree_ < kLowMark && !growing_ && grower_.joinable()) {  // the next chunk, off this thread
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
        if (!newChunk(kChunk, false, nullptr, c))
            return false;
    }
    return false;
}

void Device::warm()
{
    {
        std::lock_guard<std::mutex> l(lock_);
        uint32_t c;
        while (poolFree_ <= kLowMark && newChunk(kChunk, false, nullptr, c)) {
        }  // the first and the one ahead of it
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
        stats.poolBytes -= ch.bytes;
        stats.poolChunks--;
        k_.free(ch.bo);
        vm_deallocate(mach_task_self(), vm_address_t(ch.memory), ch.bytes);
        ch = Chunk();
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
    // an empty chunk goes back while the others keep a chunk's worth free (kLowMark: the grower's own mark), so the pool
    // shrinks after a burst instead of holding its peak for good (r8a: 320 MB in 20 chunks from 15:18 on, the screen idle)
    if (!ch.used && poolFree_ >= ch.bytes + kLowMark) {
        poolFree_ -= ch.bytes;
        stats.poolBytes -= ch.bytes;
        stats.poolChunks--;
        k_.free(ch.bo);  // (the kernel keeps the pages until the work submitted before has completed)
        vm_deallocate(mach_task_self(), vm_address_t(ch.memory), ch.bytes);
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

// Texture header of a pitch-linear 2D texture (Mesa NIL nvb097_fill_image_view_desc for a linear image: HEADER_VERSION PITCH,
// TEXTURE_TYPE TWO_D_NO_MIPMAP), word 0 NVK's for the format
void Device::writeTic(const Texture &t)
{
    const FormatInfo *f = formatInfo(t.format);
    uint32_t w[8] = {f->tic0};
    fx::MipChain one;
    if (t.blockLinear && fx::mipChainLayout(t.width, t.height, 1, t.bpp, one)) {  // NIL's header of a one-level image
        fx::mipChainTic(one, t.va, f->tic0, f->srgb, 0, 1, w);
        memcpy(tic_.cpu + 32 * t.tic, w, 32);
        return;
    }
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
    setField(w, 159, 157, 7);              // BORDER_SIZE BORDER_SAMPLER_COLOR
    setField(w, 191, 191, 1);              // NORMALIZED_COORDS
    setField(w, 216, 215, 2);              // ANISO_FINE_SPREAD_FUNC TWO
    setField(w, 218, 217, 1);              // ANISO_COARSE_SPREAD_FUNC ONE
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // write-combined: visible at the next submission's header invalidate
}

// A pitch-linear color target: its address and pitch multiples of 128 (NVK renders a linear image only so, nvk_rendering_linear,
// "the render hardware gets grumpy" otherwise; m2v's NVK renders any other through a tiled shadow, which the raw path leaves to
// m2v)
static bool linearTarget(const Texture &t)
{
    return !((t.va | t.pitch) & 127);
}

static bool shape(uint32_t width, uint32_t height, uint32_t format, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    if (!f || !width || !height || width > 16384 || height > 16384)
        return false;
    t = {};
    t.width = width;
    t.height = height;
    t.format = format;
    t.bpp = f->bpp;
    t.renderable = f->rt != 0;
    return true;
}

bool Device::createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc, bool tiled)
{
    if (!shape(width, height, format, t))
        return false;
    t.pitch = (width * t.bpp + 255) & ~255u;
    std::lock_guard<std::mutex> l(lock_);
    const uint64_t bytes = uint64_t(t.pitch) * height;
    tgm_layout bl;
    // (a colour format: the chunks are bound under colour kind 0xFE, which a depth/stencil format's zeta is not)
    if (tiled && !wc && t.renderable && tgm_layout_tiled(width, height, t.bpp, &bl) &&
        blAlloc(bl.bytes, bl.align, t.span)) {
        t.pitch = bl.row_bytes;
        t.blockLinear = true;
        t.blockHeight = bl.y_log2;
        t.layerBytes = bl.bytes;
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
        if (!createTexture(width, height, format, *sampled)) {
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

bool Device::createBufferTexture(const Buffer &b, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t format, Texture &t)
{
    if (!shape(width, height, format, t) || (pitch & 31) || pitch < width * t.bpp || ((b.va + offset) & 31) ||
        offset + uint64_t(pitch) * height > b.length)
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
    if (of.chain)
        return false;  // a view of a mipmapped texture: m2v's
    const FormatInfo *f = formatInfo(format);
    if (!f || f->bpp != of.bpp || of.memoryless || of.sampled)
        return false;
    std::lock_guard<std::mutex> l(lock_);
    t = of;
    t.format = format;
    t.renderable = f->rt != 0 && (t.blockLinear || linearTarget(t));
    t.shadowTarget = f->rt != 0 && !t.blockLinear && !linearTarget(t);
    t.import = nullptr;  // the viewed texture owns the memory (RawMetal.mm keeps it alive while a view lives)
    t.span = {};
    t.lease = nullptr;
    if (!(t.tic = allocTic()))
        return false;
    writeTic(t);
    return true;
}

bool Device::createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t)
{
    uint64_t bytes, align;
    if (format == kZetaFormat ? !zetaShape(width, height, t, bytes, align) : !shape(width, height, format, t))
        return false;
    t.memoryless = true;
    return true;
}

// ---- block-linear memory (raw/fx mip chains: kind 0xFE; zeta: kZetaPteKind), made on demand ----

constexpr uint64_t kBlChunk = 16ull << 20;  // a chunk of images (an image over half of one gets its own)
constexpr uint64_t kBlAlign = 16384;        // NVK's image alignment here (r12-cafx niloracle), a multiple of NIL's

bool Device::tiledAlloc(std::vector<Chunk> &chunks, uint32_t kind, uint64_t bytes, uint64_t align, Span &s)
{
    align = std::max(align, kBlAlign);
    bytes = (bytes + kBlAlign - 1) & ~(kBlAlign - 1);
    s = {};
    for (int pass = 0; pass < 2; pass++) {
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
                return true;
            }
        }
        if (pass)
            break;
        const bool dedicated = bytes > kBlChunk / 2;
        const uint64_t size = dedicated ? bytes : kBlChunk;
        vm_address_t memory = 0;
        if (vm_allocate(mach_task_self(), &memory, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
            return false;
        Chunk ch;
        ch.memory = reinterpret_cast<void *>(memory);
        ch.bytes = size;
        ch.dedicated = dedicated;
        if (!k_.importPages(ch.memory, size, ch.bo) || !k_.bindImport(ch.memory, ch.bo, kind) ||
            (ch.bo.va & (kBlAlign - 1))) {
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
        stats.poolBytes += size;
        stats.poolChunks++;
        if (dedicated) {
            ch.used = bytes;
            s = {c, 0, bytes, ch.bo.va, static_cast<uint8_t *>(ch.memory)};
            chunks[c] = std::move(ch);
            return true;
        }
        ch.free[0] = size;
        chunks[c] = std::move(ch);
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
        stats.poolBytes -= ch.bytes;
        stats.poolChunks--;
        k_.free(ch.bo);
        vm_deallocate(mach_task_self(), vm_address_t(ch.memory), ch.bytes);
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
}

bool Device::createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t)
{
    const FormatInfo *f = formatInfo(format);
    const uint32_t fmt2d = fx::twodFormat(format);
    if (!chainsOn_ || !f || !fmt2d || f->srgb || levels < 2 || !f->rt)
        return false;
    Texture::Chain *c = new Texture::Chain;
    if (!fx::mipChainLayout(width, height, levels, f->bpp, c->layout) || !createTexture(width, height, format, t)) {
        delete c;
        return false;
    }
    bool placed;
    {
        std::lock_guard<std::mutex> l(lock_);
        placed = blAlloc(c->layout.bytes, c->layout.align, c->span);
    }
    if (!placed) {
        delete c;
        destroyTexture(t);
        return false;
    }
    c->va = c->span.va;
    c->fmt2d = fmt2d;
    uint32_t w[8];
    fx::mipChainTic(c->layout, c->va, f->tic0, f->srgb, 0, levels, w);
    memcpy(tic_.cpu + 32 * t.tic, w, 32);  // the texture's header: the chain's (visible at the next header invalidate)
    t.chain = c;
    stats.chains++;
    stats.chainBytes += c->layout.bytes;
    return true;
}

// Once at open, after the init submission: the 2D engine's state (Mesa nvc0's 2D init), then one 24x10 chain per sampling
// candidate (fx::sampling) made from a pattern in one submission and read back (fx_mip.h). Chains go on with the first candidate
// whose chain is exact within 1 in either of the CPU's two possible views of kind-0xFE memory (production never reads a chain on
// the CPU; the view only tells the check where to look); none: chains stay off and every texture is as before. Its memory is a
// coherent allocation: the CPU reads it without an invalidate. Nothing but this submission's own fence is waited for.
void Device::selfTest()
{
    for (uint32_t i = 0; i < fx::kSamplings; i++)
        selfTestTegra[i] = selfTestFermi[i] = ~0u;
    fx::MipChain c;
    if (!fx::mipChainLayout(fx::kSelfTestW, fx::kSelfTestH, fx::fullLevels(fx::kSelfTestW, fx::kSelfTestH), 4, c))
        return;
    const uint64_t stride = (c.bytes + 16383) & ~uint64_t(16383);
    Bo chains, src;
    if (!k_.alloc(stride * fx::kSamplings, true, chains, fx::kBlockLinearKind))
        return;
    if (!k_.alloc(4096, true, src)) {
        k_.free(chains);
        return;
    }
    memset(chains.cpu, 0, stride * fx::kSamplings);
    fx::selfTestPattern(src.cpu, 256);
    const uint32_t room = uint32_t(fx::twodInitWords() + fx::kSamplings * fx::generateWords(c, 0, c.levels - 1) + 16);
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, room * 4, 256, va));
    Push p{start, start + room};
    fx::emitTwodInit(p);
    const fx::Pitch l0{src.va, 256, fx::kSelfTestW, fx::kSelfTestH};
    for (uint32_t i = 0; i < fx::kSamplings; i++)
        fx::emitGenerate(p, c, chains.va + i * stride, &l0, fx::twodFormat(80), 0, c.levels - 1, fx::sampling(i));
    TegraGM20BAbiPush seg = {va, uint32_t(p.at - start), 0};
    uint32_t seq = 0;
    if (k_.exec(channel_, &seg, 1, lastSeqno_, seq)) {
        lastSeqno_ = seq;
        retire(push_, seq);
        if (k_.wait(channel_, seq)) {
            for (uint32_t i = 0; i < fx::kSamplings; i++) {
                const uint8_t *cpu = chains.cpu + i * stride;
                selfTestTegra[i] = fx::selfTestCheck(c, cpu, nullptr, false);
                selfTestFermi[i] = fx::selfTestCheck(c, cpu, nullptr, true);
            }
            fx::selfTestDetail(c, chains.cpu, false, selfTestFirst);
            for (uint32_t view = 0; view < 2 && selfTestChosen == fx::kSamplings; view++)
                for (uint32_t i = 0; i < fx::kSamplings; i++)
                    if (!(view ? selfTestFermi[i] : selfTestTegra[i])) {
                        selfTestChosen = i;
                        selfTestFermiView = view;
                        break;
                    }
            if (selfTestChosen < fx::kSamplings) {
                sampling_ = fx::sampling(selfTestChosen);
                chainsOn_ = true;
            }
        }
    }
    k_.free(src);
    k_.free(chains);
}

void Device::destroyTexture(Texture &t)
{
    for (Texture *plane : {t.luma, t.chroma})
        if (plane) {
            destroyTexture(*plane);
            delete plane;
        }
    t.luma = t.chroma = nullptr;
    if (t.sampled) {  // its header is the pool image's
        destroyTexture(*t.sampled);
        delete t.sampled;
        t.sampled = nullptr;
        t.tic = 0;
    }
    std::lock_guard<std::mutex> l(lock_);
    if (t.tic) {
        memset(tic_.cpu + 32 * t.tic, 0, 32);
        freeTic_.push_back(t.tic);
    }
    if (t.chain) {
        for (const uint32_t level : t.chain->levelTic)
            if (level) {
                memset(tic_.cpu + 32 * level, 0, 32);
                freeTic_.push_back(level);
            }
        blFree(t.chain->span);
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

constexpr uint64_t kWcChunk = 4ull << 20;  // a chunk of buffers (a buffer over half of one gets its own BO)

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
        const bool dedicated = bytes > kWcChunk / 2;
        const uint64_t size = dedicated ? bytes : kWcChunk;
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

bool Device::createBufferNoCopy(void *bytes, uint64_t length, Buffer &b)
{
    b = {};
    if (!bytes || !length || (uintptr_t(bytes) & (pageSize() - 1)))
        return false;
    std::lock_guard<std::mutex> l(lock_);
    if (!(b.import = importSurface(0, bytes, (length + pageSize() - 1) & ~uint64_t(pageSize() - 1), nullptr)))
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
    freeSpan(b.span, b.wc ? Memory::Wc : Memory::Pool, b.lease);
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

bool Device::createSampler(const SamplerDesc &d, Sampler &s)
{
    uint32_t w[8];
    samplerHeader(d, w);
    s.desc = d;
    std::lock_guard<std::mutex> l(lock_);
    if (!freeTsc_.empty()) {
        s.tsc = freeTsc_.back();
        freeTsc_.pop_back();
    } else if (nextTsc_ < kTscCount) {
        s.tsc = nextTsc_++;
    } else {
        return false;
    }
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
    if (!freeTsc_.empty()) {
        tsc = freeTsc_.back();
        freeTsc_.pop_back();
    } else if (nextTsc_ < kTscCount) {
        tsc = nextTsc_++;
    } else {
        return false;
    }
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
    if (!createTexture(width, height, plan.image, t))
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
Texture *Device::backing(const Texture &m, uint32_t tag, bool blockLinear)
{
    blockLinear |= m.zeta;
    for (Backing *b : backings_)
        if (b->texture.width == m.width && b->texture.height == m.height && b->texture.format == m.format &&
            b->tag == tag && b->texture.blockLinear == blockLinear) {
            b->lastCommit = commitCount_;
            return &b->texture;
        }
    Backing *b = new Backing;
    Texture &t = b->texture;
    if (m.zeta) {
        uint64_t bytes, align;
        if (!zetaShape(m.width, m.height, t, bytes, align) ||
            !tiledAlloc(zChunks_, kZetaPteKind, bytes, align, t.span)) {
            delete b;
            return nullptr;
        }
        t.va = t.span.va;
    } else if (blockLinear) {
        fx::MipChain c;
        const FormatInfo *f = formatInfo(m.format);
        if (!f || !shape(m.width, m.height, m.format, t) || !fx::mipChainLayout(m.width, m.height, 1, t.bpp, c) ||
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
    b->lastCommit = commitCount_;
    backings_.push_back(b);
    return &t;
}

// lock_ held
void Device::freeBacking(Texture &t)
{
    if (t.tic) {
        memset(tic_.cpu + 32 * t.tic, 0, 32);
        freeTic_.push_back(t.tic);
    }
    if (t.zeta)
        tiledFree(zChunks_, t.span);
    else if (t.blockLinear)
        blFree(t.span);
    else
        poolFree(t.span);
}

// Backings no pass used for 600 command buffers (~10 s of frames) and whose last use completed go back to the pool.
void Device::trimBackings()
{
    for (auto it = spares_.begin(); it != spares_.end();) {
        if (commitCount_ - it->freedAt > 600) {
            Chunk &ch = chunks_[it->chunk];
            stats.poolBytes -= ch.bytes;
            stats.poolChunks--;
            k_.free(ch.bo);
            vm_deallocate(mach_task_self(), vm_address_t(ch.memory), ch.bytes);
            ch = Chunk();
            it = spares_.erase(it);
            continue;
        }
        ++it;
    }
    for (auto it = backings_.begin(); it != backings_.end();) {
        Backing *b = *it;
        if (commitCount_ - b->lastCommit > 600 && (!b->lastUse || done(b->lastUse))) {
            freeBacking(b->texture);
            delete b;
            it = backings_.erase(it);
            continue;
        }
        ++it;
    }
}

// ---- command buffer ----

void Device::noteLost()
{
    lostThrough_.store(__atomic_load_n(&lastSeqno_, __ATOMIC_RELAXED), std::memory_order_relaxed);
    lostAny_.store(true, std::memory_order_release);
}

// The kernel killed the channel (a GPU fault or hang, its own or one it was stuck behind; as a GPU restart on iOS): what it
// had in flight failed, and a new channel, named by ChannelCreate's reply, gets the context the old one took with it.
// Between command buffers: no span of either ring is open, and every one the old channel had is done (the kernel released
// its fence), whatever word the new channel's fence is.
bool Device::reopen()
{
    noteLost();
    k_.closeChannel(channel_);
    if (!k_.channel(kEngines, channel_, fence_))
        return false;
    stats.reopens++;
    push_.inflight.clear();
    upload_.inflight.clear();
    {
        std::lock_guard<std::mutex> l(lock_);
        for (Backing *b : backings_)
            b->lastUse = 0;
    }
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
        return false;
    lastSeqno_ = seq;
    retire(push_, seq);
    return true;
}

CommandBuffer *Device::begin()
{
    if (k_.lost(channel_))
        reopen();
    CommandBuffer &cb = cb_;
    cb.dev_ = this;
    cb.segments_.clear();
    cb.clean_.clear();
    cb.flush_.clear();
    cb.backings_.clear();
    cb.surfaces_.clear();
    cb.draws = cb.passes = cb.blits = 0;
    cb.refusal = {};
    cb.pushStart_ = push_.head;
    cb.uploadStart_ = upload_.head;
    memcpy(cb.rootPush_, rootPush_, sizeof rootPush_);
    cb.restart_ = restart_;
    cb.ds_ = ds_;
    cb.drawn_ = false;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (commitCount_ % 64 == 0)
            trimBackings();
    }
    // submission prologue, as NVK opens each submission: header/sampler caches (headers written since), shader and texture caches
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(alloc(push_, 64 * 4, 256, va));
    Push p{start, start + 64};
    p.m(INVALIDATE_SAMPLER_CACHE_NO_WFI, 0);
    p.m(INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);
    p.m(INVALIDATE_SHADER_CACHES_NO_WFI, 0x1000);
    p.m(INVALIDATE_SHADER_CACHES, 0x14);
    p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    p.m(INVALIDATE_SHADER_CACHES_NO_WFI, 0x10);
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
    e = RenderEncoder();
    e.cb_ = this;
    e.dev_ = dev_;
    e.pass_ = pass;
    // the render hardware takes one memory layout for all of a pass's color targets (nvk_rendering_linear): block-linear when
    // one of them is or beside a zeta, its pitch ones then rendered through block-linear shadows as NVK does; memoryless ones
    // get a backing of the pass's layout
    const Texture *zeta = pass.depthStencil.texture;
    if (zeta && (!zeta->zeta || !zeta->memoryless))
        return nullptr;
    bool blockLinear = zeta != nullptr;
    for (uint32_t j = 0; j < pass.count && j < 8; j++)
        if (const Texture *t = pass.color[j].texture; t && !t->memoryless && t->blockLinear)
            blockLinear = true;
    if (zeta) {
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
    // the body goes straight into the push ring; the pass prologue (targets, clears) is written at end() and runs first
    e.reserve();
    e.setViewport(0, 0, e.areaW_, e.areaH_, 0, 1);
    e.setScissor(0, 0, e.areaW_, e.areaH_);
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
        if (!d.k_.exec(d.channel_, segments_.data() + at, n, d.lastSeqno_, seq, &surfaces_))
            return false;
        d.lastSeqno_ = seq;
    }
    seqno = seq;
    d.retire(d.push_, seq);
    d.retire(d.upload_, seq);
    memcpy(d.rootPush_, rootPush_, sizeof rootPush_);
    d.restart_ = restart_;
    d.ds_ = ds_;
    {
        std::lock_guard<std::mutex> l(d.lock_);
        for (Texture *b : backings_)
            for (Device::Backing *k : d.backings_)
                if (&k->texture == b)
                    k->lastUse = seq;
        d.commitCount_++;
    }
    d.stats.commits++;
    return true;
}

void CommandBuffer::abandon()
{
    Device &d = *dev_;
    for (Device::Ring *r : {&d.push_, &d.upload_}) {
        // drop the open command buffer's spans and give back what it took (nothing else was allocated since: one encodes)
        for (auto it = r->inflight.begin(); it != r->inflight.end();)
            it = it->seqno ? std::next(it) : r->inflight.erase(it);
        r->head = r->since = (r == &d.push_ ? pushStart_ : uploadStart_);
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

void RenderEncoder::barrier()
{
    if (p_.room() < 64)
        reserve();
    p_.m(WAIT_FOR_IDLE, 0);
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
        p_.m(BEGIN, i ? 1u << 26 : 0);  // INSTANCE_ID FIRST, then SUBSEQUENT (Maxwell has no instance count)
        p_.inc(VERTEX_ARRAY_START, {first, count});
        p_.m(END, 0);
    }
    cb_->draws++;
    return true;
}

// The runs a draw that fetches a color it writes is drawn as, where its own triangles may cover a pixel twice
// (raw_overlap.h); empty: it is drawn whole. The indexed draw being prepared sets drawIndices_ first
const RenderEncoder::Runs &RenderEncoder::selfRuns(const Pipeline &pl, uint32_t primitive, uint32_t first,
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
// the index size) where it changed, the draw parameters as NVK_MME_DRAW_INDEXED's (base vertex 0: those of a draw from vertex 0),
// then BEGIN, SET_INDEX_BUFFER_F (first index) and DRAW_INDEX_BUFFER (count), END. m2v binds the indices the draw reads.
bool RenderEncoder::drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib,
                                uint64_t offset, const char **why)
{
    Pipeline *pl = pipeline_;
    if (!pl)
        return true;
    called_ = true;
    readBits_ |= pl->fetchBits;
    if (!count)
        return true;
    if (!ib || (indexSize != 2 && indexSize != 4) || offset % indexSize ||
        offset + uint64_t(count) * indexSize > ib->length) {
        *why = "an indexed draw the raw path does not take";
        cb_->refusal = {RefIndexedRange, uint32_t(offset), indexSize,
                        ib ? uint32_t(std::min<uint64_t>(ib->length, ~0u)) : 0};
        return false;
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
    const Runs &runs = selfRuns(*pl, primitive, 0, count, 1);
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
        const bool prepared = prepare(pl, primitive, 0, 0, most + 1, 0, true, why);
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
        p_.m(BEGIN, 0);
        p_.inc(INDEX_BUFFER_F, {begin, end - begin});  // SET_INDEX_BUFFER_F (first index), DRAW_INDEX_BUFFER (count)
        p_.m(END, 0);
    }
    cb_->draws++;
    return true;
}

bool RenderEncoder::prepare(Pipeline *pl, uint32_t primitive, uint32_t first, uint32_t vFirst, uint32_t vCount,
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
    if (baseInstance) {
        *why = "draw with a base instance";
        cb_->refusal = {RefBaseInstance, baseInstance, 0, 0};
        return false;
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
    if (pl->depthFormat != (zeta ? kZetaFormat : 0) || (!zeta && ds_.active())) {
        *why = "a depth/stencil state or format the pass's depth/stencil attachment does not take";
        cb_->refusal = {RefDepthStencil, pl->depthFormat, zeta ? kZetaFormat : 0, ds_.active()};
        return false;
    }
    if (p_.room() < 1024)
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
        emitDepthStencil(p, depthStencilRegs(ds_, stencilRef_[0], stencilRef_[1], zeta != nullptr), cb_->ds_);
        dsDirty_ = false;
    }
    uint32_t *root = cb_->rootPush_;
    const PackPipeline &r = *pl->rec;
    // set 0 first (it may refuse the draw before anything is written): the layout's entries only
    uint64_t setVa;
    uint32_t *set = reinterpret_cast<uint32_t *>(d.alloc(d.upload_, kSetBytes, 256, setVa));
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackEntry &e = r.entry[i];
        uint32_t *at = set + e.offset / 4;
        if (e.kind == EntryUbo) {
            // m2v's descriptor range (bind/instantiate.rs descriptor_range): the function's window, clamped to the bytes padded to
            // 16; NVK then rounds a uniform buffer's range up to the 256-byte cbuf alignment (nvk_ubo_descriptor)
            const Bound &b = bufs_[e.stage][e.slot];
            const uint32_t window = (uint32_t(e.sampler) | uint32_t(e.pad) << 8) * 16;
            uint32_t range = std::min<uint32_t>((b.size + 15) & ~15u, 0x10000u);  // a uniform range is at most 64 KiB
            if (window && window < range)
                range = window;
            if (!e.samplerKind)
                range = (range + 255) & ~255u;
            // A Metal offset that is not a multiple of 256 needs m2v's SlotBindOffset residual in the push window: not in the pack
            // (every probe bound at offset 0)
            if ((b.va & 255) && !e.samplerKind) {
                *why = "a uniform buffer bound at an offset that is not a multiple of 256";
                cb_->refusal = {RefUboOffset, uint32_t(e.stage) << 8 | e.slot, uint32_t(b.va & 255),
                                uint32_t(std::min<uint64_t>(b.offset, ~0u))};
                return false;
            }
            if (b.buffer && !b.buffer->wc)
                cb_->clean(b.buffer->cpu + b.offset, std::min<uint64_t>(range, b.size));
            at[0] = uint32_t(b.va);
            at[1] = uint32_t(b.va >> 32);
            at[2] = b.va ? range : 0;
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
            tic = t ? t->tic : 0;
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
    writeRaster(p, raster_);
    if (pl != emitted_) {
        blendColorDirty_ |= pl->constantBlend;
        p.raw(pl->methods.data(), uint32_t(pl->methods.size()));
        // the state's push words into the root table, as m2v's vkCmdPushConstants ranges reach it through NVK: the words its
        // ranges cover (the rest keep what an earlier state wrote), one LOAD_CONSTANT_BUFFER per run of words that change
        for (int i = 0; i < 32;) {
            if (!(pl->pushMask >> i & 1) || root[i] == pl->push[i]) {
                i++;
                continue;
            }
            int j = i;
            while (j < 32 && (pl->pushMask >> j & 1) && root[j] != pl->push[j])
                j++;
            p.m(LOAD_CB_OFFSET, 0x700 + 4 * i);
            p.nonInc(LOAD_CB, pl->push + i, j - i);
            memcpy(root + i, pl->push + i, (j - i) * 4);
            i = j;
        }
        emitted_ = pl;
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
        const uint64_t limit =
            b.buffer ? b.va + b.size - 1 : b.va + ((b.size + kUboChunk - 1) & ~uint64_t(kUboChunk - 1)) - 1;
        if (b.va != vbVa_ || limit != vbLimit_) {
            p.inc(VERTEX_STREAM_LOCATION, {uint32_t(b.va >> 32), uint32_t(b.va)});
            p.inc(VERTEX_STREAM_LIMIT, {uint32_t(limit >> 32), uint32_t(limit)});
            vbVa_ = b.va;
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
        p.inc(CB_SELECTOR_A, {kRootBytes, uint32_t(d.root_.va >> 32), uint32_t(d.root_.va)});
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    countRules(*pl);
#endif
    // framebuffer fetch reads a color attachment through the texture unit: a barrier where a draw since the last one wrote a
    // color this draw fetches (m2v's barrier, 0106, follows any color write: the ROP writes only the colors SET_CT_WRITE opens).
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
    if (storage || (texFetched & writtenBits_)) {
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
    if (first != first_ || baseInstance != baseInstance_) {
        // nvk_mme_build_set_draw_params: root draw.vs.{base_vertex, base_instance} and the base registers
        const uint32_t v[2] = {first, baseInstance};
        p.m(LOAD_CB_OFFSET, 0);
        p.nonInc(LOAD_CB, v, 2);
        p.m(GLOBAL_BASE_INSTANCE_INDEX, baseInstance);
        first_ = first;
        baseInstance_ = baseInstance;
    }
    writtenBits_ |= pl->writeBits;
    storageWrittenBits_ |= pl->storageBits;
    if (pl->writeBits)
        written_.add(g);
    return true;
}

// What the region test reads of a draw: Core Animation's transform (Pipeline::caTransform) over a triangle list or strip
// of one instance, with its matrix, positions and indices where the CPU maps them; anything else is its scissor alone
DrawGeometry RenderEncoder::geometry(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count,
                                     const PixelBox &limit) const
{
    DrawGeometry g;
    g.limit = limit;
    g.viewport = {vp_[0], vp_[1], vp_[2], vp_[3]};
    g.first = drawIndices_ ? 0 : first;
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

// The clear value m2v hands Vulkan for a Metal clear color of an attachment of `format` (metal2vulkan-runtime clear_value.rs
// `color`): what NVK's clear writes, as Metal's conversion stores it. Normalized channels through float, clamped, k / 255 of the
// nearest integer k (ties to even); half channels the value of the half nearest (ties to even); float channels themselves (a
// NaN quieted); the channels a format has none of 0.
static void clearValue(uint32_t format, const float in[4], uint32_t out[4])
{
    auto unorm8 = [](float x) {
        const double v = std::isnan(x) ? 0.0 : std::clamp(double(x), 0.0, 1.0);
        return fbits(float(std::nearbyint(v * 255.0) / 255.0));
    };
    auto exact = [](float x) {
        return std::isnan(x) ? (fbits(x) | 0x7fc00000u) : fbits(x);
    };
    auto half = [&](float x) {
        return std::isnan(x) ? exact(x) : fbits(float(_Float16(x)));
    };
    for (int i = 0; i < 4; i++)
        switch (format) {
        case 70:
        case 80:
            out[i] = unorm8(in[i]);
            break;
        case 55:
            out[i] = i ? 0u : exact(in[i]);
            break;
        case 115:
            out[i] = half(in[i]);
            break;
        default:
            out[i] = exact(in[i]);
            break;  // RGBA32Float
        }
}

namespace {
// One side of a copy-engine copy: pitch (va the first texel's, rows `pitch` apart) or a block-linear image (va its first byte,
// `pitch` its row stride, the copy from texel (x, y) of its width x height)
struct CopySide {
    uint64_t va = 0;
    uint32_t pitch = 0, x = 0, y = 0, width = 0, height = 0;
    bool blockLinear = false;
    uint8_t blockHeight = 0;
};

CopySide sideOf(const Texture &t, uint32_t x, uint32_t y)
{
    if (!t.blockLinear)
        return {t.va + uint64_t(y) * t.pitch + uint64_t(x) * t.bpp, t.pitch};
    return {t.va, t.pitch, x, y, t.pitch / t.bpp, t.height, true, t.blockHeight};
}

// `w` x `h` texels of `bpp` bytes as nouveau_copy_rect moves them: a remap of min(bpp, 4)-byte components, so lengths, widths
// and origins count texels; a block-linear side by its block size (one GOB wide, Fermi's 8-row GOBs), its width the row
// stride in texels, one layer
void copySides(Push &p, const CopySide &s, const CopySide &d, uint32_t w, uint32_t h, uint32_t bpp,
               Device::Stats &stats)
{
    if (!w || !h || !bpp)
        return;
    stats.ceOps++;
    stats.ceBytes += uint64_t(w) * bpp * h;
    const uint32_t comp = std::min(bpp, 4u), n = bpp / comp;
    // DST_X..W = SRC_X..W, COMPONENT_SIZE, NUM_SRC_COMPONENTS, NUM_DST_COMPONENTS
    p.m(CE_SET_REMAP_COMPONENTS, 0 | 1 << 4 | 2 << 8 | 3 << 12 | (comp - 1) << 16 | (n - 1) << 20 | (n - 1) << 24,
        SUBC_COPY);
    p.inc(CE_OFFSET_IN_UPPER,
          {uint32_t(s.va >> 32), uint32_t(s.va), uint32_t(d.va >> 32), uint32_t(d.va), s.pitch, d.pitch, w, h},
          SUBC_COPY);
    constexpr uint32_t kGobHeightFermi8 = 1u << 12, kSrcPitch = 1u << 7, kDstPitch = 1u << 8;
    uint32_t launch = (kLaunchPitch & ~(kSrcPitch | kDstPitch)) | 1u << 10;  // REMAP_ENABLE
    if (s.blockLinear)
        p.inc(CE_SET_SRC_BLOCK_SIZE,
              {uint32_t(s.blockHeight) << 4 | kGobHeightFermi8, s.width, s.height, 1, 0, s.y << 16 | s.x}, SUBC_COPY);
    else
        launch |= kSrcPitch;
    if (d.blockLinear)
        p.inc(CE_SET_DST_BLOCK_SIZE,
              {uint32_t(d.blockHeight) << 4 | kGobHeightFermi8, d.width, d.height, 1, 0, d.y << 16 | d.x}, SUBC_COPY);
    else
        launch |= kDstPitch;
    p.m(CE_LAUNCH_DMA, launch, SUBC_COPY);
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
    d.height = t.height;
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

bool chainLevelTarget(const Texture &t, uint32_t level, Texture &out)
{
    const FormatInfo *f = formatInfo(t.format);
    ColorTargetDesc d;
    if (!t.chain || !f || !f->rt || !chainLevelDesc(t.chain->layout, t.chain->va, level, f->rt, d))
        return false;
    out = Texture();
    out.va = d.va;
    out.width = t.chain->layout.level[level].width;
    out.height = d.height;
    out.format = t.format;
    out.pitch = d.pitch;
    out.bpp = t.bpp;
    out.renderable = true;
    out.blockLinear = true;
    out.blockHeight = d.blockHeight;
    out.layerBytes = d.layerBytes;
    return true;
}

// NVK's nvk_flush_rs_state for the state m2v makes of Metal's (render_state.rs `fixed`): culling on and the face culled,
// the front face, and the polygon mode of both faces; a register already holding its value is not written
void RenderEncoder::writeRaster(Push &p, const Raster &want)
{
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
    if (p_.room() < 16)
        reserve();
    writeRaster(p_, Raster());
    if (p_.at > bodyStart_)
        bodies_.push_back({bodyStart_, p_.at});
    d.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - d.push_.bo.cpu);  // give back the unused reservation
    // pass prologue: the full barrier m2v puts between passes, the targets, then the clears
    const std::vector<Clear> cl = clears();
    const uint32_t room = 160 + 8 * 32 + uint32_t(cl.size()) * 48 + kZetaTargetWords + kZetaClearWords + 16;
    uint64_t va;
    uint32_t *start = reinterpret_cast<uint32_t *>(d.alloc(d.push_, room * 4, 256, va));
    Push p{start, start + room};
    p.m(WAIT_FOR_IDLE, 0);
    p.m(INVALIDATE_SHADER_CACHES, 0x14);
    p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    // shadow targets' pool images: their texels over the render area first, as NVK's tiled shadow loads them (a load that
    // keeps them: Load, and a DontCare the clears leave; a Clear clears it all), then the barrier to the pass
    bool copied = false;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++)
        if (const Texture *o = shadowOf_[j]) {
            if (pass_.color[j].load == ColorAttachment::Clear)
                continue;
            copySides(p, sideOf(*o, 0, 0), sideOf(*pass_.color[j].texture, 0, 0), areaW_, areaH_, o->bpp, d.stats);
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
        p.m(COLOR_COMPRESSION, 0);
        uint32_t map = 0;
        for (uint32_t k = 0; k < 8; k++)
            map |= k << (4 + 3 * k);
        p.m(CT_SELECT, map | 1);
        p.inc(SURFACE_CLIP_H, {c.x1 << 16, c.y1 << 16});
        p.m(ZT_SELECT, 0);
        uint32_t v[4];
        clearValue(pass_.color[c.target].texture->format, c.value, v);
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
        p.m(INVALIDATE_SHADER_CACHES, 0x14);
        p.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0);
    }
    uint32_t ct = 0, count = 0;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const Texture *t = pass_.color[j].texture;
        if (!t)
            continue;
        targetWords(p, COLOR_TARGET + 0x40 * j, *t);
        p.m(COLOR_COMPRESSION + 4 * j, 0);
        count++;
    }
    // targets map to themselves (NVK writes all eight slots of the map; the hardware reads the first `count`)
    for (uint32_t k = 0; k < 8; k++)
        ct |= k << (4 + 3 * k);
    p.m(CT_SELECT, ct | count);
    p.inc(SURFACE_CLIP_H, {areaW_ << 16, areaH_ << 16});  // the render area (nvk_CmdBeginRendering)
    const DepthStencilAttachment &zs = pass_.depthStencil;
    zetaTargetWords(p, zs.texture);
    // NVK clears depth and stencil with the first color the pass clears over its render area, else on their own
    uint32_t zsBits = zs.texture ? zetaClearBits(zs.clearDepth, zs.clearStencil) : 0;
    zetaClearValues(p, zsBits & 1, zs.depth, zsBits & 2, zs.stencil);
    for (const Clear &c : cl) {
        if (c.own)
            continue;
        uint32_t v[4];
        clearValue(pass_.color[c.target].texture->format, c.value, v);
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
            copySides(q, sideOf(*pass_.color[j].texture, 0, 0), sideOf(*o, 0, 0), w, h, o->bpp, d.stats);
        }
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

void BlitEncoder::copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width,
                           uint32_t lines, uint32_t bpp)
{
    if (!width || !lines || !bpp)
        return;
    room();
    copyRows(p_, src, srcPitch, dst, dstPitch, width, lines, bpp, dev_->stats);
}

void BlitEncoder::copyTexels(const Texture &src, uint32_t sx, uint32_t sy, const Texture &dst, uint32_t dx, uint32_t dy,
                             uint32_t w, uint32_t h)
{
    room(32);
    copySides(p_, sideOf(src, sx, sy), sideOf(dst, dx, dy), w, h, src.bpp, dev_->stats);
}

void BlitEncoder::copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t w,
                                   uint32_t h, bool toTexture)
{
    room(32);
    const CopySide rows{va, pitch}, texels = sideOf(t, x, y);
    copySides(p_, toTexture ? rows : texels, toTexture ? texels : rows, w, h, t.bpp, dev_->stats);
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

void BlitEncoder::generate(Texture &t, bool all)
{
    Texture::Chain *c = t.chain;
    if (!c)
        return;
    const uint32_t last = all ? c->layout.levels - 1 : 0;
    room(uint32_t(fx::generateWords(c->layout, 0, last)));
    const fx::Pitch l0{t.va, t.pitch, t.width, t.height};
    fx::emitGenerate(p_, c->layout, c->va, &l0, c->fmt2d, 0, last, dev_->sampling_);
    c->l0Fresh = true;
    if (all) {
        c->levelsFresh = true;
        c->rawLevels = c->mirrorLevels = 0;
        dev_->stats.chainGenerates++;
    } else {
        dev_->stats.chainRefreshes++;
    }
}

void BlitEncoder::copyFromLevel(const Texture &t, uint32_t level, uint32_t sx, uint32_t sy, const Texture &dst,
                                uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    const Texture::Chain *c = t.chain;
    if (!c)
        return;
    if (dst.blockLinear) {  // the copy engine, block-linear to block-linear (the 2D engine's destination here is pitch)
        Texture from;
        if (chainLevelTarget(t, level, from))
            copyTexels(from, sx, sy, dst, dx, dy, w, h);
        dev_->stats.chainCopies++;
        return;
    }
    room(uint32_t(fx::copyWords()));
    const fx::Pitch d{dst.va, dst.pitch, dst.width, dst.height};
    fx::emitCopyFromLevel(p_, c->layout, c->va, level, d, c->fmt2d, sx, sy, dx, dy, w, h);
    dev_->stats.chainCopies++;
}

void BlitEncoder::copyToLevel(Texture &t, uint32_t level, const Texture &src)
{
    Texture::Chain *c = t.chain;
    if (!c || !level || level >= c->layout.levels)
        return;
    const fx::MipLevel &l = c->layout.level[level];
    room(uint32_t(fx::copyWords()));
    const fx::Pitch s{src.va, src.pitch, src.width, src.height};
    fx::emitCopyToLevel(p_, c->layout, c->va, level, s, c->fmt2d, 0, 0, 0, 0, l.width, l.height);
}

void BlitEncoder::end()
{
    cb_->segment(start_, p_.at);
    dev_->push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_->push_.bo.cpu);
    dev_->stats.pushBytes += uint64_t(p_.at - start_) * 4;
}

}  // namespace raw
