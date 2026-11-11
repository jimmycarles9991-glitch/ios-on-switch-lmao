// SPDX-License-Identifier: MIT
// The raw GM20B path (r7-raw, r8-rawca): a direct MAXWELL_B renderer for what Core Animation's render server draws, with no
// Vulkan, m2v or NVK at run time. Pipelines come from the pack (pack/rawpack.py: NAK's programs and NVK's register values per
// Core Animation state, read off this build's own m2v + NVK at bundle build time). This file turns Metal-shaped calls into
// MAXWELL_B / MAXWELL_DMA_COPY_A methods in a push buffer and submits them through the TegraGM20B user client (raw_kernel.h).
//
// Memory. Everything the raw path draws with is memory this task can also hand to m2v (RawMetal.mm's fallback makes m2v
// objects over the same BOs at the same VAs, never a copy or a second import: Alias):
//  - textures Core Animation makes, and buffers: pool spans, page-aligned anonymous memory imported once in big chunks;
//  - IOSurface textures: the surface's pages, imported once per surface while a texture over it lives;
//  - no-copy buffers: the caller's pages.
// So every raw texture is pitch-linear (as m2v renders Core Animation's surfaces in place), and CPU-cached: what the CPU wrote
// is cleaned to memory before the GPU reads it (CommandBuffer::clean, replaceRegion), what the GPU wrote is invalidated before
// the CPU reads it (RawMetal.mm, lazily). The raw path's own rings (push, upload, headers, root table) are write-combined.
//
// Lifetime. A texture, buffer or sampler is destroyed only once no submission that uses it can still run: RawMetal.mm's command
// buffers retain what they use until they complete, as Metal's do. Destroying it frees its header slot, pool span or import at
// once. The device's own memoryless backings and ring spans are retired by their last submission's fence.
//
// Threads. The Device's resource calls take its lock; one CommandBuffer encodes and commits at a time (RawMetal.mm serialises).
#pragma once
#include <array>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <map>
#include <tuple>
#include <vector>
#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>
#include "raw_comp.h"
#include "raw_kernel.h"
#include "raw_heap.h"
#include "raw_pack.h"
#include "raw_push.h"
#include "raw_overlap.h"
#include "raw_region.h"
#include "raw_sampler.h"
#include "raw_zeta.h"
#include "raw_xr.h"
#include "raw_ycbcr.h"
#include "../fx/fx_mip.h"

namespace raw {

class Device;
class CommandBuffer;
class ComputeEncoder;
struct ComputeState;
struct ComputeRecord;

constexpr uint32_t kPteKindPitch = 0;

struct Lease;  // raw_alias.cpp

// Where memory comes from: the pool, the write-combined GPU pool, an import, the block-linear chunks (kind 0xFE), an MTLHeap's
// span (raw_arena.cpp)
enum class Memory : uint8_t {
    Pool,
    Wc,
    Import,
    Bl,
    Arena
};

// Zeta memory another driver keeps (NVK's image, NativeBridgeMetalFrontend.h switch_nvk_image_memory): its GPU address and
// size, level 0's row stride, its texel size, block height and depth (log2 GOBs), its array stride; tiled and compressed as
// its binding maps it
struct ZetaMemory {
    uint64_t va = 0, bytes = 0, layerBytes = 0;
    uint32_t pitch = 0, bpp = 0;
    uint8_t blockHeight = 0, blockDepth = 0;
    bool tiled = false, compressed = false;
};

// A blit's aspect of a depth/stencil texture, numbered as the MTLBlitOption that names it (RawMetalBlit.mm records the option):
// DepthFromDepthStencil (1) or StencilFromDepthStencil (2), else the whole texel (0)
enum : uint8_t {
    kAspectWhole = 0,
    kAspectDepth = 1,
    kAspectStencil = 2
};

// Memory of the raw path an m2v view uses in place (RawMetal.mm's twins), as NVK takes it (switch_nvk_import_foreign): the BO
// it lies in, bound already in the GPU VM the two drivers share (raw_kernel.h), and a lease the view holds (Device::holdLease,
// dropLease, any thread) so the memory outlives the view's last use. One hold of it is the caller's to drop.
struct Alias {
    const uint8_t *cpu = nullptr;  // the BO's first byte for the CPU
    uint64_t bytes = 0, handle = 0, va = 0;
    uint32_t kind = 0;  // the PTE kind of its binding
    Lease *lease = nullptr;
};

// MTLPixelFormat -> NV formats: the texture header's word 0 NVK writes (read off the twin: pack/mkformats.py), its render target
// format when the raw path may render to it (0: sampled only), bytes per pixel, sRGB.
struct FormatInfo {
    uint32_t mtl, rt, tic0;
    uint8_t bpp;
    bool srgb;
    uint8_t blockW = 1, blockH = 1;  // a block-compressed format's texels a block, whose bytes bpp is
};

inline bool blockCompressed(const FormatInfo *f)
{
    return f && (f->blockW > 1 || f->blockH > 1);
}

const FormatInfo *formatInfo(uint32_t mtlPixelFormat);

// A texture of this size and format the raw path can lay out: `t` reset to it, with no memory yet
bool shape(uint32_t width, uint32_t height, uint32_t format, Texture &t);

constexpr uint64_t kBlAlign = 16384;  // NVK's image alignment here (r12-cafx niloracle), a multiple of NIL's

// An import of this task's pages into the GPU VM, shared by everything over them.
struct Import {
    Bo bo;
    uint32_t refs = 0;
    uint64_t id = 0, base = 0, bytes = 0;  // surfaces: IOSurfaceID, base address, allocation size (the cache key)
    const void *owner = nullptr;           // a retained IOSurfaceRef: its pages stay the surface's while imported
    // The PTE kind it is bound with: TGM_LAYOUT_KIND when the kernel decided the surface block-linear (SurfaceLayout), at
    // `layout`: its every texture is block-linear, as every GPU client, the display and a CPU lock see it
    uint32_t kind = 0;
    tgm_layout layout = {};
};

// The first texture's shape a new import asks the kernel the surface's layout with (raw_kernel.h surfaceLayout)
struct LayoutAsk {
    uint32_t width = 0, height = 0, bpp = 0, bytesPerRow = 0;
    bool writes = false;
};

// A page-aligned span of a pool chunk.
struct Span {
    uint32_t chunk = ~0u;
    uint64_t offset = 0, bytes = 0, va = 0;
    uint8_t *cpu = nullptr;

    bool valid() const
    {
        return chunk != ~0u;
    }
};

struct Texture {
    uint64_t va = 0;
    uint8_t *cpu = nullptr;  // the CPU's view of va (null: memoryless)
    uint32_t width = 0, height = 0, format = 0, pitch = 0;
    uint8_t bpp = 0;
    bool renderable = false;  // a pitch render target as it is: a target format, address and pitch multiples of 128
    // A target format, but an address or pitch the render hardware does not take (NVK's linear-render rule, linearTarget): a pass
    // renders it, block-linear, through a pool image the copy engine fills from it before and copies back to it after (NVK's
    // own tiled shadow, nvk_linear_render_copy, for the same targets: the texels stored are the same)
    bool shadowTarget = false;
    bool memoryless = false;  // no memory: a pass gives it a backing (Device::backing)
    bool wc =
        false;  // its span is write-combined GPU-pool memory (createTexture wc): CPU writes need no clean (Buffer::wc)
    uint32_t tic = 0;          // texture header index, 0: none (not sampleable)
    Import *import = nullptr;  // surface or no-copy pages, else
    Span span;                 // pool memory, else a view (memory of the texture it views)
    Lease *lease = nullptr;    // span's, once an m2v view aliases it (Device::alias): span is freed at its last drop

    // A two-plane 4:4:4 surface's texture (RGB8_444_2P, Metal format 542): the image sampled is span (RGBA8, the codes as Metal
    // reads them: red Cr, green Y, blue Cb, alpha 1), which BlitEncoder::convertPlanar writes from the surface's planes (import)
    // before the work that samples it
    struct Planes {
        uint64_t luma = 0, chroma = 0;  // GPU addresses of plane 0 (a Y code a texel) and plane 1 (Cb, Cr a texel)
        uint32_t lumaPitch = 0, chromaPitch = 0;
    } planes;

    bool planar = false;
    // A planar texture rendered into: its passes render into planarStandIn (RGBA16Float, block-linear), which a draw loads from
    // the planes before a pass that loads it and stores into them after one that stores it, through luma (R8Unorm over plane 0)
    // and chroma (RG8Unorm over plane 1): the frontend's two-plane stand-in (TwoPlaneStandIn.metal), as m2v renders it. Never
    // sampled: Metal reads the texture's codes (span)
    Texture *planarStandIn = nullptr;
    // A two-plane YCbCr surface's texture (raw_ycbcr.h): the image sampled is span (RGBA16Float, RGBA32Float, RGBA8Unorm,
    // RGB10A2Unorm), which a pass of the pack's conversion state draws from the planes' textures, luma (R8Unorm, R16Unorm) and
    // chroma (RG8Unorm, RG16Unorm; none over plane 1), before the work that samples it (pack/ycbcr.metal; shape: its fragment
    // buffer 0)
    Texture *luma = nullptr, *chroma = nullptr;
    uint32_t shape[kYcbcrShapeWords] = {};
    uint8_t ycbcr = 0;  // YcbcrOutput, 0: none
    // m2v's address map of a planar texture's samplers (metal2vulkan texture/ycbcr_image.rs `ycbcr_modes`): 1 the wrapping modes
    // (repeat, mirrored repeat, mirror-clamp) clamp to the edge (4:2:0 converted), 2 clamp to zero and to the border clamp to the
    // edge (4:4:4 codes), 3 both (4:2:0 codes)
    uint8_t addressMap = 0;

    // Its image is converted from a surface's planes: Metal's copies of it move the planes' bytes
    bool converted() const
    {
        return planar || ycbcr;
    }

    // Rows 16 but not 32 bytes apart (or an address off 32): the texture header's pitch takes multiples of 32, so it is sampled
    // through `sampled`, a block-linear pool image of its texels the copy engine refreshes from it before work that samples
    // it, as m2v samples such a surface through a shadow image it copies in. tic is sampled's.
    Texture *sampled = nullptr;

    // An extended-range texture's (raw_xr.h): the image a draw samples, its codes' values as RGBA16Float, which a pass of the
    // pack's conversion state draws from the codes (tic) before the work that samples it. Null: not sampled.
    Texture *standIn = nullptr;
    // Metal keeps it losslessly compressed (m2v nan_clear.rs keeps_nan_sign): an extended-range Clear in a pass that records
    // no draw call stores Metal's straight route's codes
    bool compressed = false;
    // Its memory is bound with a compressible kind (raw_comp.h): its passes enable colour, or Z and stencil, compression
    bool comptags = false;

    // A mipmapped texture's levels (r12, raw/fx): `chain` is the block-linear image of every level, NIL's layout, and this texture
    // is its level 0 (va, span, pitch and block height level 0's; its header, tic, the chain's). The 2D engine fills the later
    // levels from level 0 in place (BlitEncoder::generate); m2v's twin is a texture of every level over the same memory, so
    // whatever either driver writes into a level is the level.
    struct Chain {
        fx::MipChain layout;
        uint64_t va = 0;
        uint32_t fmt2d = 0;  // the 2D engine's surface format (fx::twodFormat); 0: it does not filter this one
        uint32_t levelTic[fx::kMaxLevels] = {};  // a view of each level alone (Device::levelTic), made on first use
    } *chain = nullptr;

    // Block-linear (TegraGM20BLayout.h): a chain level as a render target (chainLevelTarget: no memory or header of its own,
    // tic 0), or a texture laid out so (a tiled surface's, a memoryless attachment's backing in a block-linear pass): pitch
    // its row stride, `blockHeight` its block height (log2 GOBs), `layerBytes` the chain's or the texture's size
    bool blockLinear = false;
    uint8_t blockHeight = 0;
    uint64_t layerBytes = 0;
    // A block-linear texture's header type (fx::kTexOneD, kTexTwoD, kTexThreeD); a pitch header is always TWO_D_NO_MIPMAP
    uint8_t type = fx::kTexTwoD;
    // A 3D texture's (fx_image.h): its depth and its blocks' depth in planes, log2 (0 for one made 2D-view compatible)
    uint32_t depth = 1;
    uint8_t blockDepth = 0;
    // An array's or a cube's layers (six a cube), layerBytes apart
    uint32_t layers = 1;
    // A multisampled texture's samples a pixel (fx::msLayout): its memory the sample grid, height and width in pixels
    uint8_t samples = 1;

    // A depth/stencil format (raw_zeta.h kZetaFormatInfo): block-linear zeta memory, bound with its format's PTE kind; never a
    // colour target, sampled (tic) only where it was made to be. A stored one (not memoryless) is NVK's image, rendered,
    // sampled and copied in place, by aspect
    // (BlitEncoder::copyBufferTexels): a Depth32Float_Stencil8 one's stencil through stencilTemp, two bytes a texel (NVK's
    // stencil_copy_temp)
    bool zeta = false;
    Texture *stencilTemp = nullptr;

    // Device::createView's: a header of its own over its parent's memory. Whatever else it was copied from the parent with
    // (planes, stand-ins, images) stays the parent's, which outlives it: destroyTexture frees its header alone.
    bool view = false;
    bool arena = false;  // its span is an MTLHeap's (Device::arenaTexture): span.chunk names the arena
    // A block-compressed format's (as NIL lays one out): width, height and pitch count its blocks of blockW x blockH texels,
    // texelWidth x texelHeight its texels, the header's size. Metal's copies address texels (toElements).
    uint8_t blockW = 1, blockH = 1;
    uint32_t texelWidth = 0, texelHeight = 0;
    // Its channels' MTLTextureSwizzle (red, green, blue, alpha), which its header's X..W_SOURCE take (Device::swizzle); a view
    // starts from the identity, whatever its parent's
    uint8_t swizzle[4] = {2, 3, 4, 5};
};

constexpr uint8_t kIdentitySwizzle[4] = {2, 3, 4, 5};

// A copy's texel origin and size in `t` as its elements (a block-compressed texture's blocks; Metal's origins are whole blocks,
// a size reaching its edge may end inside one); false where the origin is not a block's
inline bool toElements(const Texture &t, uint32_t &x, uint32_t &y, uint32_t &w, uint32_t &h)
{
    if (x % t.blockW || y % t.blockH)
        return false;
    x /= t.blockW;
    y /= t.blockH;
    w = (w + t.blockW - 1) / t.blockW;
    h = (h + t.blockH - 1) / t.blockH;
    return true;
}

// Texture header word 0 with its sources read through `channels` (MTLTextureSwizzle each: Zero, One, Red, Green, Blue, Alpha);
// One is the integer or float one the format's own sources use
uint32_t swizzledWord0(uint32_t word0, const uint8_t channels[4]);

// The bytes of each texel a blit between `t` and a buffer moves in the buffer, as Metal's: a Depth32Float_Stencil8 aspect's (its
// depth 4, its stencil 1), RGB10A8_2P_XR10's color word (raw_xr.h), else the texel's
inline uint32_t bufferUnit(const Texture &t, uint8_t aspect)
{
    if (t.zeta && t.format == kZetaFormat && aspect != kAspectWhole)
        return aspect == kAspectStencil ? 1 : 4;
    return bufferTexelBytes(t.format, t.bpp);
}

struct Buffer {
    uint64_t va = 0, length = 0;
    uint8_t *cpu = nullptr;
    // Write-combined (Device::createBuffer wc): the CPU's view is the GPU pool's write-combined mapping, so what the CPU wrote is
    // in memory once a DSB drains it (CommandBuffer::commit's, the replay's) and nothing is cleaned for the GPU, nor invalidated
    // after its writes; the CPU reading it is slow (uncached), which Core Animation's buffers (written, then drawn) never do.
    bool wc = false;
    Import *import = nullptr;  // a caller's no-copy pages, else
    Span span;                 // pool memory
    Lease *lease = nullptr;    // as Texture::lease
    bool arena = false;        // as Texture::arena
};

// Level `level` (1 .. levels - 1) of mipmapped texture `t`'s chain as a render target (raw_target.h: NVK's block-linear
// target of that level), for a pass to render into: `out` names the chain's memory, it owns nothing. False: not a level of it.
bool chainLevelTarget(const Texture &t, uint32_t level, Texture &out);
// The same level as the copy engine's block-linear side, for a format the 3D engine does not render too
bool chainLevelImage(const Texture &t, uint32_t level, Texture &out);
// The sampler header NVK writes for the Vulkan sampler m2v makes of `d` (Mesa nvk_sampler_get_header; checked against the twin for
// Core Animation's combinations: pack/mksamplers.py, test/samplercheck.cpp)
void samplerHeader(const SamplerDesc &d, uint32_t w[8]);

struct Sampler {
    uint32_t tsc = 0;
    SamplerDesc desc;
    // its headers under a planar texture's address map (Texture::addressMap 1 to 3): m2v samples those textures through the
    // sampler with the map applied (Device::samplerVariant makes them; the same header where the map changes nothing)
    uint32_t mapped[4] = {};
};

struct SplitState;
struct SplitSource;

struct Pipeline {
    const PackPipeline *rec = nullptr;
    std::vector<uint32_t> methods;         // the state's registers as ready push words (program offsets relocated)
    uint32_t push[32] = {}, pushMask = 0;  // root table [0x700, 0x780): the words m2v's push ranges write, and which
    uint8_t setSlot[2] = {};  // the constant buffer slot the vertex and the pixel program read set 0 at (0: none)

    // the uniform buffers of set 0 its programs read as constant buffers: the group (0 vertex, 4 pixel), the slot and the set's
    // entry that names the buffer; false where a program reads one no entry names (its draws are refused)
    struct UboCbuf {
        uint8_t group, slot, entry;
    };

    UboCbuf ubos[2 * (sizeof(PackProgram::cbufType) - 1)] = {};  // constant buffers 1.. of each program
    uint8_t uboCount = 0;
    bool ubosNamed = true;
    uint8_t writeBits = 0;  // colors its draws write: SET_CT_WRITE(i) with a channel on
    // colors its fragment shader reads, and may write, as storage images inside the pixel interlock (metal2vulkan's storage
    // form of a fetch: the state turns the interlock on with SET_PIXEL_SHADER_INTERLOCK_CONTROL); the ROP writes none of them
    uint8_t storageBits = 0;
    bool constantBlend = false;     // a blend factor of a blended color is the encoder's blend color
    uint32_t colorFormats[8] = {};  // MTLPixelFormat of each color attachment the state was made for (0: none)
    // its vertex function is Core Animation's fixed_vert_l?h_spc (raw_bounds.h), and attribute 0 (the position) as its key
    // gives it: floats from the stream's vertex start (0: no float position)
    bool caTransform = false;
    uint8_t positionComponents = 0;
    uint16_t positionOffset = 0;
    // its draws may leave a pixel of their triangles unwritten: the fragment program discards (SPH KILLS_PIXELS) or writes
    // the sample mask, or the state tests depth, stencil or alpha to coverage, or rasterizes more than one sample
    bool dropsPixels = true;
    uint8_t samples = 1;  // its key's rasterSampleCount: a pass of attachments of that many samples draws it
    // SET_HYBRID_ANTI_ALIAS_CONTROL's passes: more than one where it shades per sample, whose sample masks differ (raw_samples.h)
    uint8_t samplePasses = 1;
    uint8_t readBits = 0;   // colors its draws read as they were: blended into, or written through a partial mask
    uint8_t fetchBits = 0;  // colors it reads by framebuffer fetch
    uint32_t depthFormat =
        0;  // its key's depth attachment format, else its stencil one (ds=<format>/<format>; 0: none)
    // push words setting back the registers outside the pack's varying ones its methods set (a state the runtime made:
    // raw_record.h extras), to their value in every pack state: emitted before the next state's methods
    std::vector<uint32_t> restore;
    // where its fragment program is split (raw_split.cpp): what its twin is made of, and the twin, made at the first draw that
    // takes it (both read without the device's lock); on that twin, its split
    const SplitSource *splitSource = nullptr;
    SplitState *split = nullptr;
    const SplitState *splitOf = nullptr;
};

// A state's fragment program split (raw_pack.h PackSplit): the twin that draws of rec.thresholdPixels pixels or more take, and
// the prologue placed in the heap
struct SplitState {
    PackSplit rec = {};
    Pipeline pipeline;
    uint32_t prologue = 0;  // QMD PROGRAM_OFFSET
    uint64_t prologueData = 0;
    uint32_t prologueDataBytes = 0;
};

// A split draw's prologue: the constant buffers its launch binds (PackSplit)
struct SplitLaunch {
    const SplitState *split = nullptr;
    uint64_t cbuf[8] = {};
    uint32_t cbufBytes[8] = {};
};

// A program in the heap: its record, its SET_PIPELINE_PROGRAM value, its shader data's GPU address (0: none) and its header
struct PlacedProgram {
    const PackProgram *record = nullptr;
    uint32_t program = 0;
    uint64_t data = 0;
    const uint8_t *header = nullptr;  // its shader header, as the CPU holds it
};

// What a state's split twin is made of: the pack's split (its programs by index) or a runtime record's programs, and the
// state's own inputs to Device::build
struct SplitSource {
    PackSplit rec = {};
    bool packed = false;
    // a runtime record's programs as heap slots: the per-pixel one laid out as the pack lays one (its PackProgram at heap
    // offset 0), the prologue's code with its shader data at prologueDataAt
    std::vector<uint8_t> fsSlot, prologueSlot;
    PackProgram fsInfo = {};
    uint32_t prologueDataAt = 0, prologueDataBytes = 0;
    const PackPipeline *pp = nullptr;
    const uint32_t *blob = nullptr;
    const char *keyString = nullptr;
    PlacedProgram vs;
    bool caTransform = false;
};

struct RuntimeRecord;
struct CapturedProgram;

// A render pass's attachments (Core Animation: color 0 the target, color 1 a memoryless attachment, writeMask 0 or 15)
struct ColorAttachment {
    Texture *texture = nullptr;

    enum Load : uint8_t {
        DontCare,
        Load,
        Clear
    } load = DontCare;

    bool store = true;  // Store or StoreAndMultisampleResolve
    float clear[4] = {};
};

// The pass's depth/stencil attachment: one zeta texture as each aspect its format has (memoryless: neither loaded nor stored)
struct DepthStencilAttachment {
    Texture *texture = nullptr;
    bool clearDepth = false, clearStencil = false;  // the aspect's load is Clear (else DontCare)
    float depth = 1;
    uint32_t stencil = 0;
};

struct PassDesc {
    ColorAttachment color[8];
    uint32_t count = 0;
    DepthStencilAttachment depthStencil;
    // renderTargetWidth / renderTargetHeight: the render area's bounds where nonzero (m2v render_pass.rs `area`: the
    // attachments' smallest extent, each axis limited by its target size)
    uint32_t width = 0, height = 0;
};

class RenderEncoder {
public:
    void setPipeline(Pipeline *p)
    {
        pipeline_ = p;
    }

    void setBytes(int stage, uint32_t slot, const void *bytes, uint32_t length);  // stage 1 vertex, 2 fragment
    // the same for bytes already written at `va` (256-byte aligned, zero to the next 256 bytes; `cpu` the CPU's view)
    void setBytesAt(int stage, uint32_t slot, uint64_t va, uint32_t length, const uint8_t *cpu);
    void setBuffer(int stage, uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setBufferOffset(int stage, uint32_t slot, uint64_t offset);

    void setTexture(uint32_t slot, const Texture *t)
    {
        if (slot < 16)
            textures_[slot] = t;
    }

    // what the pass renders color `j` into (a memoryless attachment: its backing) and its render area, kept after end()
    const Texture *target(uint32_t j) const
    {
        return j < 8 ? pass_.color[j].texture : nullptr;
    }

    uint32_t areaWidth() const
    {
        return areaW_;
    }

    uint32_t areaHeight() const
    {
        return areaH_;
    }

    void setSampler(uint32_t slot, const Sampler *s)
    {
        if (slot < 16)
            samplers_[slot] = s;
    }

    void setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void setBlendColor(const float rgba[4]);
    void setDepthStencil(const DepthStencilDesc *d);  // null: Metal's default state
    void setStencilReference(uint32_t front, uint32_t back);

    // MTLCullMode, MTLWinding and MTLTriangleFillMode, written as NVK's rasterizer registers at the next draw
    void setCullMode(uint8_t mode)
    {
        raster_.cull = mode;
    }

    void setWinding(uint8_t winding)
    {
        raster_.winding = winding;
    }

    void setFillMode(uint8_t mode)
    {
        raster_.fill = mode;
    }

    // setDepthBias:slopeScale:clamp: of finite constant and slope factors (an infinite or NaN one m2v draws through the
    // viewport: refused before it gets here)
    void setDepthBias(float constant, float slope, float clamp);

    void setViewport(double x, double y, double w, double h, double znear, double zfar);
    void barrier();  // memoryBarrier...: everything before is written and visible to what follows
    // false: this draw cannot be drawn exactly as m2v would (*why says what); the command buffer goes to m2v instead
    bool draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
              const char **why);
    // drawIndexedPrimitives: `count` indices of `indexSize` bytes (2 or 4) at `offset` in `ib`, each `baseVertex` on
    bool drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib, uint64_t offset,
                     uint32_t instances, int32_t baseVertex, uint32_t baseInstance, const char **why);
    void end();

private:
    friend class CommandBuffer;

    struct Bound {
        uint64_t va = 0;
        uint32_t size = 0;  // bytes from va (setBytes: its length)
        const Buffer *buffer = nullptr;
        uint64_t offset = 0;
        const uint8_t *cpu = nullptr;  // the bytes at va, where the CPU maps them
    };

    CommandBuffer *cb_ = nullptr;
    Device *dev_ = nullptr;
    PassDesc pass_;
    Push p_;
    const Pipeline *extrasAtBegin_ = nullptr;  // set back by the pass's prologue, before its clears
    uint32_t *bodyStart_ = nullptr;
    std::vector<std::pair<uint32_t *, uint32_t *>> bodies_;  // the pass body's push segments, in order
    void reserve();
    Pipeline *pipeline_ = nullptr, *emitted_ = nullptr;
    bool residualsPushed_ = false;  // the last draw's push words held SlotBindOffset residuals (RenderEncoder::prepare)
    Bound bufs_[3][31];
    const Texture *textures_[16] = {};
    const Sampler *samplers_[16] = {};
    uint64_t vbVa_ = 0, vbLimit_ = 0, ibVa_ = 0, ibEnd_ = 0;
    uint32_t ibSize_ = 0;
    // the state a draw needs, up to its draw parameters (vertices [vFirst, vFirst + vCount) of the stream are CPU-cleaned):
    // the first vertex a draw of vertices starts at, or the base vertex an indexed one adds to each index
    bool prepare(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t vFirst, uint32_t vCount,
                 uint32_t baseInstance, bool indexed, const char **why);
    // `pl`'s split twin where the draw is large enough for it to save time and its prologue can read what the draw binds
    Pipeline *splitTwin(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t count, const PixelBox &limit) const;
    // the twin's draw: its frontier bound and its prologue's launch recorded (`set`: the draw's set 0)
    void launchSplit(Push &p, const Pipeline &twin, const uint32_t *set);
    uint32_t scissor_[2] = {}, viewport_[8] = {};
    bool scissorDirty_ = true, viewportDirty_ = true;
    DepthStencilDesc ds_;
    uint32_t stencilRef_[2] = {};
    bool dsDirty_ = true;

    // the encoder's culling, winding and fill, and what the registers hold: the context's (Metal's defaults: no culling,
    // clockwise, fill) at a pass's start, put back at its end
    struct Raster {
        uint8_t cull = 0, winding = 0, fill = 0;
        bool bias = true;                                         // polygon offset on: a triangle topology's
        uint32_t biasConstant = 0, biasSlope = 0, biasClamp = 0;  // float bits
    };

    float depthBias_[3] = {};

    Raster raster_, rasterHeld_;
    void writeRaster(Push &p, const Raster &want);
    Raster rasterFor(uint32_t primitive) const;
    uint32_t blendColor_[4] = {};  // Metal's default, (0, 0, 0, 0), until the encoder sets one
    bool blendColorDirty_ = true;
    uint32_t topology_ = ~0u, restart_ = ~0u, first_ = 0, baseInstance_ = 0, baseVertex_ = 0;
    uint8_t writtenBits_ = 0;         // colors the ROP wrote since the last barrier
    uint8_t storageWrittenBits_ = 0;  // colors a fragment shader may have written as storage since the last barrier
    bool memoryless_[8] = {};
    const Texture *shadowOf_[8] =
        {};  // a shadow target's texture, where pass_ holds its pool image (CommandBuffer::render)
    // what m2v's DontCare loads answer (metal2vulkan clear_reach.rs: dont_care_loads, DrawnArea): whether the pass has a draw
    // call and one that draws, the colors its draws read, and the pixels they may write (scissor within viewport, each draw)
    bool called_ = false, drew_ = false, whole_ = false;
    uint32_t readBits_ = 0, areaW_ = 0, areaH_ = 0;
    uint8_t samples_ = 1;  // its attachments' samples a pixel
    double vp_[4] = {};
    uint32_t sc_[4] = {};
    std::vector<std::array<uint32_t, 4>> rects_;
    // the draws that wrote a color since the last barrier, for the fetch barrier's region test (raw_region.h); the
    // indices of the indexed draw being prepared
    WrittenRegion written_;
    // colors the pass's first draw overwrites whole and opaquely: their clears inside the render area are not needed
    uint8_t covered_ = 0;
    using Runs = std::vector<std::pair<uint32_t, uint32_t>>;
    const Runs &selfRuns(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count, uint32_t instances);
    const uint8_t *drawIndices_ = nullptr;
    uint32_t drawIndexSize_ = 0, drawIndexCount_ = 0;
    DrawGeometry geometry(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count,
                          const PixelBox &limit) const;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // the same draws under two other rules, counted only: one barrier per fetching draw after any color write (the rule
    // before writtenBits_), and the interlocked storage form (a fetched color read and written by the fragment shader inside
    // the pixel interlock, the rest through the ROP): it orders storage accesses among themselves, so it barriers only where
    // a storage read or write follows a ROP write of the color, or a ROP write follows a storage write
    bool anyWritten_ = false;
    uint8_t ropWritten_ = 0, storageWritten_ = 0;
    void countRules(const Pipeline &pl);
#endif
    void fetchBarrier(Push &p);

    struct Clear {
        uint32_t target, x0, y0, x1, y1;
        const float *value;
        bool own = false;  // cleared by a pass of its own before this one (a Clear load reaching past the render area)
    };

    std::vector<Clear> clears() const;
};

// Copy-engine work between passes (MAXWELL_DMA_COPY_A, as NVK's nouveau_copy_rect does it for pitch memory)
class BlitEncoder {
public:
    // `bytes` bytes from `src` to `dst` (vkCmdCopyBuffer)
    void copyBytes(uint64_t src, uint64_t dst, uint64_t bytes);
    // `bytes` bytes at `dst` set to `value` (fillBuffer:range:value:): the copy engine writing a constant, its whole words as
    // NVK's vkCmdFillBuffer (nvk_cmd_fill_memory_ce), the bytes before and after them one at a time
    void fillBytes(uint64_t dst, uint64_t bytes, uint8_t value);
    // `lines` rows of `width` pixels of `bpp` bytes, from `src` (row stride srcPitch) to `dst` (dstPitch) (vkCmdCopyImage and the
    // buffer<->image copies of linear images)
    void copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width, uint32_t lines,
                  uint32_t bpp);
    // `w` x `h` texels from (sx, sy) of plane sz of `src` to (dx, dy) of plane dz of `dst`, each pitch or block-linear (a 2D
    // texture's plane is 0) (vkCmdCopyImage)
    void copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                    uint32_t dy, uint32_t dz, uint32_t w, uint32_t h);
    // `w` x `h` texels between rows `pitch` bytes apart at `va` (both multiples of the texel's component, at most 4 bytes) and
    // (x, y) of plane z of `t`, pitch or block-linear: into `t` where `toTexture`, else out of it (vkCmdCopyBufferToImage,
    // vkCmdCopyImageToBuffer); the buffer's texels bufferUnit each. A Depth32Float_Stencil8 aspect's copy is NVK's
    // (nvk_remap_extract_aspect, insert_aspect): depth its texel's first word, stencil its fifth byte through t.stencilTemp
    void copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                          uint32_t h, bool toTexture, uint8_t aspect = kAspectWhole);
    // A planar texture's image from its surface's planes (Texture::planar): two remapped copies, the luma codes into green
    // (alpha 255), then the chroma pairs into red (Cr) and blue (Cb), each leaving the other components as they are
    void convertPlanar(const Texture &t);
    // generateMipmaps of a mipmapped texture (Texture::chain) on the 2D engine: each later level from the one above, in place
    void generate(Texture &t);
    // The next command reads the memory `read` names and writes the memory `write` names (null: none; a name is any pointer
    // that stands for one memory): where it reads or writes what an earlier command of this encoder wrote, or writes what one
    // read, it waits for those first (barrier), as Metal orders a blit encoder's commands on one resource
    void access(const void *read, const void *write);
    // Every copy and 2D blit before it completes before any after it starts: NVK's wait between transfers
    // (nvk_cmd_flush_wait_dep), as the copy engine starts a PIPELINED launch before the ones before it have landed
    void barrier();
    void end();

private:
    friend class CommandBuffer;
    CommandBuffer *cb_ = nullptr;
    Device *dev_ = nullptr;
    Push p_;
    uint32_t *start_ = nullptr;
    static constexpr uint8_t kAccesses = 16;
    const void *read_[kAccesses] = {}, *written_[kAccesses] = {};
    uint8_t reads_ = 0, writes_ = 0;
    void room(uint32_t words = 56);  // at least `words` words left in the reservation
};

// Which encode-time predicate refused a draw (set beside *why; RawMetal.mm counts and logs it, r10-rawsweep diagnostics)
enum RefusalId : uint8_t {
    RefNone,
    RefIndexedRange,
    RefColorFormats,
    RefUboOffset,
    RefNoTic,
    RefDepthStencil,  // a: the state's depth format, b: the pass's
    RefSamples        // a: the state's samples, b: the pass's, c: the state's shading passes
};

struct Refusal {
    uint8_t id = RefNone;
    uint32_t a = 0, b = 0, c = 0;
};

class CommandBuffer {
public:
    Refusal refusal;                              // the last refused draw's predicate and its values
    RenderEncoder *render(const PassDesc &pass);  // null: an attachment the raw path cannot render to
    BlitEncoder *blit();
    // CPU lines the GPU reads in this submission (CPU-written cached memory): cleaned to memory before it is submitted.
    // `invalidate`: also dropped from the CPU's caches (memory the GPU writes, so no dirty line lands on it later).
    void clean(const void *cpu, uint64_t bytes, bool invalidate = false);
    // One Exec (more when the segments exceed one Exec's 255): the submission prologue, then every pass and blit in order.
    // false with `partial`: the kernel refused a later Exec after earlier ones were submitted; those completed before it
    // returned, and the next command buffer gets a new channel (the GPU's state after them is not what the device tracks).
    bool commit(uint32_t &seqno);
    bool partial = false;
    // Nothing of it is on the GPU any more: the rings get back what it took, the root table's shadow what it was.
    void abandon();

    // A host import the submission reads or writes (a surface's, a no-copy buffer's), for the kernel's order with other
    // processes' GPU work on it (raw_kernel.h exec)
    void use(const Import *i, bool write)
    {
        if (i)
            surfaces_.add(i->bo.handle, write);
    }

    uint32_t draws = 0, passes = 0, blits = 0;
    uint32_t stamp = 0;  // its GPU time's slot (raw_stamps.h), for Device::gpuNanoseconds once it completed

private:
    friend class Device;
    friend class RenderEncoder;
    friend class BlitEncoder;
    friend class ComputeEncoder;
    Device *dev_ = nullptr;
    std::vector<TegraGM20BAbiPush> segments_;
    std::vector<std::pair<uintptr_t, uint64_t>> clean_, flush_;  // CPU ranges to clean / clean+invalidate before Exec
    std::vector<Texture *> backings_;                            // memoryless backings it uses
    SurfaceList surfaces_;
    RenderEncoder render_;
    BlitEncoder blit_;
    uint64_t pushStart_ = 0, uploadStart_ = 0;
    uint32_t rootPush_[32] = {};
    uint32_t restart_ = 0;                  // SET_DA_PRIMITIVE_RESTART_INDEX as the GPU will hold it
    DepthStencilRegs ds_;                   // and its depth/stencil registers
    const Pipeline *extrasLive_ = nullptr;  // and the state whose extras it holds (Pipeline::restore)
    bool drawn_ = false;  // a draw was encoded (NVK resets the restart index at a command buffer's first)
    // the prologue's INVALIDATE_SHADER_CACHES_NO_WFI data word, which commit gives the INSTRUCTION bit when programs were placed
    // in the heap since the last submission that invalidated the instruction cache
    uint32_t *shaderInvalidate_ = nullptr;
    uint32_t prologueDwords_ = 0;
    // the split draws' prologues (RenderEncoder::launchSplit), launched in one compute segment after the submission prologue
    std::vector<SplitLaunch> splits_;
    // GPU work encoded before may have written a buffer: a prologue, run ahead of it, reads only what the CPU wrote
    bool buffersWritten_ = false;
    void segment(uint32_t *from, uint32_t *to);
    void launchPrologues();
};

class Device {
public:
    // Whose device it is. The render server draws every frame of the screen from its start: its channel, shader heap and first
    // chunks are made at open. Any other process's are made at their first use, in smaller steps, and what stays empty goes
    // back to the kernel when it idles (trim): most never draw, and a widget's jetsam limit is tens of MB.
    enum class Role : uint8_t {
        RenderServer,
        Client
    };

    ~Device();
    bool open(const char *packPath, uint64_t stack, std::string &err, Role role = Role::RenderServer);

    Role role() const
    {
        return role_;
    }

    // The channel and its context, made now if not yet (a client's first command buffer, Order or mipmapped texture); with it
    // the 2D engine's mip-chain self-test. False: the kernel refused it (`channelError()`), and the device submits nothing
    // until a call kChannelRetryNs or more later gets one.
    bool ensureChannel();

    // Why the kernel last refused the channel or its context (ensureChannel, or begin() making a killed channel again)
    std::string channelError()
    {
        std::lock_guard<std::mutex> l(channelLock_);
        return channelError_;
    }

    const Pack &pack() const
    {
        return pack_;
    }

    // A pack pipeline by its state key's FNV-1a (RawMetal.mm rawPipelineKey); null: not in the pack.
    // caTransform: its vertex function is Core Animation's fixed_vert_l?h_spc (Pipeline::caTransform)
    Pipeline *pipeline(uint64_t keyHash, PackForm form = PackForm::Default, bool caTransform = false);
    // A state the runtime made a record of (raw_record.h), drawn from then on as the pack's are; null: its programs find no room
    // in the heap or its samplers none in the header pool (`why`)
    Pipeline *addRecord(const RuntimeRecord &r, bool caTransform, std::string &why);
    // A runtime record's split made after its state (RuntimeRecord::split): its draws may take it from then on
    void addSplit(const RuntimeRecord &r);

    // Resources (thread safe). All textures are 2D, one level, one sample, pitch-linear.
    // pool memory, rows 256-byte aligned; `wc`: write-combined GPU-pool memory (a texture the CPU writes with replaceRegion:
    // no clean after), else (or full) pool memory
    // `tiled`: block-linear (TegraGM20BLayout.h) where its shape tiles and its format is a colour target's, for a texture the
    // CPU never touches
    bool createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc = false,
                       bool tiled = false);

    // Before the GPU first renders or stores into the IOSurface (Kernel::surfaceGpuWrites).
    bool surfaceGpuWrites(uint64_t id)
    {
        return id && k_.surfaceGpuWrites(uint32_t(id));
    }

    bool gpuPriority(uint32_t priority, uint32_t background)
    {
        return k_.priority(priority, background);
    }

    bool hasGpuPriority() const
    {
        return (k_.info().flags & TegraGM20BInfoPriority) != 0;
    }

    const TegraGM20BAbiInfo &kernelInfo() const
    {
        return k_.info();
    }

    // `writes`: the texture is rendered into or stored to. A surface the kernel decides block-linear (the first texture over
    // it writes it: TegraGM20BLayout.h) has block-linear textures only, at the decided shape (false: another one)
    bool createSurfaceTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, uint64_t offset,
                              uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, Texture &t,
                              bool writes = false);
    // A two-plane YCbCr surface's texture of `width` x `height` as `plan` reads its planes, its image one of the pool
    bool createYcbcrTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                            const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t);
    // The pack's conversion state of a YCbCr texture's output (Texture::ycbcr), null if the pack has none
    Pipeline *ycbcrPipeline(uint8_t output);
    // An extended-range texture's stand-in (Texture::standIn), made if the pack has its conversion state
    bool createStandIn(Texture &t);
    // The pack's conversion state of an extended-range texture's stand-in, null if the pack has none
    Pipeline *xrPipeline(XrForm form);
    // The pack's resolve state into a target of `format` (raw_samples.h resolveState), null if the pack has none
    Pipeline *resolvePipeline(uint32_t format);
    // The pack's state that draws a chain level of `format` from the level above (raw_mip.h mipState), null if the pack has none
    Pipeline *mipPipeline(uint32_t format);
    // s's header under address map `map` (Texture::addressMap), made now if it is not yet
    bool samplerVariant(Sampler &s, uint8_t map);
    // A two-plane 4:4:4 surface's texture of RGB8_444_2P (542): its planes at `luma`/`chroma` (offsets in the allocation,
    // with their pitches), sampled through an RGBA8 image of the pool (Texture::planar)
    bool createPlanarTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, uint64_t luma,
                             uint32_t lumaPitch, uint64_t chroma, uint32_t chromaPitch, uint32_t width, uint32_t height,
                             Texture &t);
    // A planar texture's stand-in and its planes' render targets (Texture::planarStandIn); false where the pack lacks the
    // stand-in
    // states or a plane cannot be rendered
    bool createPlanarTarget(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, Texture &t);
    // The pack's stand-in states: the planes' codes into the stand-in, or (`store`) the stand-in into the planes
    Pipeline *standInPipeline(bool store);
    bool createBufferTexture(const Buffer &b, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t format,
                             Texture &t);  // over a buffer's memory (t holds no reference: the caller keeps the buffer)
    bool createView(const Texture &of, uint32_t format, Texture &t);  // the same memory as another format
    // Its header's sources read through `channels` (MTLTextureSwizzle each, Texture::swizzle), its header written as it is
    void swizzle(Texture &t, const uint8_t channels[4]);
    // A view of part of a block-linear 2D texture, chain, array or cube: levels [baseLevel, +levels) of a chain, or layers
    // [baseLayer, +layers) of an array or cube, as a view of `type` (fx::kTexTwoD, kTexTwoDArray, kTexCube, kTexCubeArray). Its
    // image is the part's first level and layer, which copies address and a pass renders into as a 2D view of one of each.
    bool createPartView(const Texture &of, uint32_t format, uint32_t type, uint32_t baseLevel, uint32_t levels,
                        uint32_t baseLayer, uint32_t layers, Texture &t);
    // Memoryless (a pass gives it a backing); the depth/stencil formats among them, as zeta (Texture::zeta)
    bool createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t, uint32_t samples = 1);
    // A stored depth/stencil texture of one sample over zeta memory another driver of the process keeps (NVK's image: one
    // allocation both drivers render, sample and copy in place), laid out as `m` says, with a texture header of its depth where
    // `sampled`; false where it is not a layout the raw path renders (*why says which way)
    bool createZetaOver(const ZetaMemory &m, uint32_t width, uint32_t height, uint32_t format, bool sampled, Texture &t,
                        const char **why);
    // A Depth32Float_Stencil8 texture's stencil temp (Texture::stencilTemp), made on its first stencil copy
    bool stencilTemp(Texture &t);
    // A 1D texture: NIL's 1D image, one block-linear row (y_log2 0) under a ONE_D header, in block-linear memory the CPU
    // writes through its tiled view (a 1D header takes no pitch layout)
    bool createLine(uint32_t width, uint32_t format, Texture &t);
    // A 3D texture of one level: NIL's 3D image (fx_image.h) under a THREE_D header. `view2d`: one plane a block, as m2v makes
    // a 3D render target (2D-array compatible), so its twin over the same memory reads the same texels
    bool createVolume(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, bool view2d, Texture &t);
    // A 2D array, cube or cube array of one level: NIL's image of `layers` layers (fx_image.h) under a header of `type`
    // (fx::kTexTwoDArray, kTexCube, kTexCubeArray)
    bool createArray(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t type, Texture &t);
    // A mipmapped 2D texture of `levels` levels: level 0 of its block-linear chain (Texture::chain), its header the chain's.
    // False: not one the raw path keeps (not a render target format, extended range, no memory).
    bool createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t);
    // A block-compressed 2D texture (or 2D array) over the caller's `bytes` (newTextureWithBytesNoCopy), already laid out as
    // `layout` (fx::blockChainLayout of its texel size, levels and layers): the pages imported under kind 0xFE, cleaned to
    // memory, and a block-linear header over them, of the texel size. False: not a block-compressed format, `length` short
    // of the layout, a base off 512 bytes, or the import or header refused.
    bool createBlockTextureOver(void *bytes, uint64_t length, uint32_t width, uint32_t height, uint32_t format,
                                const fx::MipChain &layout, Texture &t);
    // The texture header of level `level` of mipmapped texture `t`'s chain alone, as NVK's view of that level (a pass into
    // the level fetches its color through it); 0: none
    uint32_t levelTic(Texture &t, uint32_t level);
    // A header of plain texture `t`'s memory whose word 0 is `word0` (raw_view.h: m2v's view of it in another format
    // of the same texel size), made on first use and freed with t's header; 0: no header slot left
    uint32_t viewTic(const Texture &t, uint32_t word0);

    // Whether the connection is NVK's (raw_kernel.h): m2v's views alias this path's memory
    bool shared() const
    {
        return k_.shared();
    }

    // The memory no resource holds goes back to the kernel: empty chunks of the pool, of images and of buffers, and the kept
    // dedicated ones (the kernel keeps their pages until the work submitted before has completed)
    void trim();

    // Whether the device's resource lock is free now (try, never waits: the stall line's)
    bool lockFree()
    {
        std::unique_lock<std::mutex> l(lock_, std::try_to_lock);
        return l.owns_lock();
    }

    void destroyTexture(Texture &t);
    // `wc`: from the write-combined GPU pool (Buffer::wc), else (or full) pool memory as before
    bool createBuffer(uint64_t length, Buffer &b, bool wc = false);

    // The write-combined chunks held (non-dedicated) and their bytes (test/poolcheck.cpp: they follow what is in them)
    uint32_t wcChunksHeld();
    uint64_t wcChunkBytesHeld();
    // The free bytes of the block-linear chunks (test/poolcheck.cpp: a chunk's worth kept ahead)
    uint64_t blChunkFreeBytes();

    // the shader heap's bound bytes (the programs placed so far, in steps)
    uint64_t heapBytes() const
    {
        return heapBound_;
    }

    bool createBufferNoCopy(void *bytes, uint64_t length, Buffer &b);
    void destroyBuffer(Buffer &b);

    // MTLHeaps (raw_arena.cpp): an arena is one GPU VA reservation bound whole to host pages of its own, under
    // TGM_LAYOUT_KIND for a private heap's block-linear textures or pitch for a shared heap's textures and buffers, its
    // resources placed in it (raw_heap.h). Purged, its pages go back and its VA and CPU range stay reserved, so restoring it
    // binds fresh pages where its resources' addresses and texture headers already point.
    bool createArena(uint64_t bytes, bool blockLinear, uint32_t &arena);
    void destroyArena(uint32_t arena);  // once its resources are gone; an m2v view's lease keeps it until its last drop
    bool arenaTexture(uint32_t arena, uint32_t width, uint32_t height, uint32_t format, Texture &t);
    bool arenaBuffer(uint32_t arena, uint64_t length, Buffer &b);
    // makeAliasable: the range goes back for later placements (at an m2v view's last drop, if one aliases it)
    void arenaRelease(Texture &t);
    void arenaRelease(Buffer &b);
    uint64_t arenaUsed(uint32_t arena);
    uint64_t arenaLargestFree(uint32_t arena, uint64_t align);
    // What an arena texture of this shape takes; false: it is placed nowhere (the heap makes it as the device does)
    static bool arenaFootprint(uint32_t width, uint32_t height, uint32_t format, bool blockLinear, uint64_t &bytes,
                               uint64_t &align);
    // Volatile arenas give their pages back at purgeArenas: once every submission completed, no m2v view holds them and
    // `committed` (the caller's command buffers committed and unfinished) is 0. A commit adds to it before it reads
    // purgedArenas() and the purge counts itself there before it reads `committed`, both sequentially consistent, so one of
    // them sees the other: the commit restores what it reaches, or the purge does not happen.
    void setArenaVolatile(uint32_t arena, bool isVolatile);
    void purgeArena(uint32_t arena, const std::atomic<uint32_t> &committed);
    void purgeArenas(const std::atomic<uint32_t> &committed);
    bool arenaPurged(uint32_t arena);
    // Fresh pages where it was (their contents zero); false: no memory, it stays purged
    bool restoreArena(uint32_t arena);

    uint64_t arenaBytes();  // bound: purged arenas' are not

    uint32_t purgedArenas() const
    {
        return purgedArenas_.load();
    }

    // The memory an m2v view of t (a pool or write-combined texture) or b uses in place, one hold taken for the caller (raw_alias.cpp).
    // False: nothing to alias (memoryless, a view's memory), or the connection is not NVK's.
    bool alias(Texture &t, Alias &a);
    bool alias(Buffer &b, Alias &a);
    // The surface's import, made now if no texture of the raw path holds one: an m2v import of the surface is this one, aliased.
    bool aliasSurface(uint64_t surfaceId, void *base, uint64_t bytes, const void *surface, Alias &a);
    static void holdLease(void *lease);
    static void dropLease(void *lease);
    bool createSampler(const SamplerDesc &d, Sampler &s);
    void destroySampler(Sampler &s);

    // The first pool chunk made now (its pages wired and imported), off the render thread's first texture (raw_metal_prepare),
    // and the pool's grower started: the next chunk of the pool, of block-linear images or of a zeta format is made on its own
    // thread once the free bytes of that kind's shared chunks fall below a chunk's worth, so the render thread's allocations
    // (a pass's memoryless backings among them, made at commit) find room (Stats::poolStalls counts the ones that had to grow
    // it).
    void warm();

    // Submission. begin() opens the one command buffer (the caller serialises encoding and commit); null: no channel.
    CommandBuffer *begin();

    uint32_t completed() const
    {
        return *fence_;
    }

    // 0 before the channel is made (it may be made on another thread: a client's first mipmapped texture)
    uint32_t submitted() const
    {
        return channelReady_.load(std::memory_order_acquire) ? lastSeqno_ : 0;
    }

    bool done(uint32_t seqno) const
    {
        return !seqno || int32_t(*fence_ - seqno) >= 0;
    }

    // In the kernel, on the fence interrupt. False: the submission failed, the channel died with it in flight (reopen).
    bool wait(uint32_t seqno)
    {
        return done(seqno) ? !lost(seqno) : waitFence(seqno);
    }

    // A completed command buffer's GPU time from its stamps; false when its slot holds a later one's
    bool gpuNanoseconds(uint32_t stamp, uint64_t &ns) const;

    bool lost(uint32_t seqno) const
    {
        if (k_.killRecords())
            return killed(seqno);
        return lostAny_.load(std::memory_order_acquire) &&
               int32_t(lostThrough_.load(std::memory_order_relaxed) - seqno) >= 0;
    }

    // A kill record's error the device writes itself (waitFence): the kernel kept refusing to wait on a channel it called
    // healthy
    static constexpr uint32_t kWaitRefused = 0x100;

    // Why the kernel killed the channel of the last seqno lost() named (its kill record's error), else Kernel::lostError and
    // lostFaultVa: the channel last found dead
    uint32_t lostError() const
    {
        const uint32_t recorded = killedError_.load(std::memory_order_relaxed);
        return recorded ? recorded : k_.lostError();
    }

    uint64_t lostFaultVa() const
    {
        return k_.lostFaultVa();
    }

    // Which of this device's memory holds GPU VA `va` (a killed command buffer's fault VA), and where in it, for its line
    std::string vaOwner(uint64_t va);

    // GPU-side order with the process's other driver (NVK's channels), through the kernel's Order (TegraGM20BInfoOrder; false:
    // it has none). orderAfterOthers: this channel's next submission starts with an acquire on what they submitted so far;
    // orderOthersAfter: their next submissions start with one on everything this channel submitted. The kern_return_t (0:
    // queued; else the caller orders on the CPU); `waits`: the acquires queued. Called where submissions are (serialised).
    bool canOrder() const
    {
        return k_.canOrder();
    }

    // A channel the kernel killed is made again first (between command buffers, as begin() would), so one GPU fault does not
    // leave the process ordered on the CPU for good
    int orderAfterOthers(uint32_t &waits)
    {
        if (!ensureChannel())
            return KERN_FAILURE;
        for (int attempt = 0;; attempt++) {
            if ((contextless_ || k_.lost(channel_)) && !reopen())
                return KERN_FAILURE;
            const int kr = k_.order(channel_, TegraGM20BOrderAfterTask, 0, waits);
            if (kr != kIOReturnNotResponding || attempt)
                return kr;
        }
    }

    // A killed channel's fence is released and nothing it had runs any more: there is nothing of it to order after
    int orderOthersAfter(uint32_t &waits)
    {
        waits = 0;
        if (!channelReady_.load(std::memory_order_acquire) || contextless_ || k_.lost(channel_))
            return KERN_SUCCESS;
        const int kr = k_.order(channel_, TegraGM20BOrderBeforeTask, lastSeqno_, waits);
        return kr == kIOReturnNotResponding ? KERN_SUCCESS : kr;
    }

    struct Stats {
        std::atomic<uint64_t> commits{0}, pushBytes{0}, uploadBytes{0}, ringWaits{0}, ringsOutgrown{0}, imports{0},
            importBytes{0}, poolBytes{0}, poolChunks{0}, cleanBytes{0}, poolStalls{0};
        // uncompressed backings held now and their memory (a memoryless one's shared plain memory, a shadow's own), and the
        // retired ones among them and the free bytes of the block-linear and zeta chunks as trimBackings last counted them
        std::atomic<uint64_t> backings{0}, backingBytes{0}, retiredBackings{0}, tiledFreeBytes{0};
        // surface imports made (a kernel import each) and found live, since open
        std::atomic<uint64_t> importsMade{0}, importHits{0};
        // diagnostics: color targets the raw path rendered whose address or pitch is not a multiple of 128 (NVK renders
        // those through a tiled shadow: nvk_cmd_draw.c nvk_image_plane_aligned_for_linear_attachment), and the last one's pitch
        std::atomic<uint64_t> rtUnaligned{0};
        // color targets in rows the raw path rendered through a block-linear shadow, beside a block-linear one in their pass
        std::atomic<uint64_t> rtMixed{0};
        std::atomic<uint32_t> rtUnalignedPitch{0}, rtUnalignedVa{0};
        // mip chains: made, chain bytes, generated
        std::atomic<uint64_t> chains{0}, chainBytes{0}, chainGenerates{0};
        // write-combined buffers: live bytes, and the buffers that asked for one and got pool memory (the WC pool full)
        std::atomic<uint64_t> wcBytes{0}, wcFallbacks{0};
        // the copy engine's work encoded: launches and the bytes they write (copies, remaps, the pass prologue's shadow copies)
        std::atomic<uint64_t> ceOps{0}, ceBytes{0};
        // passes into a chain level
        std::atomic<uint64_t> levelPasses{0};
        // pack programs placed in the shader heap, and states not made because it was full
        std::atomic<uint64_t> programsPlaced{0}, heapFull{0};
        // framebuffer-fetch barriers encoded, and those left out where the draw's pixels meet none a color write reached
        // since the last barrier (raw_region.h); the barriers one per fetching draw after any color write would have encoded,
        // and those the interlocked storage form would (RenderEncoder::countRules, debug builds)
        std::atomic<uint64_t> fetchBarriers{0}, fetchBarriersElided{0}, fetchBarriersAnyColor{0},
            fetchBarriersInterlocked{0};
        // Clear loads of colors, and those left out because the pass's first draw covers them (RenderEncoder::clears)
        std::atomic<uint64_t> clears{0}, clearsElided{0};
        // draws that fetch a color they write (debug builds): split into runs where their own triangles may cover a pixel
        // twice, and the runs; those of more than one triangle it could not tell apart (drawn whole, unordered within);
        // the CPU time telling them (raw_overlap.h)
        std::atomic<uint64_t> selfOverlapSplit{0}, selfOverlapRuns{0}, selfOverlapUnbounded{0}, selfOverlapNs{0};
        // channels made again after the kernel killed one
        std::atomic<uint64_t> reopens{0};
        // compressed memory held now (raw_comp.h CompMem and made buffers no backing took) and its bytes, the backings on it
        // now; buffers made and the kernel's refusals since the device opened; full-target clears of compressed targets to a
        // value the boot ZBC table holds (no texels written) and to one it does not
        std::atomic<uint64_t> compHeld{0}, compHeldBytes{0}, compViews{0}, compMade{0}, compFailed{0}, zbcHits{0},
            zbcMisses{0};
        // compute launches encoded (raw_compute.h)
        std::atomic<uint64_t> dispatches{0};
        std::atomic<uint64_t> splitPrologues{0};  // split draws' prologue launches (raw_split.cpp)
    } stats;

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // The fetch barrier's methods, set by the context-switch stress (test/fetchform.cpp) and the ceiling measurement
    // (RawMetal.mm's debug notifications): PIXEL_SHADER_BARRIER and 0x0f74 (the shipped form), WAIT_FOR_IDLE and
    // INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI (the form before it, the stress's reference), PIXEL_SHADER_BARRIER alone or with
    // the NO_WFI invalidate, which tell a hang's method apart, and none at all. PixelShaderOnly and None let a fetching
    // draw read colors its predecessors have not finished writing: measurements of what the barrier costs, never shipped
    enum class FetchBarrier : uint8_t {
        PixelShader,
        WaitForIdle,
        PixelShaderOnly,
        PixelShaderNoWfiInvalidate,
        None
    };

    std::atomic<FetchBarrier> fetchBarrier{FetchBarrier::PixelShader};
    static const char *fetchBarrierName(FetchBarrier b);
#endif

private:
    friend class CommandBuffer;
    friend class RenderEncoder;
    friend class BlitEncoder;
    friend class ComputeEncoder;
    friend const ComputeState *placeCompute(Device &dev, const ComputeRecord &r);
    friend const ComputeState *computeState(Device &dev, uint64_t keyHash);

    struct Ring {
        Bo bo;
        uint64_t head = 0, since = 0;  // next free byte; where the open command buffer's use began
        // No span in flight lies in [head, clear): an allocation there needs no look at them (alloc's scan sets it)
        uint64_t clear = 0;
        bool waited = false;  // an allocation waited for the GPU since the last begin(): it doubles there

        struct Span {
            uint64_t begin, end;
            uint32_t seqno;  // 0: the open command buffer's
        };

        std::deque<Span> inflight;
        std::vector<Bo>
            outgrown;  // what the open command buffer grew out of (Device::alloc): freed once its work completes
    };

    struct Chunk {
        Bo bo;
        void *memory = nullptr;
        uint64_t bytes = 0;
        std::map<uint64_t, uint64_t> free;  // offset -> bytes, coalesced
        uint64_t used = 0;
        bool dedicated = false;
    };

    // A role's memory steps: the shared pool chunk (a span over half of one gets its own), a chunk of block-linear images, a
    // chunk of write-combined buffers, and the rings' first sizes (each doubles where an allocation waited, to the render
    // server's). A client's chunks of a kind start smaller and grow with what it holds (chunkSize).
    struct Sizes {
        uint64_t chunk, blChunk, wcChunk, push, upload;
    };

    uint64_t chunkSize(const std::vector<Chunk> &chunks, uint64_t cap, uint64_t need, uint64_t align) const;

    static const Sizes kSizes[2];
    Role role_ = Role::RenderServer;
    Sizes sizes_ = kSizes[0];
    std::mutex lock_;
    Kernel k_;
    Pack pack_;
    // heap_: the shader heap's GPU VA reservation (bytes: the program window), bound from its start (heapBound_) in parts of
    // host memory as programs are placed
    Bo heap_, tic_, tsc_, root_, zero_, tickets_, colorTable_, stamps_;

    struct HeapPart {
        uint64_t offset = 0, bytes = 0;  // in the heap
        uint8_t *cpu = nullptr;
        Bo bo;
    };

    std::vector<HeapPart> heapParts_;
    uint64_t heapBound_ = 0;
    Ring push_, upload_;
    // rings replaced by larger ones, freed once the GPU passed the seqno
    std::vector<std::pair<Bo, uint32_t>> grownOut_;
    bool overflowing_ = false;  // the open command buffer outgrew a ring with no memory for a larger one
    std::deque<std::vector<uint8_t>> overflow_;  // where it is written then (never submitted)
    uint32_t channel_ = 0, lastSeqno_ = 0;
    // the channel's fence word; before there is a channel a zero, which every seqno submitted so far (none) has passed
    const volatile uint32_t *fence_ = &kNoFence;
    static constexpr uint32_t kNoFence = 0;
    std::mutex channelLock_;
    std::atomic<bool> channelReady_{false};
    bool channelFailed_ = false;
    uint64_t channelRetryNs_ = 0;  // CLOCK_UPTIME_RAW: a refused channel is asked for again from then on
    static constexpr uint64_t kChannelRetryNs = 1000000000;
    std::string channelError_;
    // The channel died and the one made since has no context yet (reopen gave up before the context's Exec): begin() makes it
    // again before it gives out a command buffer. channel_ is 0 while the kernel holds none of this device's.
    bool contextless_ = false;
    std::vector<uint32_t> freeTic_, freeTsc_;
    // by header: its views' word 0 and header (viewTic)
    std::unordered_multimap<uint32_t, std::pair<uint32_t, uint32_t>> viewTics_;
    uint32_t nextTic_ = 1, nextTsc_ = 1, neutralTsc_ = 0;
    // the pack's, by record index, each made at its first lookup (a process uses a few of the pack's thousands); a node
    // container, so the pointers pipeline() hands out stay valid
    std::unordered_map<uint32_t, Pipeline> pipelines_;
    std::vector<uint32_t> placed_;  // each pack program's heap offset, ~0u: not placed
    uint64_t heapPlaced_ = 0;
    // heapPlaced_ as of the last submission whose prologue invalidated the SMs' instruction caches, ~0: none on this channel.
    // The instruction cache prefetches past a program's end, where heapPlace puts the next program.
    uint64_t heapInvalidated_ = ~0ull;
    // Shader local memory (raw_slm.h), under lock_: the area and the most a placed program or kernel asks of a warp. An area
    // it outgrew is kept: a submission already made may still use it.
    Bo slm_;
    uint64_t slmPerWarp_ = 0;
    std::vector<Bo> slmOutgrown_;
    // the per-warp size the channel was last given (commit; 0 on a new channel): the submitting thread's
    uint64_t slmChannel_ = 0;
    uint32_t ctWrite_[8] = {};  // SET_CT_WRITE(i) as the baseline leaves it
    bool slmNeed(uint64_t perWarp);
    bool place(uint32_t prog, uint32_t &program, uint64_t &data);
    bool heapGrow(uint64_t end);
    const HeapPart *heapPartAt(uint64_t at) const;
    bool heapPlace(const uint8_t *bytes, uint32_t size, uint64_t &offset);
    void build(Pipeline &pl, const PackPipeline &pp, const PlacedProgram &vs, const PlacedProgram &fs,
               const uint32_t *blob, const char *keyString, bool caTransform);
    // lock_ held: `pl`'s split offered, its twin made when a draw first takes it (makeSplit)
    void offerSplit(Pipeline &pl, SplitSource &&source);
    SplitState *makeSplit(Pipeline &pl);
    bool placeSplitPrograms(const SplitSource &source, PlacedProgram &fs, uint32_t &prologue, uint64_t &prologueData,
                            uint32_t &prologueDataBytes);
    std::deque<SplitSource> splitSources_;
    std::deque<SplitState> splitStates_;
    // The constexpr samplers' header indices, the pack's then the runtime records' (addRecord, under lock_). A slot is written
    // once, before the state that names it is published, and never moves: a draw reads it without lock_.
    static constexpr uint32_t kConstexprSamplers = 256;  // PackEntry::sampler is a byte
    uint32_t constexprTsc_[kConstexprSamplers] = {};
    uint32_t constexprCount_ = 0;

    // the states the runtime made (addRecord), never removed: Pipeline::rec points into them
    struct Completed {
        PackPipeline record;
        std::string keyString;
        std::vector<uint32_t> blob;
        PackProgram programs[2];
        Pipeline pipeline;
    };

    std::deque<Completed> completedStore_;
    std::unordered_map<uint64_t, Pipeline *> completed_;
    std::map<std::array<uint32_t, 8>, uint8_t>
        completedSamplers_;  // their constexpr samplers' headers, by constexprTsc_ index

    struct CompletedProgram {
        PackProgram info;
        PlacedProgram placed;
        uint8_t header[80];
    };

    std::map<std::string, CompletedProgram> completedPrograms_;  // by program id and constant buffer types
    bool placeCompleted(const CapturedProgram &p, PlacedProgram &out);
    // The constexprTsc_ index of sampler header `words`: the pack's own, one a runtime record placed, or placed now (under
    // lock_); false: no room
    bool constexprSampler(const std::array<uint32_t, 8> &words, uint32_t &index);

    void offerCompletedSplit(Completed &c, const RuntimeRecord &r, const PlacedProgram &vs, bool caTransform);
    bool takeTsc(uint32_t &tsc);
    const Pipeline *extrasLive_ = nullptr;  // the state whose extras (Pipeline::restore) the channel holds
    CommandBuffer cb_;
    uint32_t rootPush_[32] = {};  // what the GPU's root table holds at [0x700, 0x780) after everything submitted
    uint32_t restart_ = 0;        // and its primitive restart index
    DepthStencilRegs ds_;         // and its depth/stencil registers
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, Import *> surfaces_;
    std::vector<Chunk> chunks_;

    // Dedicated chunks freed but kept wired for the next allocation of the same size: a video's frames are surface textures
    // made and released one after another, each with a converted image past the dedicated limit (30 MB at 2500x1500), which
    // would otherwise be wired anew on the render thread for every frame. A chunk comes back only when its texture died,
    // so after every submission that used it completed.
    struct Spare {
        uint32_t chunk;
        uint64_t freedAt;  // commitCount_
    };

    std::vector<Spare> spares_;
    // mip chains' memory: chunks bound with PTE kind 0xFE (fx::kBlockLinearKind), made on demand, never CPU-written
    std::vector<Chunk> blChunks_;
    // zeta memory: chunks bound with each zeta format's PTE kind (kZetaFormatInfo), made on demand, never CPU-written
    std::vector<Chunk> zChunks_[kZetaFormats];
    // write-combined buffers' memory: GPU-pool BOs (BoAlloc coherent: the CPU maps them write-combined), sub-allocated by page
    std::vector<Chunk> wcChunks_;
    bool wcAlloc(uint64_t bytes, Span &s);
    void wcFree(Span &s);

    bool blAlloc(uint64_t bytes, uint64_t align, Span &s)
    {
        return tiledAlloc(blChunks_, fx::kBlockLinearKind, bytes, align, s);
    }

    void blFree(Span &s)
    {
        tiledFree(blChunks_, s);
    }

    struct Arena {
        uint64_t va = 0, bytes = 0;
        uint32_t kind = 0;
        uint8_t *cpu = nullptr;  // its host range, reserved for good; its pages wired and bound unless purged
        Bo bo;
        HeapPlacement place;
        uint32_t leases = 0;  // m2v views of its resources (raw_alias.cpp): never purged or freed under one
        bool used = false, isVolatile = false, purged = false, dying = false;
    };

    std::vector<Arena> arenas_;
    std::atomic<uint32_t> purgedArenas_{0};
    bool bindArena(Arena &a);  // lock_ held
    void freeArena(Arena &a);  // lock_ held
    void arenaFree(Span &s);   // lock_ held
    bool aliasArena(const Span &s, Lease *&lease, Alias &a);

    // a span of chunks bound with PTE kind `kind` (block-linear images), lock_ held
    bool tiledAlloc(std::vector<Chunk> &chunks, uint32_t kind, uint64_t bytes, uint64_t align, Span &s);
    void tiledFree(std::vector<Chunk> &chunks, Span &s);
    // lock_ held (`unlocked`: the caller's lock, let go for the kernel's wiring); the chunk's index in `chunks`
    bool newTiledChunk(std::vector<Chunk> &chunks, uint32_t kind, uint64_t size, bool dedicated,
                       std::unique_lock<std::mutex> *unlocked, uint32_t &index);
    // the free bytes of a kind's shared chunks, lock_ held
    static uint64_t tiledFreeBytes(const std::vector<Chunk> &chunks);

    // the block-linear or zeta chunks the grower thread makes one of next (lock_), each once
    struct TiledWant {
        std::vector<Chunk> *chunks;
        uint32_t kind;
    };

    std::vector<TiledWant> tiledWanted_;
    bool tiledWanted(const std::vector<Chunk> &chunks) const;

    // Plain memory under the memoryless backings of one tag and memory (the pool, block-linear chunks or a zeta format's),
    // each its own view (texture header, layout) of the same bytes, as compressed memory is (raw_comp.h CompMem): taken in
    // size classes (sizeClassBytes of kBlAlign), the largest one new backings go on, given back with its last view
    struct PlainMem {
        Span span;
        uint64_t align = 0;
        uint32_t tag = 0, memory = 0;  // kPlainPool, kPlainBl, or kPlainZeta + the zeta format's index
        uint32_t views = 0;
    };

    struct Backing {
        Texture texture;
        uint32_t tag = 0;
        uint32_t lastUse = 0;                     // the seqno of the last submission that used it (0: never submitted)
        uint64_t lastCommit = 0, lastClosed = 0;  // commitCount_ and closedCount_ when a pass last asked for it
        uint64_t usedNs = 0;        // compClock() when made, and at each commit of a command buffer that used it
        CompMem *comp = nullptr;    // the compressed memory it is a view of (raw_comp.h), else none
        PlainMem *plain = nullptr;  // or the plain memory (a memoryless one's)
        // a compressed backing of its shape, or one on larger memory of its tag, replaced it: no pass takes it again,
        // trimBackings frees it
        bool retired = false;
    };

    static constexpr uint32_t kPlainPool = 0, kPlainBl = 1, kPlainZeta = 2;
    std::vector<std::unique_ptr<PlainMem>> plainMems_;
    PlainMem *largestPlain(uint32_t tag, uint32_t memory) const;
    // A memoryless backing of `m`'s shape on the largest plain memory of its tag and memory, made where none holds it;
    // nullptr when no memory or header is had
    Backing *plainBacking(const Texture &m, uint32_t tag, bool blockLinear);
    void plainFree(Backing &b);
    void freePlainSpan(PlainMem &mem);
    void dropPlain(PlainMem *mem);  // its span back, and it out of plainMems_

    std::vector<Backing *> backings_;  // memoryless attachments' memory, by size and format
    // the pool's growth ahead of need (lock_): free bytes of its shared chunks, a chunk being made by the grower thread
    uint64_t poolFree_ = 0;
    bool growing_ = false, stopping_ = false;
    std::condition_variable grow_, grown_;
    std::thread grower_;
    bool newChunk(uint64_t size, bool dedicated, std::unique_lock<std::mutex> *unlocked, uint32_t &index);
    void growLoop();
    uint64_t commitCount_ = 0;
    uint64_t closedCount_ = 0;  // command buffers committed or abandoned (lock_)

    bool init(std::string &err);
    bool openChannel(std::string &err);
    void growRing(Ring &r, uint64_t most);
    void freeChunk(Chunk &ch);
    uint32_t contextWords() const;
    void emitContext(Push &p);
    bool reopen();
    bool waitFence(uint32_t seqno);
    void noteLost();
    // the last seqno of a channel that died: it and every one before it failed (or completed before; not told apart),
    // where the kernel writes no kill records
    std::atomic<uint32_t> lostThrough_{0};
    std::atomic<bool> lostAny_{false};

    // The seqnos a kill took (TegraGM20BAbiFence): the live channel's record, then the ones the channels it replaced left,
    // kept until reopen made the record's slot over (a slot's seqnos continue, so a kept record names its channel's alone).
    struct Killed {
        uint32_t first, last, error;
    };

    bool liveKill(Killed &k) const;
    bool killed(uint32_t seqno) const;
    mutable std::mutex killedLock_;
    std::array<Killed, 4> killedKept_{};
    uint32_t killedNext_ = 0;
    mutable std::atomic<uint32_t> killedError_{0};
    uint8_t *alloc(Ring &r, uint32_t bytes, uint32_t align, uint64_t &va);
    void retire(Ring &r, uint32_t seqno);
    bool ownWords(const Ring &r, uint64_t at, uint64_t bytes) const;
    bool outgrow(Ring &r, uint32_t bytes, uint64_t &at);
    uint32_t allocTic();
    void freeTic(uint32_t tic);  // lock_ held
    void writeTic(const Texture &t);
    bool poolAlloc(uint64_t bytes, Span &s);
    void poolFree(Span &s);
    bool tiledSurfaceTexture(uint64_t offset, uint32_t width, uint32_t height, Texture &t);
    Import *importSurface(uint64_t id, void *base, uint64_t bytes, const void *owner, const LayoutAsk *ask = nullptr);
    void release(Import *i);
    // lock_ held (raw_alias.cpp): a hold on a span's lease, made at its first alias; the last drop frees what the lease holds
    Lease *leaseSpan(Lease *&lease, const Span &span, Memory from);
    void dropLocked(Lease *lease);
    // lock_ held: the span goes back to where it came from now, or at its lease's last drop
    void freeSpan(Span &span, Memory from, Lease *&lease);

    static Memory memoryOf(const Texture &t)
    {
        return t.arena ? Memory::Arena : t.blockLinear ? Memory::Bl : t.wc ? Memory::Wc : Memory::Pool;
    }

    // A memoryless color attachment j's memory (tag kMemorylessTag + j: two of a pass never share one), a shadow target's pool
    // image for attachment j (tag 1 + j), or a memoryless zeta's (tag 0): one per size, format, tag and layout, kept while passes
    // use it. `blockLinear`: a colour backing of a pass with a block-linear colour target or a depth/stencil attachment; a zeta's
    // is zeta memory
    static constexpr uint32_t kMemorylessTag = 9;
    Texture *backing(const Texture &memoryless, uint32_t tag = 0, bool blockLinear = false);
    void freeBacking(Texture &t);
    void trimBackings();

    // Compressed backings (raw_comp.cpp), lock_ held: the compressible kind a backing of this shape takes (0: none or
    // compression off); a backing on compressed memory made off this thread, or nullptr after asking compThread_ for some
    CompQueue comp_;
    // made off the render thread on a thread of its own, started at the first ask: the kernel zeroes the pages a free left
    // dirty and clears the comptags at the bind, which takes milliseconds for a full-screen target, and no other thread (the
    // pool's grower included, which a render thread may wait for) waits behind it
    std::thread compThread_;
    std::condition_variable compWake_;
    uint32_t compKind(const Texture &m, uint32_t tag, bool blockLinear) const;
    Backing *compBacking(const Texture &m, uint32_t tag, uint32_t kind);
    // What was asked for, made, `l` dropped around each kernel allocation
    void compMake(std::unique_lock<std::mutex> &l);
    void compStop();
    // Asks compThread_ for `w` unless it was
    void compAsk(const CompWant &w);
    // compThread_ started where it is not: it trims what an idle process holds when it turns stale (compDeadline)
    void compStart();
    bool compRearm_ = false;  // shared plain memory was made: compThread_ takes its deadline (lock_)
    void compFree(Backing &b);
    // Made buffers no backing took while stale, at `now`
    void compTrim(uint64_t now);
    // When compressed or shared plain memory, a made buffer or a retired backing next turns stale (compClock()), 0 with
    // none held
    uint64_t compDeadline() const;
    // A clear of target t (counted only when compressed): `full` it covers t whole, `boot` its value is the boot ZBC table's
    void countZbc(const Texture &t, bool full, bool boot);
};

// NV topology for an MTLPrimitiveType (point, line, lineStrip, triangle, triangleStrip)
uint32_t nvTopology(uint32_t mtlPrimitive);

}  // namespace raw
