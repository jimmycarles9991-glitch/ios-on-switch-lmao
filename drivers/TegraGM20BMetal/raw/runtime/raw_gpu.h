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
#include "raw_kernel.h"
#include "raw_pack.h"
#include "raw_push.h"
#include "raw_overlap.h"
#include "raw_region.h"
#include "raw_zeta.h"
#include "raw_ycbcr.h"
#include "../fx/fx_mip.h"

namespace raw {

class Device;
class CommandBuffer;

constexpr uint32_t kPteKindPitch = 0;

struct Lease;  // raw_alias.cpp

// Where memory comes from: the pool, the write-combined GPU pool, an import, the block-linear chunks (kind 0xFE)
enum class Memory : uint8_t {
    Pool,
    Wc,
    Import,
    Bl
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
};

const FormatInfo *formatInfo(uint32_t mtlPixelFormat);

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
    // renders it through a pool image the copy engine fills from it before and copies back to it after (NVK's own tiled shadow,
    // nvk_linear_render_copy, for the same targets: the texels stored are the same)
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
    // through `sampled`, a pool image of its texels the copy engine refreshes from it (CommandBuffer::refresh) before work that
    // samples it, as m2v samples such a surface through a shadow image it copies in. tic is sampled's.
    Texture *sampled = nullptr;

    // A mipmapped texture's levels (r12, raw/fx): level 0 is this texture (pitch, pool memory: rendered, copied and cleared as any
    // other), and `chain` the block-linear image of every level that its texture header (tic) points at, which the 2D engine
    // fills from level 0 (BlitEncoder::generate). The chain's own level 0 is a copy: `l0Fresh` says it holds what the texture's
    // level 0 holds now, `levelsFresh` that its later levels were generated since the last generateMipmaps (encode order, under
    // the submit lock: the GPU's order).
    // `rawLevels`: levels written directly (a pass into the level, chainLevelTarget, or a copy from a mirror) since the last
    // generate, so not what generating from level 0 gives: m2v gets them by copies (RawMetal.mm mirrorsForM2v).
    // `mirrorLevels`: levels m2v rendered since, held by the RMTexture's mirror of each and copied into the chain before
    // the raw path next reads it (BlitEncoder::copyToLevel).
    struct Chain {
        fx::MipChain layout;
        uint64_t va = 0;
        Span span;
        uint32_t fmt2d = 0;
        bool l0Fresh = false, levelsFresh = false;
        uint16_t rawLevels = 0, mirrorLevels = 0;
        uint32_t levelTic[fx::kMaxLevels] = {};  // a view of each level alone (Device::levelTic), made on first use
    } *chain = nullptr;

    // Block-linear (TegraGM20BLayout.h): a chain level as a render target (chainLevelTarget: no memory or header of its own,
    // tic 0), or a texture laid out so (a tiled surface's, a memoryless attachment's backing in a block-linear pass): pitch
    // its row stride, `blockHeight` its block height (log2 GOBs), `layerBytes` the chain's or the texture's size
    bool blockLinear = false;
    uint8_t blockHeight = 0;
    uint64_t layerBytes = 0;

    // Depth32Float_Stencil8 (raw_zeta.h): block-linear zeta memory, bound with its own PTE kind; never sampled, copied or a
    // colour target
    bool zeta = false;
};

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
};

struct SamplerDesc {                                      // the MTLSamplerDescriptor fields the TSC takes
    uint8_t minFilter = 0, magFilter = 0, mipFilter = 0;  // MTLSamplerMinMagFilter / MTLSamplerMipFilter
    uint8_t sAddress = 0, tAddress = 0, rAddress = 0;     // MTLSamplerAddressMode (clampToBorderColor: not taken)
    uint8_t maxAnisotropy = 1;
    float lodMin = 0, lodMax = 3.402823466e+38f;
};

// Level `level` (1 .. levels - 1) of mipmapped texture `t`'s chain as a render target (raw_target.h: NVK's block-linear
// target of that level), for a pass to render into: `out` names the chain's memory, it owns nothing. False: not a level of it.
bool chainLevelTarget(const Texture &t, uint32_t level, Texture &out);
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

struct Pipeline {
    const PackPipeline *rec = nullptr;
    std::vector<uint32_t> methods;         // the state's registers as ready push words (program offsets relocated)
    uint32_t push[32] = {}, pushMask = 0;  // root table [0x700, 0x780): the words m2v's push ranges write, and which
    uint8_t setSlot[2] = {};  // the constant buffer slot the vertex and the pixel program read set 0 at (0: none)
    uint8_t writeBits = 0;    // colors its draws write: SET_CT_WRITE(i) with a channel on
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
    uint8_t readBits = 0;      // colors its draws read as they were: blended into, or written through a partial mask
    uint8_t fetchBits = 0;     // colors it reads by framebuffer fetch
    uint32_t depthFormat = 0;  // its key's depth and stencil attachment format (ds=<format>/<format>; 0: none)
};

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

// The pass's Depth32Float_Stencil8 attachment, its depth and stencil aspects one texture (memoryless: neither loaded nor stored)
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
    void setBuffer(int stage, uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setBufferOffset(int stage, uint32_t slot, uint64_t offset);

    void setTexture(uint32_t slot, const Texture *t)
    {
        if (slot < 16)
            textures_[slot] = t;
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

    void setViewport(double x, double y, double w, double h, double znear, double zfar);
    void barrier();  // memoryBarrier...: everything before is written and visible to what follows
    // false: this draw cannot be drawn exactly as m2v would (*why says what); the command buffer goes to m2v instead
    bool draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
              const char **why);
    // drawIndexedPrimitives of one instance, base vertex and instance 0 (Core Animation's meshes): `count` indices of `indexSize`
    // bytes (2 or 4) at `offset` in `ib`
    bool drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib, uint64_t offset,
                     const char **why);
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
    uint32_t *bodyStart_ = nullptr;
    std::vector<std::pair<uint32_t *, uint32_t *>> bodies_;  // the pass body's push segments, in order
    void reserve();
    Pipeline *pipeline_ = nullptr, *emitted_ = nullptr;
    Bound bufs_[3][31];
    const Texture *textures_[16] = {};
    const Sampler *samplers_[16] = {};
    uint64_t vbVa_ = 0, vbLimit_ = 0, ibVa_ = 0, ibEnd_ = 0;
    uint32_t ibSize_ = 0;
    // the state a draw needs, up to its draw parameters (vertices [vFirst, vFirst + vCount) of the stream are CPU-cleaned)
    bool prepare(Pipeline *pl, uint32_t primitive, uint32_t first, uint32_t vFirst, uint32_t vCount,
                 uint32_t baseInstance, bool indexed, const char **why);
    uint32_t scissor_[2] = {}, viewport_[8] = {};
    bool scissorDirty_ = true, viewportDirty_ = true;
    DepthStencilDesc ds_;
    uint32_t stencilRef_[2] = {};
    bool dsDirty_ = true;

    // the encoder's culling, winding and fill, and what the registers hold: the context's (Metal's defaults: no culling,
    // clockwise, fill) at a pass's start, put back at its end
    struct Raster {
        uint8_t cull = 0, winding = 0, fill = 0;
    };

    Raster raster_, rasterHeld_;
    void writeRaster(Push &p, const Raster &want);
    uint32_t blendColor_[4] = {};  // Metal's default, (0, 0, 0, 0), until the encoder sets one
    bool blendColorDirty_ = true;
    uint32_t topology_ = ~0u, restart_ = ~0u, first_ = 0, baseInstance_ = 0;
    uint8_t writtenBits_ = 0;         // colors the ROP wrote since the last barrier
    uint8_t storageWrittenBits_ = 0;  // colors a fragment shader may have written as storage since the last barrier
    bool memoryless_[8] = {};
    const Texture *shadowOf_[8] =
        {};  // a shadow target's texture, where pass_ holds its pool image (CommandBuffer::render)
    // what m2v's DontCare loads answer (metal2vulkan clear_reach.rs: dont_care_loads, DrawnArea): whether the pass has a draw
    // call and one that draws, the colors its draws read, and the pixels they may write (scissor within viewport, each draw)
    bool called_ = false, drew_ = false, whole_ = false;
    uint32_t readBits_ = 0, areaW_ = 0, areaH_ = 0;
    double vp_[4] = {};
    uint32_t sc_[4] = {};
    std::vector<std::array<uint32_t, 4>> rects_;
    // the draws that wrote a color since the last barrier, for the fetch barrier's region test (raw_region.h); the
    // indices of the indexed draw being prepared
    WrittenRegion written_;
    // colors the pass's first draw overwrites whole and opaquely: their clears inside the render area are not needed
    uint8_t covered_ = 0;
    using Runs = std::vector<std::pair<uint32_t, uint32_t>>;
    const Runs &selfRuns(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances);
    const uint8_t *drawIndices_ = nullptr;
    uint32_t drawIndexSize_ = 0, drawIndexCount_ = 0;
    DrawGeometry geometry(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count,
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
    // `lines` rows of `width` pixels of `bpp` bytes, from `src` (row stride srcPitch) to `dst` (dstPitch) (vkCmdCopyImage and the
    // buffer<->image copies of linear images)
    void copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width, uint32_t lines,
                  uint32_t bpp);
    // `w` x `h` texels from (sx, sy) of `src` to (dx, dy) of `dst`, each pitch or block-linear (vkCmdCopyImage)
    void copyTexels(const Texture &src, uint32_t sx, uint32_t sy, const Texture &dst, uint32_t dx, uint32_t dy,
                    uint32_t w, uint32_t h);
    // `w` x `h` texels between rows `pitch` bytes apart at `va` (both multiples of the texel's component, at most 4 bytes) and
    // (x, y) of `t`, pitch or block-linear: into `t` where `toTexture`, else out of it (vkCmdCopyBufferToImage,
    // vkCmdCopyImageToBuffer)
    void copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          bool toTexture);
    // A planar texture's image from its surface's planes (Texture::planar): two remapped copies, the luma codes into green
    // (alpha 255), then the chroma pairs into red (Cr) and blue (Cb), each leaving the other components as they are
    void convertPlanar(const Texture &t);
    // A mipmapped texture's chain (Texture::chain) on the 2D engine: `all`, generateMipmaps (level 0 converted from the texture,
    // then each level from the one above); else level 0 only (the texture's level 0 was written since: what sampling it needs)
    void generate(Texture &t, bool all);
    // `w` x `h` texels of chain level `level` of `t` at (sx, sy) into the pitch texture `dst` at (dx, dy) (copyFromTexture of a
    // level below the first); 1:1 on the 2D engine
    void copyFromLevel(const Texture &t, uint32_t level, uint32_t sx, uint32_t sy, const Texture &dst, uint32_t dx,
                       uint32_t dy, uint32_t w, uint32_t h);
    // The whole of chain level `level` of `t` from the pitch texture `src` of its size (its mirror); 1:1 on the 2D engine
    void copyToLevel(Texture &t, uint32_t level, const Texture &src);
    void end();

private:
    friend class CommandBuffer;
    CommandBuffer *cb_ = nullptr;
    Device *dev_ = nullptr;
    Push p_;
    uint32_t *start_ = nullptr;
    void room(uint32_t words = 56);  // at least `words` words left in the reservation
};

// Which encode-time predicate refused a draw (set beside *why; RawMetal.mm counts and logs it, r10-rawsweep diagnostics)
enum RefusalId : uint8_t {
    RefNone,
    RefIndexedRange,
    RefBaseInstance,
    RefColorFormats,
    RefUboOffset,
    RefNoTic,
    RefDepthStencil  // a: the state's depth format, b: the pass's
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
    bool commit(uint32_t &seqno);
    // Nothing of it is submitted: the rings get back what it took, the root table's shadow what it was.
    void abandon();

    // A host import the submission reads or writes (a surface's, a no-copy buffer's), for the kernel's order with other
    // processes' GPU work on it (raw_kernel.h exec)
    void use(const Import *i, bool write)
    {
        if (i)
            surfaces_.add(i->bo.handle, write);
    }

    uint32_t draws = 0, passes = 0, blits = 0;

private:
    friend class Device;
    friend class RenderEncoder;
    friend class BlitEncoder;
    Device *dev_ = nullptr;
    std::vector<TegraGM20BAbiPush> segments_;
    std::vector<std::pair<uintptr_t, uint64_t>> clean_, flush_;  // CPU ranges to clean / clean+invalidate before Exec
    std::vector<Texture *> backings_;                            // memoryless backings it uses
    SurfaceList surfaces_;
    RenderEncoder render_;
    BlitEncoder blit_;
    uint64_t pushStart_ = 0, uploadStart_ = 0;
    uint32_t rootPush_[32] = {};
    uint32_t restart_ = 0;  // SET_DA_PRIMITIVE_RESTART_INDEX as the GPU will hold it
    DepthStencilRegs ds_;   // and its depth/stencil registers
    bool drawn_ = false;    // a draw was encoded (NVK resets the restart index at a command buffer's first)
    void segment(uint32_t *from, uint32_t *to);
};

class Device {
public:
    ~Device();
    bool open(const char *packPath, uint64_t stack, std::string &err);

    const Pack &pack() const
    {
        return pack_;
    }

    // A pack pipeline by its state key's FNV-1a (RawMetal.mm rawPipelineKey); null: not in the pack.
    // caTransform: its vertex function is Core Animation's fixed_vert_l?h_spc (Pipeline::caTransform)
    Pipeline *pipeline(uint64_t keyHash, PackForm form = PackForm::Default, bool caTransform = false);

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
    // s's header under address map `map` (Texture::addressMap), made now if it is not yet
    bool samplerVariant(Sampler &s, uint8_t map);
    // A two-plane 4:4:4 surface's texture of RGB8_444_2P (542): its planes at `luma`/`chroma` (offsets in the allocation,
    // with their pitches), sampled through an RGBA8 image of the pool (Texture::planar)
    bool createPlanarTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, uint64_t luma,
                             uint32_t lumaPitch, uint64_t chroma, uint32_t chromaPitch, uint32_t width, uint32_t height,
                             Texture &t);
    bool createBufferTexture(const Buffer &b, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t format,
                             Texture &t);  // over a buffer's memory (t holds no reference: the caller keeps the buffer)
    bool createView(const Texture &of, uint32_t format, Texture &t);  // the same memory as another format
    // Memoryless (a pass gives it a backing); Depth32Float_Stencil8 among them, as zeta (Texture::zeta)
    bool createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t);
    // A mipmapped 2D texture of `levels` levels: level 0 a pool texture as createTexture's, its header the block-linear chain's
    // (Texture::chain). False: not one the raw path keeps (chainsOn() false, a format the 2D engine does not filter, no memory).
    bool createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t);
    // The texture header of level `level` of mipmapped texture `t`'s chain alone, as NVK's view of that level (a pass into
    // the level fetches its color through it); 0: none
    uint32_t levelTic(Texture &t, uint32_t level);

    // Whether the connection is NVK's (raw_kernel.h): m2v's views alias this path's memory
    bool shared() const
    {
        return k_.shared();
    }

    // Whether mip chains are on: the 2D engine's self-test at open made the chain the design expects (fx_mip.h)
    bool chainsOn() const
    {
        return chainsOn_;
    }

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

    uint64_t heapBytes() const
    {
        return heap_.bytes;
    }

    bool createBufferNoCopy(void *bytes, uint64_t length, Buffer &b);
    void destroyBuffer(Buffer &b);

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
    // and the pool's grower started: the next chunk is made on its own thread once the free bytes of the pool's shared chunks fall
    // below a low mark, so the render thread's allocations find room (Stats::poolStalls counts the ones that had to grow it).
    void warm();

    // Submission. begin() opens the one command buffer (the caller serialises encoding and commit).
    CommandBuffer *begin();

    uint32_t completed() const
    {
        return *fence_;
    }

    uint32_t submitted() const
    {
        return lastSeqno_;
    }

    bool done(uint32_t seqno) const
    {
        return int32_t(*fence_ - seqno) >= 0;
    }

    // In the kernel, on the fence interrupt. False: the submission failed, the channel died with it in flight (reopen).
    bool wait(uint32_t seqno)
    {
        if (!done(seqno) && !k_.wait(channel_, seqno) && k_.lost(channel_))
            noteLost();
        return !lost(seqno);
    }

    bool lost(uint32_t seqno) const
    {
        return lostAny_.load(std::memory_order_acquire) &&
               int32_t(lostThrough_.load(std::memory_order_relaxed) - seqno) >= 0;
    }

    // GPU-side order with the process's other driver (NVK's channels), through the kernel's Order (TegraGM20BInfoOrder; false:
    // it has none). orderAfterOthers: this channel's next submission starts with an acquire on what they submitted so far;
    // orderOthersAfter: their next submissions start with one on everything this channel submitted. The kern_return_t (0:
    // queued; else the caller orders on the CPU); `waits`: the acquires queued. Called where submissions are (serialised).
    bool canOrder() const
    {
        return k_.canOrder();
    }

    int orderAfterOthers(uint32_t &waits)
    {
        return k_.order(channel_, TegraGM20BOrderAfterTask, 0, waits);
    }

    int orderOthersAfter(uint32_t &waits)
    {
        return k_.order(channel_, TegraGM20BOrderBeforeTask, lastSeqno_, waits);
    }

    struct Stats {
        std::atomic<uint64_t> commits{0}, pushBytes{0}, uploadBytes{0}, ringWaits{0}, imports{0}, importBytes{0},
            poolBytes{0}, poolChunks{0}, cleanBytes{0}, poolStalls{0};
        // surface imports made (a kernel import each) and found live, since open
        std::atomic<uint64_t> importsMade{0}, importHits{0};
        // diagnostics: color targets the raw path rendered whose address or pitch is not a multiple of 128 (NVK renders
        // those through a tiled shadow: nvk_cmd_draw.c nvk_image_plane_aligned_for_linear_attachment), and the last one's pitch
        std::atomic<uint64_t> rtUnaligned{0};
        // color targets in rows the raw path rendered through a block-linear shadow, beside a block-linear one in their pass
        std::atomic<uint64_t> rtMixed{0};
        std::atomic<uint32_t> rtUnalignedPitch{0}, rtUnalignedVa{0};
        // mip chains: made, chain bytes, generated (whole) and level-0 refreshes, copies out of a level
        std::atomic<uint64_t> chains{0}, chainBytes{0}, chainGenerates{0}, chainRefreshes{0}, chainCopies{0};
        // write-combined buffers: live bytes, and the buffers that asked for one and got pool memory (the WC pool full)
        std::atomic<uint64_t> wcBytes{0}, wcFallbacks{0};
        // the copy engine's work encoded: launches and the bytes they write (copies, remaps, the pass prologue's shadow copies)
        std::atomic<uint64_t> ceOps{0}, ceBytes{0};
        // passes into a chain level, and levels copied between a chain and its mirrors (out: for m2v; in: what m2v rendered)
        std::atomic<uint64_t> levelPasses{0}, mirrorsOut{0}, mirrorsIn{0};
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

    // The self-test's result (open), per sampling candidate (fx::sampling(i)): bytes off by more than 1 in the CPU's Tegra and
    // Fermi views (~0u: not run); the one chains use (selfTestChosen, fx::kSamplings: none passed) and its view; the first
    // candidate's detail in the Tegra view (what the documented convention gave)
    uint32_t selfTestTegra[fx::kSamplings], selfTestFermi[fx::kSamplings];
    uint32_t selfTestChosen = fx::kSamplings;
    bool selfTestFermiView = false;
    fx::SelfTestReport selfTestFirst;

    fx::Sampling sampling() const
    {
        return sampling_;
    }

private:
    friend class CommandBuffer;
    friend class RenderEncoder;
    friend class BlitEncoder;

    struct Ring {
        Bo bo;
        uint64_t head = 0, since = 0;  // next free byte; where the open command buffer's use began

        struct Span {
            uint64_t begin, end;
            uint32_t seqno;  // 0: the open command buffer's
        };

        std::deque<Span> inflight;
    };

    struct Chunk {
        Bo bo;
        void *memory = nullptr;
        uint64_t bytes = 0;
        std::map<uint64_t, uint64_t> free;  // offset -> bytes, coalesced
        uint64_t used = 0;
        bool dedicated = false;
    };

    std::mutex lock_;
    Kernel k_;
    Pack pack_;
    Bo heap_, tic_, tsc_, root_, zero_, tickets_;
    Ring push_, upload_;
    uint32_t channel_ = 0, lastSeqno_ = 0;
    const volatile uint32_t *fence_ = nullptr;
    std::vector<uint32_t> freeTic_, freeTsc_;
    uint32_t nextTic_ = 1, nextTsc_ = 1, neutralTsc_ = 0;
    // the pack's, each made at its first lookup (rec set); sized once at init and never resized: pipeline() hands out
    // pointers into it
    std::vector<Pipeline> pipelines_;
    std::vector<uint32_t> placed_;  // each pack program's heap offset, ~0u: not placed
    uint64_t heapPlaced_ = 0;
    uint32_t ctWrite_[8] = {};  // SET_CT_WRITE(i) as the baseline leaves it
    bool place(uint32_t prog, uint32_t &program, uint64_t &data);
    std::vector<uint32_t> constexprTsc_;
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
    // zeta memory: chunks bound with kZetaPteKind, made on demand, never CPU-written
    std::vector<Chunk> zChunks_;
    // write-combined buffers' memory: GPU-pool BOs (BoAlloc coherent: the CPU maps them write-combined), sub-allocated by page
    std::vector<Chunk> wcChunks_;
    bool wcAlloc(uint64_t bytes, Span &s);
    void wcFree(Span &s);
    bool chainsOn_ = false;
    fx::Sampling sampling_;  // the self-test's winner: how every chain's scaled steps sample

    bool blAlloc(uint64_t bytes, uint64_t align, Span &s)
    {
        return tiledAlloc(blChunks_, fx::kBlockLinearKind, bytes, align, s);
    }

    void blFree(Span &s)
    {
        tiledFree(blChunks_, s);
    }

    // a span of chunks bound with PTE kind `kind` (block-linear images), lock_ held
    bool tiledAlloc(std::vector<Chunk> &chunks, uint32_t kind, uint64_t bytes, uint64_t align, Span &s);
    void tiledFree(std::vector<Chunk> &chunks, Span &s);
    void selfTest();

    struct Backing {
        Texture texture;
        uint32_t tag = 0;
        uint32_t lastUse = 0;  // the seqno of the last submission that used it (0: never submitted)
        uint64_t lastCommit = 0;
    };

    std::vector<Backing *> backings_;  // memoryless attachments' memory, by size and format
    // the pool's growth ahead of need (lock_): free bytes of its shared chunks, a chunk being made by the grower thread
    uint64_t poolFree_ = 0;
    bool growing_ = false, stopping_ = false;
    std::condition_variable grow_, grown_;
    std::thread grower_;
    bool newChunk(uint64_t size, bool dedicated, std::unique_lock<std::mutex> *unlocked, uint32_t &index);
    void growLoop();
    uint64_t commitCount_ = 0;

    bool init(std::string &err);
    uint32_t contextWords() const;
    void emitContext(Push &p);
    bool reopen();
    void noteLost();
    // the last seqno of a channel that died: it and every one before it failed (or completed before; not told apart)
    std::atomic<uint32_t> lostThrough_{0};
    std::atomic<bool> lostAny_{false};
    uint8_t *alloc(Ring &r, uint32_t bytes, uint32_t align, uint64_t &va);
    void retire(Ring &r, uint32_t seqno);
    uint32_t allocTic();
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
        return t.blockLinear ? Memory::Bl : t.wc ? Memory::Wc : Memory::Pool;
    }

    // A memoryless color attachment j's memory (tag kMemorylessTag + j: two of a pass never share one), a shadow target's pool
    // image for attachment j (tag 1 + j), or a memoryless zeta's (tag 0): one per size, format, tag and layout, kept while passes
    // use it. `blockLinear`: a colour backing of a pass with a block-linear colour target or a depth/stencil attachment; a zeta's
    // is zeta memory
    static constexpr uint32_t kMemorylessTag = 9;
    Texture *backing(const Texture &memoryless, uint32_t tag = 0, bool blockLinear = false);
    void freeBacking(Texture &t);
    void trimBackings();
};

// NV topology for an MTLPrimitiveType (point, line, lineStrip, triangle, triangleStrip)
uint32_t nvTopology(uint32_t mtlPrimitive);

}  // namespace raw
