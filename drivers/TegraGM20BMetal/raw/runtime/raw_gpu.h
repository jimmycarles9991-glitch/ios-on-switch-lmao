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
#include "raw_lock.h"
#include "raw_dirty_ranges.h"
#include "raw_batch.h"
#include "raw_mme.h"
#include "raw_icb.h"
#include "raw_state_segment.h"
#include "raw_replay_shape.h"
#include "raw_split_capture.h"
#include "raw_split_reuse.h"
#include "raw_split_batch.h"
#include "raw_split_cost.h"
#include "raw_attachment_order.h"
#include "raw_attachment_view.h"
#include "raw_probe_forms.h"
#include "raw_probe_ops.h"
#include "raw_private_comp.h"
#include "raw_event_release.h"
#include "raw_predication.h"
#include "raw_loop_copies.h"
#include "raw_loop_ranges.h"
#include "raw_visibility.h"
#include "raw_fresh.h"
#include "raw_view.h"
#include <array>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>
#include "raw_comp.h"
#include "raw_counters.h"
#include "raw_attachment_lifetime.h"
#include "raw_stats.h"
#if SWITCH_RAW_DIAGNOSTICS
#include "raw_glass_mem.h"
#endif
#include "raw_descriptors.h"
#include "raw_encode_profile.h"
#include "raw_use_memo.h"
#include "raw_transfer.h"
#include "raw_fetch_free.h"
#include "raw_fetch_order.h"
#include "raw_kernel.h"
#include "raw_channel_recovery.h"
#include "raw_fence.h"
#include "raw_host_access.h"
#include "raw_arena_trim.h"
#include "raw_heap.h"
#include "raw_heap_members.h"
#include "raw_heap_tables.h"
#include "raw_order.h"
#include "raw_variants.h"
#include "raw_pack.h"
#include "raw_program_pages.h"
#include "raw_push.h"
#include "raw_overlap.h"
#include "raw_region.h"
#include "raw_sampler.h"
#include "raw_depth_stencil_state.h"
#include "raw_sample_positions.h"
#include "raw_zcull.h"
#include "raw_xr.h"
#include "raw_ycbcr.h"
#include "raw_rotation.h"
#include "raw_viewports.h"
#include "raw_turn_copy.h"
#include "raw_stencil_bits.h"
#include "raw_reach.h"
#include "raw_draw_bands.h"
#include "raw_band_rates.h"
#include "raw_span_marks.h"
#include "raw_view_cache.h"
#include "raw_viewports.h"
#include "../fx/fx_mip.h"
#include "raw_surface_image.h"

namespace raw {

struct FillStream;

constexpr uint32_t kTextureHeaders = 8192;  // the texture header pool's entries (Device::allocTic)

class Device;
class CommandBuffer;
class ComputeEncoder;
struct ComputeState;
struct ComputeRecord;
struct NativeSurface;

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
    NativeSurface *native = nullptr;

    // Bound 0xFE as the kernel laid the surface out, not under a layout of one texture's own (createBlockTextureOver,
    // createSurfaceImage), whose textures address their memory as their images say
    bool kernelTiled() const
    {
        return kind == TGM_LAYOUT_KIND && layout.bytes;
    }
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

// An MTLHeap's arena a texture is made in (raw_arena.cpp): at `offset` of a placement heap's, else wherever it fits
struct ArenaAt {
    uint32_t arena = 0;
    std::optional<uint64_t> offset;
};

// A texture of one of the kinds the device makes, by the Device::create* that makes it, with that one's arguments
// (Device::footprint, arenaTexture)
struct TextureAsk {
    enum Kind : uint8_t {
        Plain,        // createTexture: `tiled` its block-linear ask
        Line,         // createLine
        Mips,         // createMipTexture
        Volume,       // createVolume: `view2d`, `levels`
        Array,        // createArray: `type` its header's, `layers` (six a cube), `levels`
        Multisample,  // createStoredMultisample
        Zeta          // createOwnedZeta: `sampled` (a multisampled one has a header either way)
    } kind = Plain;

    uint32_t width = 0, height = 1, depth = 1, layers = 1, levels = 1, samples = 1, format = 0;
    uint8_t type = 0;
    bool tiled = false, view2d = false, sampled = false;
    bool standIn = false;  // an extended-range Plain one's stand-in (createStandIn) beside it
};

struct Texture {
    struct StorageRange {
        uint64_t va = 0, bytes = 0;
        bool hostVisible = false;
    } storage;

    StorageRange storageRange() const;
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
    uint32_t tic = 0;  // texture header index, 0: none (not sampleable)
    uint32_t nativePlane = ~0u;
    uint64_t nativeGeneration = 0;
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
    // A two-plane YCbCr texture rendered into (raw_ycbcr_store.h) renders through planarStandIn too, a memoryless RGBA32Float
    // attachment whose backing is the pass's; the pack's store states then draw the luma plane and the chroma blocks from that
    // backing, ycbcrStore their fragment buffer
    YcbcrStoreShape *ycbcrStore = nullptr;
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
    // A no-copy block texture whose caller's memory is off its image's alignment (raw_block_base.h) is a pool image of the
    // same layout: callerPages are the caller's pages, bound pitch, that the copy engine fills the image from
    // (Device::copyCallerPages) and that Metal's copies into the image go on to. Both copies reach the image through
    // imagePitchVa, a kind-0 view of its chunk (Device::blPitchVa): each kind places a GOB's bytes in DRAM its own way
    // (TegraGM20BRelayout.h), so only kind 0 on both sides moves the DRAM bytes unchanged. A view's are its parent's.
    Import *callerPages = nullptr;
    uint64_t imagePitchVa = 0;
    bool surfaceView = false;
    uint32_t surfaceX = 0, surfaceY = 0;

    // An extended-range texture's (raw_xr.h): the image a draw samples, its codes' values as RGBA16Float, which a pass of the
    // pack's conversion state draws from the codes (tic) before the work that samples it. Null: not sampled.
    Texture *standIn = nullptr;
    // RGB10A8_2P_XR10 held in Metal's two planes (raw_xr_pass.h): luma the words (R32Uint), chroma the alpha bytes (R8Uint),
    // each a target and a fetch's header; this texture has no memory, and its header (tic, an over-planes one's) is plane
    // 0's words read as RGB10A2Unorm codes, plane 1's bytes as R8Unorm at the next index. xrPlane on luma (1) and chroma (2)
    bool xrPlanes = false;
    uint8_t xrPlane = 0;
    // Metal keeps it losslessly compressed (m2v nan_clear.rs keeps_nan_sign): an extended-range Clear in a pass that records
    // no draw call stores Metal's straight route's codes
    bool compressed = false;
    // Its memory is bound with a compressible kind (raw_comp.h): its passes enable colour, or Z and stencil, compression
    bool comptags = false;
#if SWITCH_RAW_DIAGNOSTICS
    // A pass, copy, dispatch or upload of this process wrote it (raw_glass_probe.h)
    bool probeWritten = false;
#endif

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
    // Metal's rotation of a texture over a surface plane (raw_rotation.h): width and height are the plane's, the frame a pass
    // draws is turned from them
    uint8_t rotation = 0;
};

struct NativeSurface {
    Bo bo;
    TegraGM20BAbiNativeSurfaceViewReply layout = {};
    using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::map<Key, Texture> textures;
    uint64_t channelGeneration = 0, generation = 0;
    uint32_t lastUse = 0, encodingReferences = 0;
    bool cached = true;
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
    bool hostVisible = true;
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
    // its header with depth comparison on (desc.compare), which a state that samples its slot by comparison reads
    // (entryCompares): m2v makes the slot's sampler per use, compare-enabled for such a state
    uint32_t compareTsc = 0;
    SamplerDesc desc;
    // its headers under a planar texture's address map (Texture::addressMap 1 to 3): m2v samples those textures through the
    // sampler with the map applied (Device::samplerVariant makes them; the same header where the map changes nothing)
    uint32_t mapped[4] = {};
};

struct SplitState;
struct SplitSource;

#if SWITCH_RAW_DIAGNOSTICS
// The keyline probe (raw_keyline_probe.h) for 30 s or 256 lines: its budgets start again, and every Core Animation state's
// thin draws log their line
void armKeylineProbe();
#endif

struct Pipeline {
    const PackPipeline *rec = nullptr;
    uint32_t vertexInstructions = 1, fragmentInstructions = 1;
    std::vector<uint32_t> methods;  // the state's registers as ready push words (program offsets relocated)
    uint32_t viewportClip = 0;      // its SET_VIEWPORT_CLIP_CONTROL
    bool namesViewportClip = false;
    bool namesViewportClipRange = false;   // it sets SET_VIEWPORT_CLIP_MIN_Z or MAX_Z
    StateSegment segment;                  // the same words in the device's segment heap; empty: they go inline
    uint32_t push[32] = {}, pushMask = 0;  // root table [0x700, 0x780): the words m2v's push ranges write, and which
    // the constant buffer slot the vertex, pixel, tessellation init and tessellation program read set 0 at (0: none), whose
    // bind groups are kSetGroups'
    uint8_t setSlot[4] = {};
    uint16_t streamStride[kPackVertexStreams] =
        {};  // each vertex stream's static stride (its SET_VERTEX_STREAM_A_FORMAT)

    // the uniform buffers of set 0 its programs read as constant buffers: the group (0 vertex, 4 pixel), the slot and the set's
    // entry that names the buffer; false where a program reads one no entry names (its draws are refused)
    struct UboCbuf {
        uint8_t group, slot, entry;
    };

    UboCbuf ubos[4 * (sizeof(PackProgram::cbufType) - 1)] = {};  // constant buffers 1.. of each program
    uint8_t uboCount = 0;
    bool ubosNamed = true;
    // set 0's entries its programs read from the set's upload: the record's (PackPipeline::memoryEntries), and those a
    // variant's or a split twin's own programs read
    uint32_t memoryEntries = ~0u;
    uint8_t writeBits = 0;  // colors its draws write: SET_CT_WRITE(i) with a channel on
    // colors its fragment shader reads, and may write, as storage images inside the pixel interlock (metal2vulkan's storage
    // form of a fetch: the state turns the interlock on with SET_PIXEL_SHADER_INTERLOCK_CONTROL); the ROP writes none of them
    uint8_t storageBits = 0;
    uint8_t storageWriteBits = 0;
    // of storageWriteBits, those whose fixed-function blend the fragment shader computes inside the interlock
    uint8_t storageBlendBits = 0;
    uint32_t storageTic[8] = {};
    // the same state's barrier form, which orders its fetches with barriers where a pass's attachments cannot be viewed as
    // the storage images of this one (raw_storage_form.h); resolved with the state, null where the pack has none
    Pipeline *barrierTwin = nullptr;
    // a storage form that computes the pipeline's blend: its barrier form, placed in debug builds only and drawn only where
    // the probe forces it (raw_fetch_rungs.h kProbeBlendTwinTriangles, kProbeBlendTwinWhole)
    Pipeline *blendTwin = nullptr;
    // a state of bind-stride vertex streams: the same state with those layouts pulled in the shader (RuntimeRecord's
    // strideTwin), which a draw takes where a stream's bind stride is past SET_VERTEX_STREAM_A_FORMAT's 12 bits
    Pipeline *strideTwin = nullptr;
    // the same state's fetch-free form (raw_fetch_free.h), which a draw takes where its rung's rule holds; null where the
    // pack has none. On that form: the rung it draws on and the colour it blends
    Pipeline *fetchFree = nullptr;
    FetchFreeForm freeForm;
    bool constantBlend = false;     // a blend factor of a blended color is the encoder's blend color
    bool blendKill = false;         // its SET_ITERATED_BLEND_OPTIMIZATION drops zero-source pixels (raw_blend_kill.h)
    bool fragmentDepth = false;     // its fragment program writes depth (SPH OMAP_DEPTH)
    bool viewportIndex = false;     // its vertex program writes ViewportIndex (SPH OMAP, raw_viewports.h)
    bool writesLayer = false;       // its last stage before the rasterizer writes Layer: SET_RT_LAYER takes it
    uint32_t colorFormats[8] = {};  // MTLPixelFormat of each color attachment the state was made for (0: none)
    // an amplifying runtime record's push words + 1 of m2v's AmplificationLayerOffset and AmplificationViewportOffset
    // (RuntimeRecord's; 0: none), which each amplification writes
    uint8_t amplificationLayerWord = 0, amplificationViewportWord = 0;
    // its vertex function is Core Animation's fixed_vert_l?h_spc (raw_bounds.h), and attribute 0 (the position) as its key
    // gives it: floats from the stream's vertex start (0: no float position)
    bool caTransform = false;
    uint8_t positionComponents = 0;
    uint16_t positionOffset = 0;
    // its draws may leave a pixel of their triangles unwritten: the fragment program discards (SPH KILLS_PIXELS) or writes
    // the sample mask, or the state tests depth, stencil or alpha to coverage, or rasterizes more than one sample
    bool dropsPixels = true;
    bool primitiveId = false;  // fragment input observes primitive numbering, which a split BEGIN would reset
    uint8_t samples = 1;       // its key's rasterSampleCount: a pass of attachments of that many samples draws it
    // SET_HYBRID_ANTI_ALIAS_CONTROL's passes: more than one where it shades per sample, whose sample masks differ (raw_samples.h)
    uint8_t samplePasses = 1;
    mutable BandRate bandRate;  // what its draws cost the GPU, learned from its measured ones (raw_band_rates.h)
    uint8_t readBits = 0;       // colors its draws read as they were: blended into, or written through a partial mask
    uint8_t fetchBits = 0;      // colors it reads by framebuffer fetch
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
    // DEVELOPMENT: a glass background or generic merge state's name until its uniforms show a non-finite float (one such
    // value makes the whole glass band black), and whether non-finite halves alone were reported
    const char *uniformProbe = nullptr;
    bool uniformHalvesReported = false;
    bool genericState = false;  // DEVELOPMENT: Core Animation's generic merge state (..Xghfc), for the fetch keys line
    // its stages' program variants (raw_variants.cpp), and the pipeline drawn for each choice, vertex choice major (the
    // last: this one), each made the first time a draw selects it
    const PackVariant *variants[2] = {};
    uint8_t variantCount[2] = {};
    std::vector<Pipeline *> chosen;
    // DEVELOPMENT: a Core Animation state's name, whose draws log their form and quads (RenderEncoder::probeDraw): a solid
    // fill into its three-colour pass (..Xmw_A3Xhfc, the scroll edge keyline's state: fillState) its first draws and thin
    // ones always, any other state its thin ones while the keyline probe is armed (armKeylineProbe); the probe's generation
    // this form's lines count in, and its lines there, of any draw and of thin ones
    const char *caName = nullptr;
    bool fillState = false;
    const char *spanName =
        nullptr;  // DEVELOPMENT: the raw path's own states' name in the long span line (raw_span_marks.h)
    // what Core Animation's name for the state says its draws are (raw_ca_route.h CaRoute, raw_blur_twod.h BlurFunction)
    uint8_t caRoute = 0, blurFunction = 0;
    uint32_t probeGeneration = 0;
    uint8_t probeLines = 0, probeThinLines = 0;
    // raw_rotation.h: its fragment program has a turned variant (AssumeTurned), or reads a turned plane's axes with none; a
    // runtime record's turned variant's pipeline (a pack record's is one of `chosen`)
    bool turnedVariant = false, turnRefused = false;
    // the fragment slots its programs read two-plane RGB10A8_2P_XR10 textures at through their header pairs (PackPipeline)
    uint16_t xrSlots = 0;
    uint16_t writtenSlots = 0;  // the fragment slots its programs write textures at (raw_written_textures.h)
    Pipeline *turnedTwin = nullptr;
    // its vertex program writes memory (SPH DOES_GLOBAL_STORE), which a reach twin or bands would write again
    bool vertexStores = false;
    Pipeline *reachTwin = nullptr;  // Device::reachTwin's
    // its record's set 1 (raw_heap_tables.h, null: none) and the constant buffer slot each program reads it at (0: none); its
    // draws refused where the device had no memory for it (heapMissing), or where a tessellation program reads it, which
    // has no slot of its own here (tessReadsHeap)
    const HeapSet *heapSet = nullptr;
    uint8_t heapSlot[2] = {};
    bool heapMissing = false, tessReadsHeap = false;
    // its argument tables' words (raw_spill_table.h), vertex then fragment: the pack's or its runtime record's; null: none
    const uint32_t *spill[2] = {};
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
    uint32_t batchRecord = ~0u;
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
    // A level or slice of `image` (raw_attachment_target.h): what the pass's other views of the attachment are made of
    const Texture *image = nullptr;
    uint32_t level = 0, slice = 0;

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
    bool loaded = true, stored = true;  // an aspect is loaded, or stored: its contents outside the pass matter
};

struct PassDesc {
    ColorAttachment color[8];
    uint32_t count = 0;
    DepthStencilAttachment depthStencil;
    // renderTargetWidth / renderTargetHeight: the render area's bounds where nonzero (m2v render_pass.rs `area`: the
    // attachments' smallest extent, each axis limited by its target size)
    uint32_t width = 0, height = 0;
    // colors a draw of the pass binds as storage images (a pipeline's storage form): their memoryless backings take no
    // compression, which the surface unit cannot read through
    uint8_t storageMask = 0;
    // of those, the colors a layered record's draws bind as 2D arrays (raw_pack.h layeredRecord): a pitch one renders
    // through a block-linear backing, as a pitch header has no array form
    uint8_t arrayedStorageMask = 0;
    SamplePositions positions;
    // no depth/stencil attachment, and a draw's state tests depth or stencil (testsHiddenZeta): the pass renders over a hidden
    // Depth32Float_Stencil8 backing of its own, cleared to 1.0 and 0, as plain Metal tests such a draw
    bool hiddenZeta = false;
    // renderTargetArrayLength: every target binds this many layers from its attachment's slice (a 3D level's planes from its
    // depth plane), each cleared; 0 and 1 render one
    uint32_t layers = 1;
    // a pass of another one's clears alone: its Clear loads reach past the render area as that pass's do, which has a draw
    // call (raw_turned_pass.cpp)
    bool drawn = false;
};

// Whether a pass of colour attachments `color` draws RGB10A8_2P_XR10 in its two planes (raw_xr_pass.h): one of them an
// over-planes texture, each a texture held so; null, else what refuses the pass (another 550 beside one, no free index)
const char *xrPlanesPass(const Texture *const color[8], bool &planes);
// `p` with each such colour's planes as its targets (raw_xr_pass.h); null, else xrPlanesPass's refusal
const char *expandXrPlanes(PassDesc &p, Device &d);

class RenderEncoder {
public:
    void setPipeline(Pipeline *p)
    {
        pipeline_ = p;
    }

    void setBytes(int stage, uint32_t slot, const void *bytes, uint32_t length);  // stage 1 vertex, 2 fragment
    // the same for bytes already written at `va` (256-byte aligned, zero to the next 256 bytes; `cpu` the CPU's view)
    void setBytesAt(int stage, uint32_t slot, uint64_t va, uint32_t length, const uint8_t *cpu,
                    const Buffer *allocation = nullptr);
    void setBuffer(int stage, uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setBufferOffset(int stage, uint32_t slot, uint64_t offset);
    // A vertex buffer slot's bind stride (UINT32_MAX: none, MTLAttributeStrideStatic), which a state of a dynamic stride
    // reads (raw_amplification.cpp)
    void setVertexStride(uint32_t slot, uint32_t stride);
    // setVertexAmplificationCount:viewMappings: (raw_amplification.cpp): each draw after draws `count` amplifications, one
    // after another, amplification a through its mapping's viewport and render target offsets (0 past the mappings)
    static constexpr uint32_t kAmplificationMaps = SWITCH_GM20B_AMP_MAX;
    void setAmplification(uint32_t count, const uint32_t (&viewports)[kAmplificationMaps],
                          const uint32_t (&layers)[kAmplificationMaps]);

    // stage 1 vertex, 2 fragment; the one-argument forms bind the fragment stage
    void setTexture(int stage, uint32_t slot, const Texture *t);

    void setTexture(uint32_t slot, const Texture *t)
    {
        setTexture(2, slot, t);
    }

    // what the pass renders color `j` into (a memoryless attachment: its backing) and its render area, kept after end()
    const Texture *target(uint32_t j) const
    {
        return j < 8 ? pass_.color[j].texture : nullptr;
    }

    const Texture *depthTarget() const
    {
        return pass_.depthStencil.texture;
    }

    bool turned() const
    {
        return rot_.turned();
    }

    uint32_t areaWidth() const
    {
        return areaW_;
    }

    uint32_t areaHeight() const
    {
        return areaH_;
    }

    void setSampler(int stage, uint32_t slot, const Sampler *s)
    {
        const uint32_t at = stage == 1 ? 0 : 1;
        if (slot < kStageSamplers && samplers_[at][slot] != s) {
            samplers_[at][slot] = s;
            ++bindingRevision_;
            moved_.samplers[at] |= uint16_t(1u << slot);
        }
    }

    void setSampler(uint32_t slot, const Sampler *s)
    {
        setSampler(2, slot, s);
    }

    // Materializing a bound texture or sampler header must invalidate this encoder's descriptors.
    void invalidateDescriptors()
    {
        ++bindingRevision_;
        moved_.all = true;
    }

    void setScissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void setBlendColor(const float rgba[4]);
    void setDepthStencil(const DepthStencilState *s);  // null: Metal's default state
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

    // MTLDepthClipModeClamp: the state's clip control as NVK sets depth clamp without depth clip, from the next draw
    void setDepthClamp(bool clamp)
    {
        depthClamp_ = clamp;
    }

    // setDepthBias:slopeScale:clamp: of finite constant and slope factors (an infinite or NaN one m2v draws through the
    // viewport: refused before it gets here)
    void setDepthBias(float constant, float slope, float clamp);

    void setViewport(double x, double y, double w, double h, double znear, double zfar);
    // setViewports:count: and setScissorRects:count: (x, y, w, h, znear, zfar each; x, y, w, h each), `count` up to
    // kMaxViewports: index 0 as setViewport and setScissor take it, the others at the next draw
    void setViewports(const double (*viewports)[6], uint32_t count);
    void setScissors(const uint32_t (*rects)[4], uint32_t count);
    void barrier();  // memoryBarrier...: everything before is written and visible to what follows
    // false: this draw cannot be drawn exactly as m2v would (*why says what); the command buffer goes to m2v instead
    bool draw(uint32_t primitive, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance,
              const char **why);
    // drawIndexedPrimitives: `count` indices of `indexSize` bytes (2 or 4) at `offset` in `ib`, each `baseVertex` on
    bool drawIndexed(uint32_t primitive, uint32_t count, uint32_t indexSize, const Buffer *ib, uint64_t offset,
                     uint32_t instances, int32_t baseVertex, uint32_t baseInstance, const char **why);
    bool drawIndirect(uint32_t primitive, const Buffer *arguments, uint64_t offset, const char **why);
    bool drawIndexedIndirect(uint32_t primitive, uint32_t indexSize, const Buffer *indices, uint64_t indexOffset,
                             const Buffer *arguments, uint64_t argumentOffset, const char **why);
    // A tessellation state's draws (raw_tessellation.cpp): the encoder's factor buffer (null: none, whose patches are
    // culled) and scale, and patches of `controlPoints` each, from the draw's patch and control-point index buffers (null:
    // none)
    void setTessellationFactors(const Buffer *buffer, uint64_t offset, uint64_t instanceStride);
    void setTessellationFactorScale(float scale);
    bool drawPatches(uint32_t controlPoints, uint32_t start, uint32_t count, uint32_t instances, uint32_t baseInstance,
                     const Buffer *patchIndices, uint64_t patchIndexOffset, const Buffer *pointIndices,
                     uint64_t pointIndexOffset, const char **why);
    bool drawPatchesIndirect(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                             const Buffer *pointIndices, uint64_t pointIndexOffset, const Buffer *arguments,
                             uint64_t offset, const char **why);

    // An indirect command buffer's commands (raw_icb.h) [first, first + count) of `slots`, `slotWords` each, drawn with the
    // state they inherit (`primitive` one of theirs: icb::kPatchPrimitive where they are patches; `indexed`: some draw
    // indices), or with `range` the commands of the MTLIndirectCommandBufferExecutionRange the GPU reads there (`count` the
    // buffer's), once per amplification of the encoder's. `buffers`: what the commands read (index buffers, patch commands'
    // factors and indices). `written`: the GPU wrote some of the words, which the channel fetches after those writes land
    struct IcbExecute {
        const Buffer *slots;
        uint32_t first, count, slotWords;
        const Buffer *range;
        uint64_t rangeOffset;
        uint32_t primitive;
        bool indexed, written;
        const Buffer *const *buffers;
        size_t bufferCount;
    };

    bool executeSlots(const IcbExecute &x, const char **why);

    // An indirect command of patches as the CPU encoded it (Metal's drawPatches: and drawIndexedPatches: of a command,
    // `scale` its factor scale, 1 where the call names none), and its words (raw_icb.h icb::patches) as raw_tessellation.cpp
    // binds a draw's: false where Metal's validation stops it (*why)
    struct IcbPatches {
        uint32_t first, count, instances, baseInstance;
        const Buffer *factors;
        uint64_t factorOffset, factorStride;
        float scale;
        const Buffer *patchIndices;
        uint64_t patchIndexOffset;
        const Buffer *pointIndices;
        uint64_t pointIndexOffset;
    };

    static bool icbPatchDraw(const Device &dev, const IcbPatches &in, icb::PatchDraw &out, const char **why);
    // A counter sample where the pass starts: in its prologue, after the barrier to the work before
    void timestampAtStart(uint64_t va, counters::Point point);
    void end();

    void continuePass(uint8_t stores);
    // setVisibilityResultMode:offset: (raw_visibility.h): false where the raw path cannot take it
    bool setVisibility(uint32_t mode, const Buffer *results, uint64_t offset);
    // the Metal encoder's results to their offsets, at its end (a fused pass's too)
    void finishVisibility();
    // raw_reach.h: the pass a reach target's, at `step` frame pixels a texel, of a frame `width` x `height`: viewports and
    // scissors in the target's units, no depth/stencil state; and the state set last, its reach twin for the draws after
    void beginReach(uint32_t step, uint32_t width, uint32_t height);
    void takeReachTwin();

    // the state's vertex program writes memory, so its draws are not drawn again: the reach pass covers their viewports
    bool reachCovers() const
    {
        return reachCover_;
    }

    const char *reachRefusal() const
    {
        return reachWhy_;
    }

    void beginSplitPlan()
    {
        splitPlan_.clear();
    }

    void addSplitCandidates(Pipeline *pipeline, uint32_t draws, uint64_t pixels)
    {
        if (pipeline)
            if (const auto *source = __atomic_load_n(&pipeline->splitSource, __ATOMIC_ACQUIRE))
                splitPlan_.add(pipeline, source->rec, draws, pixels);
    }

    void finishSplitPlan()
    {
        splitPlan_.finish();
    }

private:
    friend class CommandBuffer;
#ifdef SWITCH_RAW_HARNESS_MARKS
    friend struct EncoderHarness;
    friend struct DeviceHarness;
#endif

    struct Bound {
        uint64_t va = 0;
        uint32_t size = 0;  // bytes from va (setBytes: its length)
        const Buffer *buffer = nullptr;
        uint64_t offset = 0;
        const uint8_t *cpu = nullptr;  // the bytes at va, where the CPU maps them
        uint64_t resourceVa = 0;
        // setBytes: the memory holding it was named, read, for the command buffer's jobs when it was bound (its
        // allocation, or the upload ring commit names), so no draw that only reads it names it again
        bool named = false;
    };

    CommandBuffer *cb_ = nullptr;
    Device *dev_ = nullptr;
    PassDesc pass_;
    Push p_;
    uint32_t reachStep_ = 0, reachW_ = 0, reachH_ = 0;
    uint32_t reachScissor_[4] = {};  // a reach draw's Metal scissor and viewport, each bounding the other
    double reachViewport_[4] = {};
    const char *reachWhy_ = nullptr;
    const Pipeline *reachTwin_ = nullptr;  // the twin takeReachTwin set
    bool reachCover_ = false;
    const Pipeline *extrasAtBegin_ = nullptr;  // set back by the pass's prologue, before its clears
    uint32_t *bodyStart_ = nullptr;
    std::array<counters::Stamp, 8> startStamps_{};
    uint8_t startStampCount_ = 0;

    using Body = PushBody;
    std::vector<Body> bodies_;
    SplitPassPlan splitPlan_;
    bool indirectGeometry_ = false;
    mme::DrawParams params_;  // what the channel holds: the context's zeros at a pass's start, put back at its end
    bool indirect(uint32_t primitive, bool indexed, const Buffer *arguments, uint64_t offset, const char **why);

    struct Tessellation {
        Bound buffers[TessBufferCount];
        uint32_t instanceStride = 0, scaleBits = 0x3f800000, indexed = 0;
        bool scaleValid = true;
    };

    Tessellation tess_;
    bool tessWordsDirty_ = true;
    uint32_t vertexStrides_[31] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                   UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t amplifications_ = 1, amplification_ = 0;
    uint32_t amplificationViewports_[kAmplificationMaps] = {}, amplificationLayers_[kAmplificationMaps] = {};
    bool amplifying_ = false, amplificationDirty_ = true;
    // what the amplify macros' scratch words hold (raw_mme.h), written before an amplified draw where they changed
    bool amplificationScratchDirty_ = true;
    bool amplificationMapsWide_ = false;  // a mapping's offset past the 16 bits of its scratch word
    mme::AmplificationWords heldAmplification_ = {~0u};

    // A draw under vertex amplification is one amplify macro call (raw_amplification.cpp), but where runs of its
    // primitives are drawn apart with barriers between: that draw is drawn once per amplification
    bool amplifies() const
    {
        return amplifications_ > 1 && !amplifying_;
    }

    bool runsPerAmplification(uint32_t primitive, uint32_t count, uint32_t instances, bool indexed);
    void amplificationScratch(Push &p, const Pipeline &pl);
    void amplifyCall(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                     uint32_t root, uint32_t baseInstance);
    // the root words an amplify call leaves: the last amplification's id and offsets, the count
    void heldAmplifyWords(const Pipeline &pl);

    // `draw` once per amplification, its id and offsets in the state's words
    template <typename Draw> bool amplified(const Draw &draw)
    {
        amplifying_ = true;
        bool drawn = true;
        for (uint32_t a = 0; drawn && a < amplifications_; a++) {
            amplification_ = a;
            amplificationDirty_ = true;
            drawn = draw();
        }
        amplifying_ = false;
        amplification_ = 0;
        amplificationDirty_ = true;
        return drawn;
    }

    // What of a draw's amplification and bind strides the state does not take (null: it takes them)
    const char *drawnWordsRefusal(const PackPipeline &r) const;
    // A vertex stream of the state's takes its bind's stride, and that stride is past the stream's 12-bit field
    bool bindStridePastStream(const PackPipeline &r) const;
    void drawnWords(const Pipeline &pl, uint32_t (&want)[32]);
    // The state's vertex streams at their binds (raw_vertex_streams.cpp): a stream's location and limit where its bind
    // moved, its format where its stride is its bind's, and its vertices [vFirst, vFirst + count) cleaned
    void bindStreams(Push &p, const Pipeline &pl, bool indexed, uint32_t vFirst, uint32_t count);
    void bindStream(Push &p, uint32_t stream, uint8_t slot, uint32_t stride, bool bindStride, bool indexed,
                    uint32_t vFirst, uint32_t count);

    uint32_t vertexStride(uint32_t slot) const
    {
        return slot < 31 && vertexStrides_[slot] != UINT32_MAX ? vertexStrides_[slot] : 0;
    }

    static Bound boundFrom(const Buffer *buffer, uint64_t offset);
    bool bindPatches(uint32_t controlPoints, const Buffer *patchIndices, uint64_t patchIndexOffset,
                     const Buffer *pointIndices, uint64_t pointIndexOffset, const char **why);
    Bound tessBuffer(uint8_t which) const;
    void controlWords(const PackPipeline &r, uint32_t (&want)[32]) const;
    // An execute of patch commands: the state prepare() drew with as their layout's scratch words; after them, the root
    // table's control words set back to what the encoder holds and set 0 built again, as the commands loaded both
    bool icbPatchLayout(const char **why);
    void icbPatchesDone();
    // The amplification `a`'s id into the root table of the state prepare() drew with, before each of an execute's passes
    void icbAmplification(uint32_t a);
    const Pipeline *prepared_ = nullptr;  // what the last prepare() drew with, and its set 0's address
    uint64_t preparedSet_ = 0;
    void reserve();
    void finishDraw(uint32_t vertices, uint32_t instances, bool indirect = false);
    void cutBatch();
    // the prepared draw's words, whole or in scissor bands (raw_draw_bands.h)
    struct Shortcut;
    void emitDraw(bool indexed, uint32_t first, uint32_t count, uint32_t instances, uint32_t root,
                  uint32_t baseInstance, const Shortcut &cut);
    uint64_t drawUnits(uint32_t instances) const;
    uint32_t drawBands(uint32_t vertices, uint32_t instances, uint64_t units) const;
    void noteCandidate(uint64_t units);
    bool measurePass(bool drained);
    uint32_t passDraws_ = 0;
    DrawTiming candidate_;  // the pass's first draw, measured at the pass's end when it stays the only one
    // whether the prepared draw of `pl` is drawn whole, never in bands: it amplifies, or may choose a viewport past 0 and so
    // its scissor, which bands do not narrow; and its words so, an amplify call where it amplifies
    bool drawnWhole(const Pipeline &pl) const;
    void wholeDraw(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                   uint32_t baseVertex, uint32_t baseInstance);
    Pipeline *pipeline_ = nullptr, *emitted_ = nullptr;
    void bindState(Push &p, const Pipeline &pl);
    bool measuringInline() const;
    bool bindsBySegment() const;
    const Pipeline *compatible_ = nullptr;
    uint32_t compatibleSampleMasks_[4]{};
    bool residualsPushed_ = false;  // the last draw's push words held SlotBindOffset residuals (RenderEncoder::prepare)
    PreparedDescriptors descriptors_;
    SpillUpload spillUploads_[2];  // each stage's argument table as the encoder uploaded it last (raw_spill_table.h)
    // a fragment argument table's dynamic words may have moved since its last upload: the blend colour, the depth and
    // stencil state and references, the viewport's depth range
    bool spillDirty_ = true;
    bool blendWords(const PackPipeline &r, const char **why);
    bool fillSpills(const Pipeline &pl, uint32_t moved, PreparedDescriptorWords &prepared, uint32_t *set,
                    const char **why);
    uint64_t bindingRevision_ = 0;

    // the bindings set 0's entries are built from that moved since the last draw's set (descriptors_)
    struct MovedBindings {
        uint32_t buffers[3] = {};
        uint32_t textures[2] = {};  // vertex, fragment (handleStage)
        uint16_t samplers[2] = {};
        bool all = true;
    } moved_;

    void bufferMoved(int stage, uint32_t slot)
    {
        ++bindingRevision_;
        moved_.buffers[stage] |= 1u << slot;
    }

    void textureMoved(uint32_t at, uint32_t slot)
    {
        ++bindingRevision_;
        moved_.textures[at] |= 1u << slot;
    }

    uint32_t movedEntries(const PackPipeline &rec) const;
    DescriptorBinding descriptorBinding_;
    Bound bufs_[3][31];
    const Texture *textures_[2][kStageTextures] = {};  // vertex, fragment (handleStage)
    const Sampler *samplers_[2][kStageSamplers] = {};

    const Texture *boundTexture(const PackEntry &e) const
    {
        return e.slot < kStageTextures ? textures_[handleStage(e)][e.slot] : nullptr;
    }

    const Sampler *boundSampler(const PackEntry &e) const
    {
        return e.sampler < kStageSamplers ? samplers_[handleStage(e)][e.sampler] : nullptr;
    }

    // what each vertex stream's location and limit registers hold (raw_vertex_streams.cpp)
    uint64_t streamVa_[kPackVertexStreams] = {}, streamLimit_[kPackVertexStreams] = {};
    uint64_t ibVa_ = 0, ibEnd_ = 0;
    uint32_t ibSize_ = 0;
    // the state a draw needs but its draw parameters, which its macro call writes (vertices [vFirst, vFirst + vCount) of
    // the stream are CPU-cleaned): the first vertex a draw of vertices starts at, or the base vertex an indexed one adds to
    // each index
    bool prepare(Pipeline *pl, uint32_t primitive, int32_t first, uint32_t vFirst, uint32_t vCount, bool indexed,
                 const char **why);
    // `pl`'s split twin where the draw is large enough for it to save time and its prologue can read what the draw binds
    Pipeline *splitTwin(Pipeline *pl, WrittenRegion::Analysis &analysis) const;
    uint32_t bindingFacts(const Pipeline &pl);
#if SWITCH_RAW_DIAGNOSTICS
    void logUniformTuple(const Pipeline &pl) const;
#endif
    const Pipeline *factsPipeline_ = nullptr;
    uint64_t factsRevision_ = 0;
    uint32_t facts_ = 0;
    // the twin's draw: its frontier bound and its prologue's launch recorded (`set`: the draw's set 0)
    void launchSplit(Push &p, const Pipeline &twin, const uint32_t *set);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    void captureSplit(const Pipeline &pl, WrittenRegion::Analysis &analysis, uint32_t primitive, uint32_t first,
                      uint32_t count);
#endif
    uint32_t scissor_[2] = {}, viewport_[8] = {};
    float depthRange_[2] = {0, 1};
    bool scissorDirty_ = true, viewportDirty_ = true;
    ViewportArray views_;
    void emitViewports(Push &p);
    DepthStencilDesc ds_;
    const DepthStencilState *dsState_ = &DepthStencilState::metalDefault();
    uint32_t stencilRef_[2] = {};
    bool dsDirty_ = true;
    ZcullPass zcull_;  // raw_zcull.h
    VisibilityQueries visibility_;
    const Pipeline *zcullProgram_ = nullptr;  // the state zcull_ last saw
    Texture *shaderDepth_ = nullptr, *shaderStencil_ = nullptr, *shaderStencilTemp_ = nullptr;
    uint8_t shaderTestsActive_ = 0;
    bool prepareShaderTests(const Pipeline &pipeline, const char **why);
    uint64_t rootTable(const PackPipeline &r, uint32_t k);
    void copyShaderTests(uint8_t flags, bool toAttachment);
    void endShaderTests();

    // the encoder's culling, winding and fill, and what the registers hold: the context's (Metal's defaults: no culling,
    // clockwise, fill) at a pass's start, put back at its end
    struct Raster {
        uint8_t cull = 0, winding = 0, fill = 0;
        bool bias = true;                                         // polygon offset on: a triangle topology's
        uint32_t biasConstant = 0, biasSlope = 0, biasClamp = 0;  // float bits
    };

    float depthBias_[3] = {};

    Raster raster_, rasterHeld_;
    bool depthClamp_ = false;
    uint32_t clipHeld_ = 0;        // the channel's SET_VIEWPORT_CLIP_CONTROL since the encoder's first bind
    bool clampRangeHeld_ = false;  // the viewport's clip depth range is a clamp's, clampRange_
    uint32_t clampRange_[2] = {};
    void writeRaster(Push &p, const Raster &want);
    // false: a depth clamp on a state whose registers set no clip control
    bool writeClipControl(Push &p, const Pipeline &pl, const char **why);
    void writeClipDepthRange(Push &p, const uint32_t range[2]);
    void unclampDepthRange(Push &p);
    Raster rasterFor(uint32_t primitive) const;
    uint32_t blendColor_[4] = {};  // Metal's default, (0, 0, 0, 0), until the encoder sets one
    bool blendColorDirty_ = true;
    uint32_t blendWords_[8] = {};
    bool blendWordsDirty_ = true;
    uint32_t topology_ = ~0u, restart_ = ~0u;
    uint32_t samplePasses_ = 1;
    AttachmentWrites attachmentWrites_;
    bool memoryless_[8] = {};
    bool hiddenZeta_ = false;  // pass_.depthStencil.texture is PassDesc::hiddenZeta's backing: the pass declares none
    // The hidden zeta: over the plane the colors' backings cover (once they are taken), of the pass's samples
    Texture hiddenZetaShape(const PassDesc &pass) const;
    const Texture *shadowOf_[8] =
        {};  // a shadow target's texture, where pass_ holds its pool image (CommandBuffer::render)
    // what m2v's DontCare loads answer (metal2vulkan clear_reach.rs: dont_care_loads, DrawnArea): whether the pass has a draw
    // call and one that draws, the colors its draws read, and the pixels they may write (scissor within viewport, each draw)
    bool called_ = false, drew_ = false, whole_ = false;
    uint32_t readBits_ = 0, areaW_ = 0, areaH_ = 0;
    // rung-2 forms' ROP blends over each colour so far (rule fetch.blend's layers), and whether SET_CT_SELECT holds a rung-3
    // form's one target in place of the pass's
    std::array<uint8_t, 8> ropLayers_ = {};
    bool narrowed_ = false;
    uint64_t drawPixels_ = 0;
    PixelBox drawBox_;
    uint8_t samples_ = 1;  // its attachments' samples a pixel
#if SWITCH_RAW_DIAGNOSTICS
    uint32_t prologueMark_ = SpanMarks::kNone;  // raw_span_marks.h: its prologue's mark, taken ahead of its draws'
    void markSpan(Push &p, uint32_t index, SpanKind kind);
#endif
    double vp_[4] = {};
    uint32_t sc_[4] = {};
    std::vector<std::array<uint32_t, 4>> rects_;
    // the draws that wrote a color since the last barrier, for the fetch barrier's region test (raw_region.h); the
    // indices of the indexed draw being prepared
    WrittenRegion written_;
    GeometryCache geometryCache_;
    std::array<bool, TriangleRuns::kMaxTriangles> runWinding_ = {};
    // colors the pass's first draw overwrites whole and opaquely: their clears inside the render area are not needed
    uint8_t covered_ = 0;
    using Runs = std::vector<std::pair<uint32_t, uint32_t>>;

    // raw_draw_shortcuts.cpp: the ROP shortcuts a draw takes (constant colour, a quad as one bounding-box triangle) and a
    // blur-chain pass drawn by the 2D engine, each behind its rule
    struct Shortcut {
        bool constant = false, bbox = false;
        uint32_t rgba[4] = {};
    };

    bool cpuStream(const Pipeline &pl, uint32_t first, FillStream &s) const;
    bool cpuIndices(uint32_t count, uint32_t out[6]) const;
    Shortcut shortcut(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count, bool indexed);
    void shortcutBegin(const Shortcut &s);
    void shortcutEnd(const Shortcut &s);
    bool blurTwod(const Pipeline &pl, uint32_t primitive, uint32_t first, uint32_t count, bool indexed);

public:
    // [x0, x1) x [y0, y1) of colour target 0 set to `rgba` (fillTexture:...): CLEAR_SURFACE over the region, with the ZBC
    // table's value where the target is compressed
    void clearRegion(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const float rgba[4]);

private:
    // `pl`, or its barrier twin where an attachment of this pass cannot be viewed as one of its storage images
    Pipeline *formFor(Pipeline *pl);
    Pipeline *fetchFreeFor(const Pipeline &pl);
    bool matrixConstants(const Pipeline &pl, const char **why);
    uint32_t colorTargets() const;
    StorageFallback storageFallback(const Pipeline &pl) const;
#if SWITCH_RAW_DIAGNOSTICS
    void probeEdges() const;
    void probeDraw(const Pipeline &bound, Pipeline &drawn, uint32_t primitive, int32_t first, uint32_t count,
                   bool indexed, const PixelBox &limit);
    void probeGlassDraw(const Pipeline &bound, const Pipeline &drawn, const PixelBox &limit);
    void glassMemSample(const Pipeline &bound, const Pipeline &drawn);
#endif
    const Runs *selfRuns(const Pipeline &pl, uint32_t primitive, uint32_t count, uint32_t instances, const char **why);
    const Runs *sequentialRuns(const Pipeline &pl, const FetchDraw &shape, Runs &runs, const char **why);
    const Runs *refuseFetch(const Pipeline &pl, FetchRefusal refusal, uint32_t primitive, uint32_t count,
                            uint32_t instances, const char **why);
    const uint8_t *drawIndices_ = nullptr;
    const Buffer *drawIndexBuffer_ = nullptr;
    bool drawIndexCpuSafe_ = false;
    uint32_t drawIndexSize_ = 0, drawIndexCount_ = 0;
    DrawGeometry geometry(const Pipeline &pl, uint32_t primitive, int32_t first, uint32_t count, const PixelBox &limit);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // the same draws under two other rules, counted only: one barrier per fetching draw after any color write (the rule
    // before attachmentWrites_), and the interlocked storage form (a fetched color read and written by the fragment shader inside
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

    // the pass's clears into `out`; the tiles its draws reach in tiles_ (both keep the last pass's capacity)
    void clears(std::vector<Clear> &out);
#if SWITCH_RAW_DIAGNOSTICS
    void glassMemReached(const std::vector<Clear> &cl);
#endif
    std::vector<Clear> initialClears_;
    std::vector<bool> tiles_;
    bool clearsSealed_ = false;

    // The pass's other headers of its attachments' memory (Device::viewTic: storage views, shader depth/stencil
    // copies): asked of the device once a pass, not under its lock at each draw's descriptors
    struct PassView {
        uint32_t ownTic;
        AttachmentView view;
        uint32_t tic;
    };

    std::array<PassView, 8> passViews_{};
    uint8_t passViewCount_ = 0;
    uint32_t passViewTic(const Texture &t, uint32_t word0);
    uint32_t passViewTic(const Texture &owner, const AttachmentView &v, const char **why);
    // The header a draw's fetch of attachment `a` binds: the storage form's view of word 0 `storage`, a 2D array of the
    // pass's layers for a record of the layered form (`layeredRecord`), or (storage 0) the input-attachment form's, which
    // NVK fetches at (x, y, Layer) (nvk_nir_lower_descriptors.c lower_load_input_attachment_coord); 0: none (`why`)
    uint32_t fetchTic(const ColorAttachment &a, uint32_t storage, bool layeredRecord, const char **why);

    Rotation
        rot_;  // the pass's turned plane (raw_rotation.h): its frame is what areaW_, areaH_ and the rectangles count in
    void emitSwizzle(Push &p, uint32_t word);

    Rect planeRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const
    {
        return rot_.rect({x0, y0, x1, y1});
    }

    Rect planeArea() const
    {
        return planeRect(0, 0, areaW_, areaH_);
    }
};

// Copy-engine work between passes (MAXWELL_DMA_COPY_A, as NVK's nouveau_copy_rect does it for pitch memory)
class BlitEncoder {
public:
    // `bytes` bytes from `src` to `dst` (vkCmdCopyBuffer)
    void copyBytes(uint64_t src, uint64_t dst, uint64_t bytes);
    // `bytes` bytes at `dst` set to `value` (fillBuffer:range:value:): the copy engine writing a constant, its whole words as
    // NVK's vkCmdFillBuffer (nvk_cmd_fill_memory_ce), the bytes before and after them one at a time
    void fillBytes(uint64_t dst, uint64_t bytes, uint8_t value);
    // `bytes` (whole words) at `dst` set to the 32-bit `word`
    void fillWords(uint64_t dst, uint64_t bytes, uint32_t word);
    // `ms`'s sample grid (2^sxLog2 x 2^syLog2 samples a pixel) halved into `into` by the 2D engine (raw_resolve_twod.cpp);
    // `complement`: through the grid's complement (rule resolve.exact), the grid complemented back after where `keep`
    void resolveTwod(const Texture &ms, uint32_t sxLog2, uint32_t syLog2, const Texture &into, uint32_t fmt2d,
                     bool complement, bool keep);
    // [x0, x1) x [y0, y1) of `t` set to `word` by the 2D engine's solid rectangle (rule clear.twod, raw_twod_fill.h)
    void fillTwod(const Texture &t, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t word);
    // `lines` rows of `width` pixels of `bpp` bytes, from `src` (row stride srcPitch) to `dst` (dstPitch) (vkCmdCopyImage and the
    // buffer<->image copies of linear images)
    void copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width, uint32_t lines,
                  uint32_t bpp);
    // `w` x `h` texels from (sx, sy) of plane sz of `src` to (dx, dy) of plane dz of `dst`, each pitch or block-linear (a 2D
    // texture's plane is 0) (vkCmdCopyImage)
    void copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                    uint32_t dy, uint32_t dz, uint32_t w, uint32_t h);
    // `w` x `h` texels from the origin of plane z of `t` (a chain level: chainLevelImage) each set to `fill`'s texel
    void fillTexels(const Texture &t, uint32_t z, uint32_t w, uint32_t h, const CopyFill &fill);
    // `w` x `h` texels between rows `pitch` bytes apart at `va` (both multiples of the texel's component, at most 4 bytes) and
    // (x, y) of plane z of `t`, pitch or block-linear: into `t` where `toTexture`, else out of it (vkCmdCopyBufferToImage,
    // vkCmdCopyImageToBuffer); the buffer's texels bufferUnit each. A Depth32Float_Stencil8 aspect's copy is NVK's
    // (nvk_remap_extract_aspect, insert_aspect): depth its texel's first word, stencil its fifth byte through t.stencilTemp
    void copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                          uint32_t h, bool toTexture, uint8_t aspect = kAspectWhole);
    // A planar texture's image from its surface's planes (Texture::planar): two remapped copies, the luma codes into green
    // (alpha 255), then the chroma pairs into red (Cr) and blue (Cb), each leaving the other components as they are
    void convertPlanar(const Texture &t);
    // generateMipmaps of a mipmapped texture (Texture::chain) on the 2D engine as surface format `fmt2d`: levels first + 1 ..
    // last from the one above, in place (fx_mip_route.h MipRun; `unproven` of its steps)
    void generate(Texture &t, uint32_t fmt2d, uint32_t first, uint32_t last, uint32_t unproven);
    // The next command reads the memory `read` names and writes the memory `write` names (null: none; a name is any pointer
    // that stands for one memory): where it reads or writes what an earlier command of this encoder wrote, or writes what one
    // read, it waits for those first (barrier), as Metal orders a blit encoder's commands on one resource
    void access(const void *read, const void *write);
    // Every copy and 2D blit before it completes before any after it starts: NVK's wait between transfers
    // (nvk_cmd_flush_wait_dep), as the copy engine starts a PIPELINED launch before the ones before it have landed
    void barrier();
    // A counter sample on the copy engine now; `afterCopies`: once the copies before it landed
    void timestamp(uint64_t va, bool afterCopies);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // hw_test_app's 2D-engine probes (raw_probe_ops.h): `op` reading `read` and writing `written`
    void probe(const ProbeOp &op, const Texture *read, const Texture &written);
#endif
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
    void fillRun(uint64_t at, uint32_t width, uint32_t height, uint32_t unit);  // a constant fill's launch
    LoopStream stream();
};

// Which encode-time predicate refused a draw (set beside *why; RawMetal.mm counts and logs it, r10-rawsweep diagnostics)
enum RefusalId : uint8_t {
    RefNone,
    RefIndexedRange,
    RefColorFormats,
    RefUboOffset,
    RefNoTic,
    RefDepthStencil,   // a: the state's depth format, b: the pass's
    RefSamples,        // a: the state's samples, b: the pass's, c: the state's shading passes
    RefGeometry,       // CPU ordering proof unavailable; requires an interlocked native program
    RefStorageView,    // a: the attachment, b: its format, c: its width (raw_storage_form.h)
    RefRotated,        // a: why (raw_rotated_pass.h RotatedRefusal), b: the rotation
    RefViewports,      // a: the encoder's viewport count
    RefWrittenTexture  // a: the slot, b: the bound format, c: the format the record's programs store
};

struct Refusal {
    uint8_t id = RefNone;
    uint32_t a = 0, b = 0, c = 0;
};

class CommandBuffer {
public:
    Refusal refusal;                              // the last refused draw's predicate and its values
    RenderEncoder *render(const PassDesc &pass);  // null: an attachment the raw path cannot render to
    // raw_turn_copy.h: each texel of `dst` from `src`'s at `map`, both of one texel size or depth format (a pass of the pack's
    // turn copy state; `src` given a texture header where it has none), over `dst`'s first width x height (0: all of an axis);
    // false: *why
    bool turnCopy(const Texture &dst, Texture &src, const TurnMap &map, const char **why,
                  const TurnMask *mask = nullptr, uint32_t width = 0, uint32_t height = 0);
    // raw_reach.h: the tiles of the `width` x `height` frame of `pass` that its draws reach, drawn again from `draws`; false: *why
    bool reach(const PassDesc &pass, uint32_t width, uint32_t height, DrawReplay &draws, PassReach &out,
               const char **why);
    // A block-linear image of `width` x `height` texels of `format` (turnCopyFormat's, or a depth format), the device's backing
    // of tag `tag` for this command buffer: what a turn copy moves texels through; null where none can be had
    Texture *turnScratch(uint32_t format, uint32_t width, uint32_t height, uint32_t tag);
    // raw_stencil_bits.h: `src`'s stencil drawn into `e`'s zeta of `format` (its stencil cleared to 0 at the pass's start, or
    // `zeroFirst`: zeroed by a draw where `map`'s source rectangle reaches, the rest kept), each pixel's from `src`'s texel at
    // `map` or its `pick` sample; `view` the stencil header's texture, read until e's end()
    bool stencilBits(RenderEncoder &e, const Texture &src, Texture &view, uint32_t format, const TurnMap &map,
                     StencilSample pick, const char **why, bool zeroFirst = false);
    // `src`'s stencil bound for a stencil program into a zeta of `format`: its stencil header's texture `view` at 0, or with
    // `depth` `src` itself at 0 (its depth, read as its float's bits) and `view` at 1; `view` read until e's end()
    bool bindStencilSource(RenderEncoder &e, const Texture &src, Texture &view, uint32_t format, bool depth,
                           const char **why);
    // A multisampled zeta's stencil resolve into one-sample `into` of its format, over (0, 0, width, height): a pass of its own
    bool stencilResolve(Texture &into, const Texture &from, StencilSample pick, uint32_t width, uint32_t height,
                        const char **why);
    // raw_zeta_colour.h: whether one draw of the colour ROP writes `into`'s stencil from `src` (its `pick` sample), and that
    // draw's pass: each texel from `src`'s at `map`, or its pick sample's stencil, over (0, 0, width, height) (0: all)
    bool zetaColourTakes(const Texture &into, const Texture &src, StencilSample pick);
    bool zetaColour(const Texture &into, const Texture &src, StencilSample pick, const TurnMap &map, uint32_t width,
                    uint32_t height, const char **why);
    BlitEncoder *blit();

    // Metal's predication unit ended (raw_predication.h)
    void endPredicationUnit()
    {
        predication_.endUnit();
        loops_ = {};
    }

    // `multisampled` resolved into `into` by the 2D engine (rules resolve.twod and resolve.exact, raw_resolve_rule.h);
    // false: not a sample grid the engine reads at 2:1, nothing recorded
    bool resolveTwod(const Texture &multisampled, const Texture &into, bool exact = false, bool keepSamples = false);
    // Counter samples behind every encoder ended so far, each on its own engine, in a segment of their own
    void timestamps(const counters::Stamp *stamps, size_t count);
    // CPU lines the GPU reads in this submission (CPU-written cached memory): cleaned to memory before it is submitted.
    // `invalidate`: also dropped from the CPU's caches (memory the GPU writes, so no dirty line lands on it later).
    void clean(const void *cpu, uint64_t bytes, bool invalidate = false);
    // Bounded legal draw/dispatch batches, in channel order, without CPU waits between their Execs.
    // false with `partial`: the kernel refused a later Exec after earlier ones were submitted; those completed before it
    // returned, and the next command buffer gets a new channel (the GPU's state after them is not what the device tracks).
    bool commit(uint32_t &seqno);

    // releases: one per signal, the payload releaseEvent put in the pushes for it (0: its job's end signals it)
    void sharedEvents(std::vector<TegraGM20BAbiEventUse> waits, std::vector<TegraGM20BAbiEventUse> signals,
                      std::vector<uint32_t> releases = {})
    {
        sharedWaits_ = std::move(waits);
        sharedSignals_ = std::move(signals);
        sharedReleases_ = std::move(releases);
        sharedReleases_.resize(sharedSignals_.size());
    }

    // A kernel event's signal here, after the work encoded so far: `payload` into its word at `va` (raw_event_release.h)
    void releaseEvent(uint64_t va, uint32_t payload);
    bool partial = false;
    bool admissionBlocked = false;
    // the commit's last job is quiet too (raw_completion_source.h quietEnd); every job before it always is
    bool quietEnd = false;
    // the writer promises its last job keeps (Device::promise), and whether a job of the commit carried them
    std::vector<uint32_t> promises;
    bool promisesKept = false;
    int nativeError = 0;
    // Why a job was refused when it was the resource set: its generation ended and the kernel refused to make it again
    // (SetView::refused). The command buffer fails with it.
    std::string setRefused;
    std::shared_ptr<ResourceReadyCompletion> resourceReadiness(std::function<void()> ready, int &result);
    const Texture *readTexture(const Texture *texture);
    void requirePitch(const Import *source);
    void invalidateNativeReads();
    void releaseNativeReads();

    void internalCompletion()
    {
        visibility_ = JobVisibility::Internal;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    void forceWholePrograms(bool force)
    {
        forceWholePrograms_ = force;
    }

    void splitProbe(SplitProbe probe)
    {
        splitProbe_ = probe;
    }

    // HW Test's bench: every draw the split plan would take if large enough is taken, without a plan
    void forceSplit(bool force)
    {
        splitForced_ = force;
    }

    // The split capture's records of this command buffer's draws (raw_split_capture.h), handed over once
    std::vector<SplitCaptureRecord> takeSplitCaptures()
    {
        return std::move(splitCaptures_);
    }

    // The batch records of this command buffer's split draws (raw_split_batch.h), one a prologue launch, until its commit
    const std::vector<SplitBatchRecord> &splitRecords() const
    {
        return splitRecords_;
    }

    // HW Test's bench: each prologue launch's frontier (its address, its bytes), until the next begin()
    std::vector<std::pair<uint64_t, uint32_t>> splitFrontiers() const
    {
        std::vector<std::pair<uint64_t, uint32_t>> out;
        for (const SplitLaunch &l : splits_)
            if (l.batchRecord < splitRecords_.size())
                out.emplace_back(splitRecords_[l.batchRecord].frontier, l.split->rec.frontierBytes);
        return out;
    }

    // hw_test_app's measurement of state segments: every state bound as the inline copy it was before them
    void inlineStates(bool inline_)
    {
        inlineStates_ = inline_;
    }

    const ProbeForms &probeForms() const
    {
        return forms_;
    }

    const ProbeTaken &probeTaken() const
    {
        return taken_;
    }

    // a multisampled attachment's resolve by the 2D engine (raw_probe_ops.h); false: none for its format or grid
    bool probeResolve(const Texture &multisampled, const Texture &into);
#endif
    // Nothing of it is on the GPU any more: the rings get back what it took, the root table's shadow what it was.
    void abandon();

    // A host import the submission reads or writes (a surface's, a no-copy buffer's), for the kernel's order with other
    // processes' GPU work on it (raw_kernel.h exec)
    void use(const Import *i, bool write)
    {
        if (i)
            surfaces_.add(i->bo.handle, write);
    }

    // Actual GPU addresses resolve through this connection's binding table in the addressed job ABI.
    void useAddress(uint64_t va, uint64_t bytes, bool write);

    // An MTLHeap an encoder used (useHeap): named once per command buffer, its jobs name what it holds at their commit
    void useHeap(const HeapMembers &heap)
    {
        if (std::find(heaps_.begin(), heaps_.end(), &heap) == heaps_.end())
            heaps_.push_back(&heap);
    }

    bool useMember(uint64_t va, uint64_t bytes, bool write, bool hostWritten);
    void use(const Buffer &buffer, bool write);
    void use(const Texture &texture, bool write);

    uint32_t draws = 0, passes = 0, blits = 0, bands = 0;
    uint32_t stamp = 0;  // its GPU time's slot (raw_stamps.h), for Device::gpuNanoseconds once it completed
    std::vector<DrawTiming> timings;  // its measured draws, which its commit hands the device (Device::learnCompleted)
    bool loopsLaid = false;           // a predicated loop's copies: the GPU writes the push ring

private:
    friend class Device;
    friend struct DeviceHarness;
    std::vector<TegraGM20BAbiEventUse> sharedWaits_, sharedSignals_;
    std::vector<uint32_t> sharedReleases_;
    friend class RenderEncoder;
    friend class BlitEncoder;
    friend class ComputeEncoder;
    Device *dev_ = nullptr;
    Predication predication_;
    LoopCopies loops_;  // a unit's loops, laid across its compute encoders
    PushBatches segments_;
    BatchWork batchWork_;
    uint64_t batchLimit() const;
    DirtyRanges dirty_;
    std::vector<Texture *> backings_;         // memoryless backings it uses
    std::vector<const HeapMembers *> heaps_;  // useHeap's, each kept alive by the facade's command buffer
    void nameHeaps();                         // lock_ held
    SurfaceList surfaces_;
    std::vector<SetUse> memberUses_;
    uint32_t memberRemovals_ = 0;
    bool memberHostWritten_ = false;
    UseMemo named_;  // (raw_use_memo.h)
    JobVisibility visibility_ = JobVisibility::Legacy;
    bool externalWrites_ = false;
    EventPublication publication_;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    bool forceWholePrograms_ = false, inlineStates_ = false;
    SplitProbe splitProbe_ = SplitProbe::Twin;
    bool splitForced_ = false;
    bool splitCapturing_ = false;  // the device's capture was armed at begin(): its draws may take a record
    std::vector<SplitCaptureRecord> splitCaptures_;

    // the encode's per-draw and per-bind counts, kept plainly and added to the device's at the next begin(): an atomic
    // add per draw for each was a measurable share of a debug build's draw
    struct DrawCounts {
        uint64_t uploadBytes = 0, segmentBinds = 0, segmentWords = 0, segmentSkips = 0, blendKillBinds = 0,
                 interlockDraws = 0, interlocked = 0, interlockedBlends = 0;
    } counts_;

    void foldCounts();
    ProbeForms forms_;
    ProbeTaken taken_;
    ProbeEngine lastEngine_ = ProbeEngine::None;  // the engine of the work encoded last, for forms_
#endif
#if SWITCH_RAW_DIAGNOSTICS
    Texture glassMemImages_[kGlassMemItems];               // the image each of the probe's rows is read from
    const Texture *glassMemSources_[kGlassMemItems] = {};  // and the texture a draw bound (alive until completion)
    int glassMemSlot_ = -1;                                // its ring slot once its reads are encoded
    void glassMemTail();
#endif
    std::map<const Texture *, const Texture *> nativeReads_;
    std::vector<NativeSurface *> nativeSurfaces_;
    std::vector<const Import *> pitchImports_;
    GeometryBudget geometryBudget_;
    GeometrySnapshotPool geometryPool_;

    struct GeometryRead {
        Bo bo;
        uint64_t offset, bytes;
        uint32_t access;
    };

    std::vector<GeometryRead> geometryReads_;
    // the buffers and backings whose CPU read the kernel refused in this command buffer: their draws take their
    // scissor without asking again
    std::vector<const Buffer *> geometryRefusedBuffers_;
    std::vector<uint64_t> geometryRefusedBackings_;
    bool acquireGeometry(const Buffer &buffer, const uint8_t *cpu, uint64_t bytes);
    bool geometryWritable() const;
    void releaseGeometry();
    RenderEncoder render_;
    BlitEncoder blit_;
    uint64_t pushStart_ = 0, uploadStart_ = 0;
    uint32_t rootPush_[32] = {};
    uint32_t restart_ = 0;                  // SET_DA_PRIMITIVE_RESTART_INDEX as the GPU will hold it
    uint32_t rtLayer_ = ~0u;                // and SET_RT_LAYER (~0: not known)
    DepthStencilRegs ds_;                   // and its depth/stencil registers
    const Pipeline *extrasLive_ = nullptr;  // and the state whose extras it holds (Pipeline::restore)
    HeldSegment heldSegment_;               // and the state segment its registers hold
    uint32_t segmentBinds_ = 0;             // its own (kSegmentBindsPerCommandBuffer)
    bool drawn_ = false;  // a draw was encoded (NVK resets the restart index at a command buffer's first)
    // the prologue's INVALIDATE_SHADER_CACHES_NO_WFI data word, which commit gives the INSTRUCTION bit when programs were placed
    // in the heap since the last submission that invalidated the instruction cache
    uint32_t *shaderInvalidate_ = nullptr;
    uint32_t prologueDwords_ = 0;
    // the split draws' prologues (RenderEncoder::launchSplit), launched in one compute segment after the submission prologue
    std::vector<SplitLaunch> splits_;
    std::vector<SplitBatchRecord> splitRecords_;
    SplitBatchPlanner splitBatches_;
    SplitReuse splitReuse_;
    // GPU work encoded before may have written a buffer: a prologue, run ahead of it, reads only what the CPU wrote
    bool buffersWritten_ = false;
    void segment(uint32_t *from, uint32_t *to);
    // An encoder's words from `start` as segments of the unit's open loops (raw_loop_copies.h), or plain ones
    LoopStream loopStream(Push &p, uint32_t *&start);
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
        std::lock_guard<raw::Mutex> l(channelLock_);
        return channelError_;
    }

    // the pipeline a draw of `pl` with binding facts `facts` (raw_pack.h PackAssumption) draws with (raw_variants.cpp)
    Pipeline *variant(Pipeline &pl, uint32_t facts);

    const Pack &pack() const
    {
        return pack_;
    }

    // `rec`'s state key where it is one of the pack's records, else null (a state the runtime made)
    const char *packKey(const PackPipeline &rec) const;

    // a storage form may view a compressed attachment (raw_storage_form.h storageReadsCompressed, or its probe's switch)
    bool storageCompressedOk() const
    {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (probeStorageCompressed.load(std::memory_order_relaxed))
            return true;
#endif
        return storageReadsCompressed();
    }

    const Kernel &kernel() const
    {
        return k_;
    }

    // A pack pipeline by its state key's FNV-1a (RawMetal.mm rawPipelineKey), else the record this process made for that
    // key (completed_; packHolds says which); null: neither.
    // caTransform: its vertex function is Core Animation's fixed_vert_l?h_spc (Pipeline::caTransform)
    Pipeline *pipeline(uint64_t keyHash, PackForm form = PackForm::Default, bool caTransform = false);

    // A new depth/stencil state's registers into the segment heap
    void placeSegments(DepthStencilState &state)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        state.place(segmentHeap_);
    }

    bool packHolds(const Pipeline &pl) const
    {
        return raw::packHolds(pack_, pl.rec);
    }

    bool fetchFreeReachable(uint64_t keyHash) const;
    Pipeline *placePipeline(uint64_t keyHash, PackForm form, bool caTransform);
    // The state's pack records for the variants its draws pick (PipelineVariantRecord), each placed, by name: null where it
    // cannot be (its programs find no room in the heap, or its storage or push metadata is not this runtime's)
    std::vector<std::pair<std::string, Pipeline *>> variantPipelines(uint64_t keyHash, bool caTransform);
    // A state the runtime made a record of (raw_record.h), drawn from then on as the pack's are; null: its programs find no room
    // in the heap or its samplers none in the header pool (`why`)
    Pipeline *addRecord(const RuntimeRecord &r, bool caTransform, std::string &why, bool *retry = nullptr);

    // Resources (thread safe). All textures are 2D, one level, one sample, pitch-linear.
    // pool memory, rows 256-byte aligned; `wc`: write-combined GPU-pool memory (a texture the CPU writes with replaceRegion:
    // no clean after), else (or full) pool memory
    // `tiled`: block-linear (TegraGM20BLayout.h) where its shape tiles and its format is a colour target's, for a texture the
    // CPU never touches
    bool createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc = false,
                       bool tiled = false, const ArenaAt *at = nullptr);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // createTexture's block-linear target over compressible memory of its own (probeCompressNext), under lock_
    bool probeCompressible(const tgm_layout &bl, Texture &t);
    // createOwnedZeta's one-sample zeta over compressible memory of its own (probeCompressNext), under lock_
    bool probeCompressibleZeta(uint64_t bytes, uint32_t kind, bool sampled, Texture &t);
#endif

    // Before the GPU first renders or stores into the IOSurface (Kernel::surfaceGpuWrites).
    bool surfaceGpuWrites(uint64_t id)
    {
        return id && k_.surfaceGpuWrites(uint32_t(id));
    }

    bool gpuPriority(uint32_t priority, uint32_t background)
    {
        if (!k_.priority(priority, background))
            return false;
        highPriority_.store(priority == 0, std::memory_order_relaxed);
        return true;
    }

    // Its channel is the kernel's high level (TegraGM20BSchedPolicy.h tgm_priority_level): the render server's, or a
    // process whose queues asked for GPU priority 0. Nothing preempts it, so its draws are never banded.
    bool compositorClass() const
    {
        return role_ == Role::RenderServer || highPriority_.load(std::memory_order_relaxed);
    }

    uint64_t bandBudgetNs() const
    {
#if SWITCH_RAW_DIAGNOSTICS
        return bandBudgetNs_.load(std::memory_order_relaxed);
#else
        return TGM_APP_BAND_NS;
#endif
    }

    bool hasGpuPriority() const
    {
        return (k_.info().flags & TegraGM20BInfoPriority) != 0;
    }

    const TegraGM20BAbiInfo &kernelInfo() const
    {
        return k_.info();
    }

    std::shared_ptr<SharedEvents> sharedEvents()
    {
        return k_.sharedEvents();
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
    bool createStandIn(Texture &t, const ArenaAt *at = nullptr);
    // The pack's conversion state of an extended-range texture's stand-in, null if the pack has none
    Pipeline *xrPipeline(XrForm form, bool twoPlanes = false);
    // The pack's resolve state into a target of `format` (raw_samples.h resolveState), null if the pack has none
    Pipeline *resolvePipeline(uint32_t format, uint32_t samples);
    // The pack's state that draws a chain level of `format` from the level above (raw_mip.h mipState), null if the pack has none
    Pipeline *mipPipeline(uint32_t format);
    Pipeline *depthResolvePipeline(uint32_t format, uint32_t filter);  // raw_samples.h depthResolveState
    Pipeline *stencilBitsPipeline(uint32_t format, uint32_t samples, StencilSample pick);    // raw_stencil_bits.h
    Pipeline *stencilColourPipeline(uint32_t format, uint32_t samples, StencilSample pick);  // raw_zeta_colour.h
    // raw_zeta_colour.h: the chip fact, or a debug build's probe asking for the route
    bool zetaColourTaken() const;
    // raw_reach.h: `pl` drawing its geometry into a reach target; null: *why
    Pipeline *reachTwin(Pipeline &pl, const char **why);
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
    // RGB10A8_2P_XR10 over a two-plane surface, its planes at `word` and `alpha` (offsets in the allocation, with their
    // pitches), held in them (raw_xr_pass.h, Texture::xrPlanes): the planes imported as luma and chroma; false with *why
    // where a plane cannot be one
    bool createXrPlaneTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, uint64_t word,
                              uint32_t wordPitch, uint64_t alpha, uint32_t alphaPitch, uint32_t width, uint32_t height,
                              Texture &t, const char **why);
    // A memoryless RGB10A8_2P_XR10 texture's planes, memoryless too (Texture::xrPlanes)
    bool createXrMemoryless(Texture &t);
    // The pack's stand-in states: the planes' codes into the stand-in, or (`store`) the stand-in into the planes
    Pipeline *standInPipeline(bool store);
    // The pack's store state of a YCbCr texture's luma or chroma plane of `store`'s target (pack/ycbcr_store.metal): of 8-bit,
    // 16-bit or packed words, its steps through the sRGB encode or not
    Pipeline *ycbcrStorePipeline(bool chroma, const YcbcrStoreShape &store);

    // pack/srgb_encode.h, which the _srgb store states read at kSrgbEncodeSlot (raw_ycbcr_store.h)
    const Bo &srgbEncodeTable() const
    {
        return srgbEncode_;
    }

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
    // Memoryless (a pass gives it a backing of the pass's layers); the depth/stencil formats among them, as zeta
    // (Texture::zeta). `layers`: a 2D array's or cube's, of one sample
    bool createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t, uint32_t samples = 1,
                          uint32_t layers = 1);
    // A stored depth/stencil texture of one sample over zeta memory another driver of the process keeps (NVK's image: one
    // allocation both drivers render, sample and copy in place), laid out as `m` says, with a texture header of its depth where
    // `sampled`; false where it is not a layout the raw path renders (*why says which way). A 2D array or cube of `layers`
    // layers, layerBytes apart, is rendered alone (no header)
    bool createZetaOver(const ZetaMemory &m, uint32_t width, uint32_t height, uint32_t format, bool sampled, Texture &t,
                        const char **why, uint32_t layers = 1);
    // A stored multisampled colour texture (raw_multisample.cpp): the persistent form of a memoryless one's block-linear
    // backing, in memory of its own, which m2v's twin (NVK's image of the same descriptor) lies over in place
    bool createStoredMultisample(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                                 const ArenaAt *at = nullptr);
    // A stored depth/stencil texture of `samples` samples in zeta memory of its own (raw_multisample.cpp), with a header
    // where `sampled`; a one-sample 2D array or cube of `layers` layers, layerBytes apart, has none (as createZetaOver's).
    // NVK views no zeta memory it did not make: a multisampled one never has an m2v twin, a one-sample one only once a
    // range replays on m2v (moveOwnedZeta).
    bool createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, bool sampled, Texture &t,
                         uint32_t layers = 1);
    // The same with a header always (texture2d_ms reads it, and a depth resolve's pass samples it): a multisampled one,
    // or one of a single sample in an arena (an MTLHeap's, whose depth is not NVK's image)
    bool createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                         const ArenaAt *at = nullptr);
    // A one-sample owned zeta's texels, every layer, copied into NVK's image `m` (its m2v twin's, made for a replay), the
    // texture over that image from then on. Its own memory and header stay until it is destroyed: the copy and the work before it may
    // still use them. False (*why): the texture keeps its own memory.
    bool moveOwnedZeta(Texture &t, const ZetaMemory &m, const char **why);
    // A Depth32Float_Stencil8 texture's stencil temp (Texture::stencilTemp), made on its first stencil copy
    bool stencilTemp(Texture &t);
    // A 1D texture: NIL's 1D image, one block-linear row (y_log2 0) under a ONE_D header, in block-linear memory the CPU
    // writes through its tiled view (a 1D header takes no pitch layout)
    bool createLine(uint32_t width, uint32_t format, Texture &t, const ArenaAt *at = nullptr);
    // A 3D texture of `levels` levels: NIL's 3D image (fx_image.h) under a THREE_D header. `view2d`: one plane a block, as m2v
    // makes a 3D render target (2D-array compatible), so its twin over the same memory reads the same texels
    bool createVolume(uint32_t width, uint32_t height, uint32_t depth, uint32_t format, bool view2d, Texture &t,
                      uint32_t levels = 1, const ArenaAt *at = nullptr);
    // A 2D array, cube or cube array of `levels` levels: NIL's image of `layers` layers (fx_image.h) under a header of `type`
    // (fx::kTexTwoDArray, kTexCube, kTexCubeArray)
    bool createArray(uint32_t width, uint32_t height, uint32_t layers, uint32_t format, uint32_t type, Texture &t,
                     uint32_t levels = 1, const ArenaAt *at = nullptr);
    // A mipmapped 2D texture of `levels` levels: level 0 of its block-linear chain (Texture::chain), its header the chain's.
    // False: not one the raw path keeps (not a render target format, extended range, no memory).
    bool createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t,
                          const ArenaAt *at = nullptr);
    // A block-compressed 2D texture (or 2D array, `type`) of `levels` levels and `layers` layers in block-linear memory of its
    // own: NIL's image (fx::blockChainLayout), which the copy engine and the CPU (in its view of kind-0xFE memory) address
    bool createBlockTexture(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, uint32_t format,
                            uint8_t type, Texture &t);
    // A block-compressed 2D texture (or 2D array) over the caller's `bytes` (newTextureWithBytesNoCopy), already laid out as
    // `layout` (fx::blockChainLayout of its texel size, levels and layers), its pages cleaned to memory. At the layout's
    // alignment (raw_block_base.h) the pages are imported under kind 0xFE with a block-linear header of the texel size over
    // them; off it, the texture is a pool image of the layout over which the caller's pages, imported pitch, are copied
    // (callerPages: copyCallerPages fills it). False: not a block-compressed format, `length` short of the layout, or the
    // import, image or header refused.
    bool createBlockTextureOver(void *bytes, uint64_t length, uint32_t width, uint32_t height, uint32_t format,
                                const fx::MipChain &layout, Texture &t);
    // The copy engine's copy of a createBlockTextureOver texture's caller's pages into its image, ahead of the work after it
    // (TextureTransfers::copyBytes); `owner` held until it completed. True with nothing to copy (the texture is the pages).
    bool copyCallerPages(const Texture &t, std::shared_ptr<void> owner);
    // A shared texture over its IOSurface (raw_surface_image.h): the surface's pages imported under its ID and bound 0xFE,
    // the texture over them as the private one of the same image (createMipTexture, createArray, createVolume,
    // createStoredMultisample, createLine, createBlockTextureOver) lies in memory of its own. False: a Plain image, an
    // allocation short of the layout, a base off a page, or the import or header refused.
    bool createSurfaceImage(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                            const SurfaceImage &image, uint32_t width, uint32_t height, uint32_t format, Texture &t);

    // Header `tic`'s eight words as the texture unit reads them
    const uint32_t *headerWords(uint32_t tic) const
    {
        return reinterpret_cast<const uint32_t *>(tic_.cpu + 32 * tic);
    }

    // The texture header of level `level` of mipmapped texture `t`'s chain alone, as NVK's view of that level (a pass into
    // the level fetches its color through it); 0: none
    uint32_t levelTic(Texture &t, uint32_t level);
    // The header a fetch from `part` (raw::attachmentTarget of level `level`, slice `slice` of `t`, maybe in a format view's
    // format) reads: levelTic's for a 2D chain's level in t's format, else attachmentHeader's, made on first use and freed
    // with t's header, kept in views_ as viewTic's are; 0: none
    uint32_t attachmentTic(Texture &t, const Texture &part, uint32_t level, uint32_t slice);
    // A header of plain texture `t`'s memory whose word 0 is `word0` (raw_view.h: m2v's view of it in another format
    // of the same texel size; a storage view of a multisampled texture spans its sample grid), made on first use and freed
    // with t's header; 0: no header slot left. The encoding thread's: what it answered is kept there and answered again
    // with no lock (raw_view_cache.h, keyed as viewTics_)
    uint32_t viewTic(const Texture &t, uint32_t word0, ViewAccess access = ViewAccess::Texture);
    // The header of view `v` of `owner`, an attachment's image (v.part) or the attachment itself (raw_attachment_view.h),
    // made on first use and freed with owner's header; 0: none (`why`)
    uint32_t attachmentViewTic(const Texture &owner, const AttachmentView &v, const char **why);
    // `t` with a texture header (a depth/stencil texture made only to be rendered gets one, a turn copy reading it); false: no
    // header slot left
    bool sampleable(Texture &t);

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
        std::unique_lock<raw::Mutex> l(lock_, std::try_to_lock);
        return l.owns_lock();
    }

    void destroyTexture(Texture &t);
    // Private render targets on compressed memory (raw_private_comp.h): made when compressed memory of the size is ready,
    // moved to plain memory before anything names its pages through another binding (the caller serialises submissions
    // and has no command buffer open)
    bool createCompressedTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t kind, Texture &t);
    bool privateCompressed(const Texture &t);
    bool plainFromCompressed(Texture &t);

    uint64_t privateMade() const
    {
        return privateMade_.load(std::memory_order_relaxed);
    }

    uint64_t privateTransitions() const
    {
        return privateTransitions_.load(std::memory_order_relaxed);
    }

    TextureTransferResult transferTexture(const Texture &texture, uint32_t level, uint32_t slice,
                                          const uint32_t origin[3], const uint32_t size[3], void *bytes, uint64_t row,
                                          uint64_t image, bool write, std::shared_ptr<void> owner);
    // `wc`: from the write-combined GPU pool (Buffer::wc), else (or full) pool memory as before
    bool createBuffer(uint64_t length, Buffer &b, bool wc = false);

    // The write-combined chunks held (non-dedicated) and their bytes (test/poolcheck.cpp: they follow what is in them)
    uint32_t wcChunksHeld();
    uint64_t wcChunkBytesHeld();
    // The free bytes of the block-linear chunks (test/poolcheck.cpp: a chunk's worth kept ahead)
    uint64_t blChunkFreeBytes();

    // the shader heap's bound bytes of host memory (the programs copied so far, in steps)
    uint64_t heapBytes() const
    {
        return heapBound_ - heapCopiesAt_;
    }

    // the pack's and the shader compiler's store's pages bound in the program window, where larger programs run in place
    uint64_t heapInPlaceBytes();

    // Where the shader compiler's program slots are, as this process maps its store (RawComplete.mm): a slot's bytes found
    // there in place, with that mapping's first byte and size, else null. A copied slot of a page or more is looked up
    // there first and runs in place where it is found. Null: none.
    using SlotLookup =
        std::function<const uint8_t *(const uint8_t *slot, uint32_t size, const uint8_t *&mapping, uint64_t &bytes)>;
    void runSlotsFrom(SlotLookup lookup);

    // The CPU mappings of the device's tables and rings (each counts whole in the task's IOKit footprint)
    uint64_t controlMappedBytes();

    // The GPU memory the device's connection allocated and holds
    uint64_t allocatedBytes() const
    {
        return k_.allocatedBytes();
    }

    bool createBufferNoCopy(void *bytes, uint64_t length, Buffer &b);
    // An IOSurface's pages as a buffer of `length` bytes (newBufferWithIOSurface:), sharing its textures' import. False
    // where the kernel refuses the import, or keeps the surface block-linear (`tiled`): its bytes are no pitch buffer then
    bool createSurfaceBuffer(uint64_t id, void *base, uint64_t bytes, uint64_t length, const void *owner, Buffer &b,
                             bool &tiled);
    bool createControlBuffer(uint64_t length, Buffer &buffer);
    bool hostAccess(const Buffer &buffer, bool write, HostAccess &access);
    bool hostAccess(const Texture &texture, bool write, HostAccess &access);
    bool hostAccess(const Buffer &owner, const void *cpu, uint64_t bytes, bool write, HostAccess &access);
    bool hostAccess(const Texture &owner, const void *cpu, uint64_t bytes, bool write, HostAccess &access);
    CpuCompletion captureCpuCompletion(uint32_t seqno, uint64_t generation, const std::vector<const Buffer *> &buffers);

    // A job of a channel made again since keeps nothing in the kernel to maintain its ranges against: Stale, so its buffers
    // stay GPU-dirty and their next CPU access acquires them, never a failure of a job that completed
    CpuCompletionResult completeCpu(const CpuCompletion &completion)
    {
        const auto retired = [&] {
            return completion.valid && !completion.ranges.empty() && completion.generation != channelGeneration();
        };
        if (retired())
            return CpuCompletionResult::Stale;
        const CpuCompletionResult result = k_.completeCpu(completion);
        return result == CpuCompletionResult::Failed && retired() ? CpuCompletionResult::Stale : result;
    }

    void destroyBuffer(Buffer &b);

    // MTLHeaps (raw_arena.cpp): an arena is one GPU VA reservation bound whole to host pages of its own, under
    // TGM_LAYOUT_KIND for a private heap's block-linear textures or pitch for a shared heap's textures and buffers, its
    // resources placed in it (raw_heap.h). Purged, its pages go back and its VA and CPU range stay reserved, so restoring it
    // binds fresh pages where its resources' addresses and texture headers already point.
    // A kind of memory the arena's own mapping is not (a private heap's pitch, a shared heap's block-linear, a depth
    // format's) is a view of its one BO, bound under that kind at the first texture that takes it: a job naming the arena
    // names every view.
    bool createArena(uint64_t bytes, bool blockLinear, uint32_t &arena, bool explicitPlacement = false);
    void destroyArena(uint32_t arena);  // once its resources are gone; an m2v view's lease keeps it until its last drop
    bool arenaTexture(uint32_t arena, uint32_t width, uint32_t height, uint32_t format, Texture &t,
                      std::optional<uint64_t> offset = {});
    // Any kind the device makes (TextureAsk), made as its create* makes it, in the arena; an extended-range one's stand-in
    // there too
    bool arenaTexture(const ArenaAt &at, const TextureAsk &ask, Texture &t);
    // What `ask` takes of an arena (its stand-in's after it, at the stand-in's alignment); false: the device makes no such
    // texture. A placement heap's offset is aligned to `align`.
    static bool footprint(const TextureAsk &ask, uint64_t &bytes, uint64_t &align);
    bool arenaBuffer(uint32_t arena, uint64_t length, Buffer &b, std::optional<uint64_t> offset = {});
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
    bool setArenaVolatile(uint32_t arena, bool isVolatile);  // whether its pages had gone (raw_arena_purge.h)
    void purgeArena(uint32_t arena, const std::atomic<uint32_t> &committed);
    void purgeArenas(const std::atomic<uint32_t> &committed);
    bool arenaPurged(uint32_t arena);
    // Fresh pages where it was (their contents zero); false: no memory, it stays purged
    bool restoreArena(uint32_t arena);
    // The idle give-back of the arenas kept non-volatile: as purgeArenas judges a purge (nothing in flight or committed, no
    // m2v view), the ranges no resource is placed in go back (raw_arena_trim.h); their contents stay
    void trimArenas(const std::atomic<uint32_t> &committed);

    uint64_t arenaBytes();  // bound: purged arenas' are not
    // Of arenaBytes, what their resources are placed in (the rest holds no resource: what an idle give-back could return
    // of a heap kept non-volatile) and what is in heaps made volatile
    void arenaUse(uint64_t &placed, uint64_t &volatileBytes, uint64_t *givenBack = nullptr);

    uint32_t purgedArenas() const
    {
        return purgedArenas_.load();
    }

    // The memory an m2v view of t (a pool or write-combined texture) or b uses in place, one hold taken for the caller (raw_alias.cpp).
    // False: nothing to alias (memoryless, a view's memory), or the connection is not NVK's.
    bool alias(Texture &t, Alias &a);
    bool alias(Buffer &b, Alias &a);
    // [va, va + bytes) of an arena whose placement there was given back (makeAliasable): the lease holds the arena, never the
    // placement, which a resource placed since may own
    bool aliasArenaRange(uint32_t arena, uint64_t va, uint64_t bytes, Alias &a);
    // The surface's import, made now if no texture of the raw path holds one: an m2v import of the surface is this one, aliased.
    bool aliasSurface(uint64_t surfaceId, void *base, uint64_t bytes, const void *surface, Alias &a);
    static void holdLease(void *lease);
    static void dropLease(void *lease);
    bool createSampler(const SamplerDesc &d, Sampler &s);
    void destroySampler(Sampler &s);

    // m2v's handle tables and heap (raw_heap_tables.h): texture header `tic` now names `texture`, sampler header `tsc`
    // `sampler` (null: none). False: no memory for the tables.
    bool heapTexture(uint32_t tic, const HeapTexture *texture);
    bool heapSampler(uint32_t tsc, const HeapSampler *sampler);
    // A texture or sampler whose gpuResourceID was asked for, named in the tables until it is forgotten as it goes. A sample
    // through a texture's ID reads it as a slot binds it: its stand-in where it has one, none for a planar texture (its
    // samplers take its address map, which no argument buffer's sampler carries). A sampler of pixel coordinates samples
    // through `twin`, made here with normalized ones, since m2v's program scales such a sampler's coordinates itself.
    static HeapTexture heapTextureOf(const Texture &t, const HeapTextureFacts &facts, uint32_t flags);
    bool nameTexture(const Texture &t, const HeapTextureFacts &facts, uint32_t flags);
    void forgetTexture(const Texture &t);
    bool nameSampler(const Sampler &s, uint32_t word, Sampler &twin);
    void forgetSampler(const Sampler &s, Sampler &twin);

    // Which texture an image array of a heap holds (the facade's: the bridge's switch_nmb_heap_holds), before any ID is named
    void heapHolds(HeapHolds holds)
    {
        heapHolds_ = holds;
    }

    // The first pool chunk made now (its pages wired and imported), off the render thread's first texture (raw_metal_prepare),
    // and the pool's grower started: the next chunk of the pool, of block-linear images or of a zeta format is made on its own
    // thread once the free bytes of that kind's shared chunks fall below a chunk's worth, so the render thread's allocations
    // (a pass's memoryless backings among them, made at commit) find room (Stats::poolStalls counts the ones that had to grow
    // it).
    void warm();

    // Submission. begin() opens the one command buffer (the caller serialises encoding and commit); null: no channel.
    CommandBuffer *begin();

    uint64_t channelGeneration() const
    {
        return generations_.current();
    }

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
        return fence_ == &kNoFence ||
               (int32_t(*fence_ - seqno) >= 0 && semanticFence(seqno).status != SemanticFenceStatus::Pending);
    }

    // In the kernel, on the fence interrupt. False: the submission failed, the channel died with it in flight (reopen).
    void keepFailure(uint32_t seqno);

    bool wait(uint32_t seqno)
    {
        return wait(seqno, channelGeneration());
    }

    bool wait(uint32_t seqno, uint64_t generation)
    {
        if (channelGeneration() != generation)
            return retiredWait(seqno, generation);
        const bool success = done(seqno) && semanticFence(seqno).status != SemanticFenceStatus::Superseded
                                 ? !lost(seqno)
                                 : waitFence(seqno, generation);
        if (channelGeneration() == generation)
            return success;
        return retiredWait(seqno, generation);
    }

    // A completed command buffer's GPU time from its stamps; false when its slot holds a later one's
    bool gpuNanoseconds(uint32_t stamp, uint64_t &ns) const;
    // a committed command buffer's measured draws, kept until the GPU has run them (raw_band_rates.h)
    void noteCommittedTimings(std::vector<DrawTiming> &timings);
    // the measured draws the GPU has run, each teaching its state's rate: every command buffer's completer calls it
    void learnCompleted();
#if SWITCH_RAW_DIAGNOSTICS
    SpanMarks spans_;
    // The render server's completed command buffer's spans of kLongSpanNs or more (raw_span_marks.h), which the facade logs
    // and notes to the kernel for HW Test ui.launch_hitch
    std::vector<StateSpan> longSpans(uint32_t stamp);

    int kernelDebugCall(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
    {
        return k_.debugCall(method, in, inSize, out, outSize);
    }
#endif
    // Where the command buffer of `stamp` records a loop that would run past its copies, and once it completed which
    // loop did (the stamp page is volatile to the GPU: the record needs no L2 flush)
    uint64_t loopRecordVa(uint32_t stamp) const;
    LoopContinue loopContinue(uint32_t stamp) const;

    bool clockAnchors(const volatile TegraGM20BAbiClockAnchors *&table, uint64_t &gpuVa)
    {
        return k_.clockAnchors(table, gpuVa);
    }

    bool lost(uint32_t seqno) const
    {
        return killed(seqno);
    }

    // A kill record's error the device writes itself (waitFence): the kernel kept refusing to wait on a channel it called
    // healthy
    static constexpr uint32_t kWaitRefused = 0x100;
    static constexpr uint32_t kDependencyFailed = 0x101;

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

    // The index fetch reads index 0 past SET_INDEX_BUFFER_C/D's limit (kits/SwitchGM20B caps row 3D-B11 at chip grade,
    // which the facade reads at open): a draw past its buffer's indices binds the buffer with its limit, no padded copy
    bool indexZeroPastLimit = false;

    // GPU-side order with the process's other driver (NVK's channels), through the kernel's Order (TegraGM20BInfoOrder; false:
    // it has none). orderAfterOthers: this channel's next submission starts with an acquire on what they submitted so far;
    // orderOthersAfter: their next submissions start with one on everything this channel submitted. The kern_return_t (0:
    // queued; kOrderNoChannel: no channel to order; else the kernel refused it and the caller orders on the CPU); `waits`:
    // the acquires queued. Called where submissions are (serialised).
    bool canOrder() const
    {
        return k_.canOrder();
    }

    // A channel the kernel killed is made again first (between command buffers, as begin() would), so one GPU fault does not
    // leave the process ordered on the CPU for good
    int orderAfterOthers(uint32_t &waits)
    {
        if (!ensureChannel())
            return kOrderNoChannel;
        for (int attempt = 0;; attempt++) {
            if ((contextless_ || k_.lost(channel_)) && !reopen())
                return kOrderNoChannel;
            const int kr = k_.order(channel_, TegraGM20BOrderAfterTask, 0, waits);
            if (kr != kIOReturnNotResponding)
                return kr;
            if (attempt)
                return kOrderNoChannel;
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

    // A writer promise (TegraGM20BJobABI.h method 34) of the device's channel for the IOSurface imports `handles`, kept by
    // the job that names it (CommandBuffer::promises) or cancelled
    bool promise(const std::vector<uint64_t> &handles, uint32_t &id)
    {
        return ensureChannel() && k_.promise(channel_, handles, id) == KERN_SUCCESS;
    }

    void cancelPromise(uint32_t id)
    {
        (void)k_.cancelPromise(id);
    }

    // TegraGM20BJobABI.h method 40 on this device's channel: the kern_return_t
    int eventWord(uint64_t handle, uint64_t &address, uint64_t &base, uint32_t &floor)
    {
        if (!ensureChannel())
            return kIOReturnNotReady;
        return k_.eventWord(handle, channel_, address, base, floor);
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint64_t jobsSubmitted() const
    {
        return k_.jobsSubmitted();
    }

    // HW Test's bench: `bytes` of the upload ring at `va` as the GPU left them (the ring is GPU-volatile); false outside it
    bool readUpload(uint64_t va, void *out, uint32_t bytes) const
    {
        if (va < upload_.bo.va || va + bytes > upload_.bo.va + upload_.bo.bytes || !upload_.bo.cpu)
            return false;
        memcpy(out, upload_.bo.cpu + (va - upload_.bo.va), bytes);
        return true;
    }

    uint64_t signalsReleased() const
    {
        return k_.signalsReleased();
    }

    // Of those, the ones released without the L2 flush: nothing host-visible written since the last flush, on a kernel
    // with addressed jobs (exactJobs)
    uint64_t signalsUnflushed() const
    {
        return unflushed_.load(std::memory_order_relaxed);
    }

    bool exactJobs() const
    {
        return k_.exactJobs();
    }
#endif

    // A job of no pushes that carries event waits and signals (at most TegraGM20BMaxJobEvents together): the kernel makes the
    // channel acquire each wait's producer, and releases the signals once every earlier submission of this channel and what
    // orderAfterOthers queued for it completed. Called where submissions are (serialised). `generation`: the channel the
    // caller ordered the job after (orderAfterOthers) and checked its waits' producers on, 0 for none. A channel made again
    // since has neither, so the job is not submitted: false with kIOReturnNotResponding, and the caller orders again.
    bool eventJob(const JobEvents &events, uint32_t &seqno, int *result = nullptr, uint64_t generation = 0)
    {
        setView_.refused.clear();
        if (events.empty() || events.waitCount + events.signalCount > TegraGM20BMaxJobEvents || !ensureChannel())
            return false;
        Killed live = {};
        if ((contextless_ || k_.lost(channel_) || liveKill(live)) && !reopen())
            return false;
        if (generation && generation != channelGeneration()) {
            if (result)
                *result = kIOReturnNotResponding;
            return false;
        }
        SurfaceList none;
        uint32_t seq = 0;
        if (!job(setView_, channel_, nullptr, 0, none, JobVisibility::External, seq, result, events))
            return false;
        lastSeqno_ = seq;
        seqno = seq;
        return true;
    }

    // Why the submitting thread's last job was refused when it was the resource set: the kernel refused to make it
    // again (SetView::refused); empty otherwise
    const std::string &setRefused() const
    {
        return setView_.refused;
    }

    ResourceSet::Counts setCounts() const
    {
        return set_.counts();
    }

    std::string setLastRefusal() const
    {
        return set_.lastRefusal();
    }

    struct Stats {
#if SWITCH_RAW_DIAGNOSTICS
        std::atomic<uint64_t> commits{0}, pushBytes{0}, uploadBytes{0}, ringWaits{0}, ringsOutgrown{0}, imports{0},
            importBytes{0}, poolBytes{0}, poolChunks{0}, cleanBytes{0}, poolStalls{0};
        std::atomic<uint64_t> textureUploads{0}, textureReadbacks{0}, fusedPasses{0};
        // GPU memory made or given back since open: chunks, compressed buffers, memoryless backings' memory, heap purges
        std::atomic<uint64_t> memEvents{0};
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
        // chain levels generateMipmaps made on the 2D engine (and those of its steps no probe covered), and by 3D passes
        std::atomic<uint64_t> mipTwodLevels{0}, mipTwodUnproven{0}, mip3dLevels{0};
        // write-combined buffers: live bytes, and the buffers that asked for one and got pool memory (the WC pool full)
        std::atomic<uint64_t> wcBytes{0}, wcFallbacks{0};
        // the copy engine's work encoded: launches and the bytes they write (copies, remaps, the pass prologue's shadow copies)
        std::atomic<uint64_t> ceOps{0}, ceBytes{0};
        // passes that drew RGB10A8_2P_XR10 in its two planes (raw_xr_pass.h), the planes those rendered through a shadow, and
        // the draws' binds of a two-plane texture's header pair; the copy engine's merges and splits of a one-image form
        std::atomic<uint64_t> xrPlanePasses{0}, xrPlaneShadows{0}, xrPairBinds{0};
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
        FetchCounts fetch;
        // compressed memory held now (raw_comp.h CompMem and made buffers no backing took) and its bytes, the backings on it
        // now; buffers made and the kernel's refusals since the device opened; full-target clears of compressed targets to a
        // value the boot ZBC table holds (no texels written) and to one it does not
        std::atomic<uint64_t> compHeld{0}, compHeldBytes{0}, compViews{0}, compMade{0}, compFailed{0}, zbcHits{0},
            zbcMisses{0};
        // passes whose memoryless attachment wanted compressed memory not made yet, and took plain memory
        std::atomic<uint64_t> compMisses{0};
        // ZBC entries this process added, those a full table refused, and full clears of compressed targets to a value no
        // entry can hold (raw_zbc.h)
        std::atomic<uint64_t> zbcAdded{0}, zbcFull{0}, zbcInexact{0};
        // a shadow's fills and stores whose image was compressed memory (the copy engine through a compressible kind), and
        // the shader depth tests' copies of a compressed zeta
        std::atomic<uint64_t> compCopies{0}, compZetaCopies{0};
        // compute launches encoded (raw_compute.h)
        std::atomic<uint64_t> dispatches{0};
        // states made with the zero-source pixel kill on (raw_blend_kill.h), and their binds encoded
        std::atomic<uint64_t> blendKillStates{0}, blendKillBinds{0};
        std::atomic<uint64_t> blurTwodPasses{0};  // blur-chain passes drawn as 2D blits (raw_draw_shortcuts.cpp)
        std::atomic<uint64_t> resolveTwod{0};     // MSAA resolves by the 2D engine (raw_resolve_twod.cpp)
        std::atomic<uint64_t> resolveExact{0};    // those through the grid's complement (rule resolve.exact)
        // MSAA resolves by route, 2x and 4x: resolve.twod, resolve.exact, the pack's pass (index 2: other sample counts)
        std::atomic<uint64_t> resolveRoute[3][3] = {};
        // passes given a zcull region, and the regions a draw's state invalidated (raw_zcull.h)
        std::atomic<uint64_t> zcullPasses{0}, zcullInvalidations{0};
        // stencil plane draws encoded: the eight-draw route's (raw_stencil_bits.h) and the one-draw route's
        // (raw_zeta_colour.h)
        std::atomic<uint64_t> stencilBitDraws{0}, zetaColourDraws{0};
        std::atomic<uint64_t> splitPrologues{0};         // split draws' prologue launches (raw_split.cpp)
        std::atomic<uint64_t> reopenTicks{0};            // the last reopen that made a channel, mach_absolute_time
        std::atomic<uint64_t> bandedDraws{0}, bands{0};  // raw_draw_bands.h
        std::atomic<uint64_t> learnedDraws{0};           // raw_band_rates.h
        std::atomic<uint64_t> bandedDispatches{0};       // raw_dispatch_bands.h
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        std::atomic<uint64_t> splitLaunches{0};
        std::atomic<uint64_t> interlockDraws{0}, storageBarriers{0};
        // state segment binds, the inline words they replaced, a pass's first binds of the segment the channel still held
        // from the pass or command buffer before (no words; raw_state_segment.h), and passes that could not keep it because
        // their prologue set back the last state's extras (Pipeline::restore)
        std::atomic<uint64_t> segmentBinds{0}, segmentWords{0}, segmentSkips{0}, segmentExtrasLosses{0};
        ReplayTally replay;
        // binding slots the encode compared applying recorded state, and the slots of the tables it compared
        // (RawMetalPlanApply.h): what a comparison of every slot of each changed table looked at
        std::atomic<uint64_t> slotsCompared{0}, slotsInTables{0};
        EncodeTotals encode;
#endif
#else
        std::atomic<uint64_t> heapFull{0};
#endif
        // channels made again after the kernel killed one; GPU order returns on one made since a refusal (raw_order.h)
        std::atomic<uint64_t> reopens{0};
    } stats;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // hw_test_app's hardware probes (RawMetalProbe.mm): push words a draw of one instance emits after its state and before
    // BEGIN, and after its END the words that set back what those set, while armed
    struct ProbeWords {
        std::vector<uint32_t> before, after;
    };

    std::atomic<bool> probeArmed{false};
    // generateMipmaps of a format no probe covered on the 2D engine as fx_mip_probe.h's surface format, recorded from now
    std::atomic<bool> probeMipCandidates{false};
    // memoryless zetas the copy engine fills and drains for the shader's depth tests take their compressible kind (raw_comp.h)
    std::atomic<bool> probeZetaKinds{false};
    // stencil planes take the one-draw route (raw_zeta_colour.h) where it writes them, whatever kColourRopWritesZeta says
    std::atomic<bool> probeZetaColour{false};
    std::atomic<uint64_t> probeEmitted{0};  // draws that emitted them since they were armed
    // hw_test_app's gpu.sched.long_draw_bands: the band budget its unbanded arm sets to 0 (RawMetalProbe.mm)
    std::atomic<uint64_t> bandBudgetNs_{TGM_APP_BAND_NS};
    raw::Mutex probeLock;
    std::shared_ptr<const ProbeWords> probeWords;

    std::shared_ptr<const ProbeWords> armedProbe()
    {
        if (!probeArmed.load(std::memory_order_acquire))
            return nullptr;
        std::lock_guard<raw::Mutex> hold(probeLock);
        return probeWords;
    }

    // the barrier forms of the command buffer begin() starts next (raw_probe_forms.h), set by its encoder
    ProbeForms nextForms;
    // the pipelines made while set bind their programs' shader data at its exact size (raw_cbuf.h): HW Test's bench
    // measuring what the chip reads of a constant buffer bound off a 256-byte multiple
    std::atomic<bool> probeExactCbufs{false};
    // HW Test's debug switch plain-memory: textures, backings and shadows made while set take no compressible memory
    std::atomic<bool> probePlainMemory{false};
    // the next createTexture binds compressible memory of its own (raw_comp.h kinds) where it makes a block-linear
    // target, as the next createOwnedZeta does for a one-sample zeta, which probeCompressed holds until destroyTexture;
    // any other texture takes the flag and stays plain
    std::atomic<bool> probeCompressNext{false};
    // split draws the capture still takes (raw_split_capture.h); 0: disarmed
    std::atomic<uint32_t> splitCaptureLeft{0};
    // private textures stay plain while set (raw_private_comp.h), for a probe's uncompressed reference
    std::atomic<bool> probePlainPrivate{false};
    std::atomic<bool> probeResolveDraw{false};  // resolves take the pack's resolve pass, never resolve.twod
    // resolves resolve.twod takes go through the complement whatever resolve.exact says of their sample count
    std::atomic<bool> probeResolveExact{false};
    // storage forms view compressed attachments and storage-bound memoryless backings compress (gpu.raw.storage_compressed)
    std::atomic<bool> probeStorageCompressed{false};
    // A fetching draw whose state has a fetch-free form of this rung takes it whatever its rule (2, 3), or keeps the storage
    // form (4); a blended storage form's barrier twin (5 a barrier a triangle, 6 one a draw: raw_fetch_rungs.h); 0: the
    // rules decide (raw_fetch_free.h fetchFreeTaken)
    std::atomic<uint8_t> probeFetchRung{0};
    std::map<uint64_t, Bo> probeCompressed;  // lock_
    std::atomic<uint64_t> unflushed_{0};     // signalsUnflushed
#endif

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

    // The render server's blur-texture probe (raw_glass_mem.h); null in any other process. It runs only while armed
    // (glassMemArming); glassMemOn is the frame path's one load, set when armed and cleared at the first begin() after
    std::unique_ptr<GlassMem> glassMem;
    GlassMemArming glassMemArming;
    std::atomic<bool> glassMemOn{false};
    // A submission carrying the probe's reads was killed: the probe stops for the process (its reads are new code on
    // the compositor's every command buffer)
    std::atomic<bool> glassMemStopped{false};
    void glassMemEvent(const char *what, uint64_t bytes);
#endif

private:
    friend class CommandBuffer;
    friend class RenderEncoder;
    std::atomic<uint32_t> drawStamps_{0};  // measured draws' stamp slots taken (raw_stamps.h kDrawStampSlots)
    std::mutex timingsLock_;
    std::vector<DrawTiming> committedTimings_;
    std::atomic<uint32_t> committedTimingCount_{0};
    // A block-compressed image of `layout` at t.va (lock_ held, t.tic allocated): its header of `type` and, past one level,
    // its chain
    void layBlocks(const fx::MipChain &layout, const FormatInfo *f, uint32_t width, uint32_t height, uint8_t type,
                   Texture &t);
    // An array, cube or 3D image of `layout` at t.va (lock_ held, t.tic allocated) under a header of `type`; past one level,
    // level 0 of its chain (Texture::chain), the header every level's
    void layImage(const fx::MipChain &layout, const FormatInfo *f, uint8_t type, Texture &t);
    friend class BlitEncoder;
    friend class ComputeEncoder;
#ifdef SWITCH_RAW_HARNESS_MARKS
    friend struct DeviceHarness;
#endif
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
            uint32_t seqno = 0;
            bool submitted = false;
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
        uint64_t pitchVa = 0;  // a block-linear chunk's kind-0 view (blPitchVa), made at its first ask
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
    std::atomic<bool> highPriority_{false};
    Sizes sizes_ = kSizes[0];
    raw::Mutex lock_;
    Kernel k_;
    // The BOs only this device's jobs touch (raw_resource_set.h); setView_ is the submitting thread's
    ResourceSet set_;
    SetView setView_;
    TextureTransfers transfers_;

    // One job of `channel` naming the set; `uses`, the job's uses no member holds; `recorded`, the members its command
    // buffer used (raw_resource_set.h submitInSet)
    bool job(SetView &view, uint32_t channel, const TegraGM20BAbiPush *pushes, uint32_t count, SurfaceList &uses,
             JobVisibility visibility, uint32_t &seqno, int *result = nullptr, const JobEvents &events = {},
             const std::vector<SetUse> *recorded = nullptr, uint32_t removals = 0, bool hostWritten = false,
             uint64_t admission = 0, uint32_t admissionFlags = 0, bool quiet = false,
             const std::vector<uint32_t> *promises = nullptr)
    {
        const KernelJob made = {channel,
                                pushes,
                                count,
                                nullptr,
                                0,
                                0,
                                0,
                                visibility,
                                events,
                                hostWritten,
                                admission,
                                admissionFlags,
                                quiet,
                                promises ? promises->data() : nullptr,
                                promises ? uint32_t(promises->size()) : 0};
        return submitInSet(k_, set_, view, true, made, seqno, result, &uses, recorded, removals);
    }

    // [va, va + bytes) is in a member, so no job names it, as the submitting thread sees the set; else null. A draw's
    // resources mostly lie in the member the last one found (a ring, a bytes chunk), which answers without a search while
    // the view's version is the one it was found in.
    const SetMember *member(uint64_t va, uint64_t bytes)
    {
        set_.view(setView_);
        const SetMember *last = lastMember_.member;
        if (last && lastMember_.version == setView_.version && va - last->va < last->bytes &&
            bytes <= last->bytes - (va - last->va))
            return last;
        const SetMember *m = setView_.find(va, bytes);
        if (m)
            lastMember_ = {m, setView_.version};
        return m;
    }

    struct {
        const SetMember *member = nullptr;  // in setView_.members, which only a version change replaces
        uint32_t version = 0;
    } lastMember_;

    bool member(const Bo &bo)
    {
        set_.view(setView_);
        return setView_.holds(bo);
    }

    Pack pack_;
    // heap_: the shader heap's GPU VA reservation (bytes: the program window). The pack's pages are bound at its start where
    // programs run in place (packPages_), and from heapCopiesAt_ on parts of host memory, through heapBound_, hold the
    // programs copied there
    Bo heap_, tic_, tsc_, root_, zero_, tickets_, colorTable_, srgbEncode_, stamps_;
    // visibility results' report slots (raw_visibility.h), made at first use
    uint64_t visibilitySlot();
    Buffer visibilityPage_;
    std::once_flag visibilityOnce_;
    std::atomic<uint32_t> visibilityNext_{0};
#if SWITCH_RAW_DIAGNOSTICS
    Bo spanMarks_;
#endif
    // m2v's handle tables and roots, made at the first ID named or state drawn through an argument buffer, and each record's
    // set 1 (raw_heap_tables.h), under lock_
    Bo handleTables_, argumentRoots_;
    std::vector<Bo> heapSets_;
    HeapTables heapTables_;
    // the tables' and sets' CPU writes (counted under lock_, read by commit without it), and their count as of the last
    // submission that invalidated the L2 for them (the submitting thread's): an exact job names them host-written while the
    // two differ
    std::atomic<uint64_t> tablesWritten_{0};
    uint64_t tablesInvalidated_ = 0;
    HeapHolds heapHolds_ = nullptr;
    bool heapMemory();
    const HeapSet *heapSet(const HeapLayout &layout);
    void bindKernelArguments(ComputeState &c, const PackKernel &k);
    void bindArguments(Pipeline &pl, const PackPipeline &pp);
    void tablesWritten();

    struct HeapPart {
        uint64_t offset = 0, bytes = 0;  // in the heap
        uint8_t *cpu = nullptr;
        Bo bo;
    };

    std::vector<HeapPart> heapParts_;
    uint64_t heapCopiesAt_ = 0, heapBound_ = 0;
    ProgramPages packPages_;

    // each mapping of the shader compiler's store slots ran from, its pages from window offset heapStoresAt_ on, a mapping
    // after another's (a store written again is mapped again; the old mapping stays)
    struct StoreRegion {
        const uint8_t *mapping;
        ProgramPages pages;
    };

    std::deque<StoreRegion> storeRegions_;
    uint64_t heapStoresAt_ = 0;
    SlotLookup slotLookup_;
    std::vector<Bo> inPlaceImports_;  // the pages programs run from in place, held as long as the device
    Ring push_, upload_;
    // rings replaced by larger ones, freed once the GPU passed the seqno
    std::vector<OutgrownRing> grownOut_;
    bool overflowing_ = false;  // the open command buffer outgrew a ring with no memory for a larger one
    std::deque<std::vector<uint8_t>> overflow_;  // where it is written then (never submitted)
    uint32_t channel_ = 0, lastSeqno_ = 0;
    Bo bufferBoLocked(const Buffer &buffer) const;
    // the channel's fence word; before there is a channel a zero, which every seqno submitted so far (none) has passed
    const volatile uint32_t *fence_ = &kNoFence;
    static constexpr uint32_t kNoFence = 0;

    SemanticFenceRead semanticFence(uint32_t seqno) const
    {
        const volatile uint32_t *fence = fence_;
        if (fence == &kNoFence)
            return {};
        const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
        return readSemanticFence(reinterpret_cast<const volatile TegraGM20BAbiFenceSemantic *>(record + 1), seqno);
    }

    raw::Mutex channelLock_;
    std::atomic<bool> channelReady_{false};
    ChannelGenerations generations_;
    bool channelFailed_ = false;
    uint64_t channelRetryNs_ = 0;  // CLOCK_UPTIME_RAW: a refused channel is asked for again from then on
    static constexpr uint64_t kChannelRetryNs = 1000000000;
    std::string channelError_;
    // The channel died and the one made since has no context yet (reopen gave up before the context's Exec): begin() makes it
    // again before it gives out a command buffer. channel_ is 0 while the kernel holds none of this device's.
    bool contextless_ = false;
    std::vector<uint32_t> freeTic_, freeTsc_, freeTicPairs_;
    ViewCache views_{kTextureHeaders};  // viewTic's answers, the encoding thread's
    // by header: its views' word 0 (above it, the sample grid of a storage view: raw_view.h viewHeader) and header (viewTic)
    std::unordered_multimap<uint32_t, std::pair<uint64_t, uint32_t>> viewTics_;
    // by header: its attachment views and their headers (attachmentViewTic)
    std::unordered_multimap<uint32_t, std::pair<AttachmentView, uint32_t>> attachmentViewTics_;
    // by header: its attachment parts' key and header (attachmentTic)
    std::unordered_multimap<uint32_t, std::pair<uint64_t, uint32_t>> partTics_;
    uint32_t nextTic_ = 1, nextTsc_ = 1, neutralTsc_ = 0, neutralCompareTsc_ = 0;
    // the pack's, by record index, each made at its first lookup (a process uses a few of the pack's thousands); a node
    // container, so the pointers pipeline() hands out stay valid
    std::unordered_map<uint32_t, Pipeline> pipelines_;
    std::vector<uint32_t> placed_;  // each pack program's heap offset, ~0u: not placed
    uint64_t heapPlaced_ = 0;
    // Programs placed so far, in place or copied, and that count as of the last submission whose prologue invalidated the
    // SMs' instruction caches (~0: none on this channel). The instruction cache prefetches past a program's end, where the
    // next program is placed.
    std::atomic<uint64_t> heapEpoch_{0};
    uint64_t heapInvalidated_ = ~0ull;
    // Shader local memory (raw_slm.h), under lock_: the area and the most a placed program or kernel asks of a warp. An area
    // it outgrew is kept: a submission already made may still use it.
    Bo slm_;
    std::atomic<uint64_t> slmPerWarp_{0}, slmVa_{0};
    std::vector<Bo> slmOutgrown_;
    // the per-warp size the channel was last given (commit; 0 on a new channel): the submitting thread's
    uint64_t slmChannel_ = 0;
    uint32_t ctWrite_[8] = {};  // SET_CT_WRITE(i) as the baseline leaves it
    bool slmNeed(uint64_t perWarp);
    bool place(uint32_t prog, uint32_t &program, uint64_t &data);
    bool heapGrow(uint64_t end);
    const HeapPart *heapPartAt(uint64_t at) const;
    bool heapPlace(const uint8_t *bytes, uint32_t size, uint64_t &offset);
    bool placeInPlace(ProgramPages &pages, const uint8_t *file, uint64_t fileOffset, uint32_t size, uint64_t &offset);
    bool placeFromStore(const uint8_t *slot, uint32_t size, uint64_t &offset);
    const uint8_t *heapCpu(uint64_t at) const;
    // `spills`: its argument tables' words (PackPipeline::spillWords), null where it has none. `tess`: a tessellation state's
    // control and evaluation programs (null: not one)
    void build(Pipeline &pl, const PackPipeline &pp, const PlacedProgram &vs, const PlacedProgram &fs,
               const uint32_t *blob, const char *keyString, bool caTransform, const uint32_t *spills,
               const PlacedProgram *tess = nullptr);
    // whether this runtime can draw pack record `rec`: its push and storage metadata are ones it reads
    bool usableRecord(const PackPipeline *rec) const;
    // lock_ held: pack record `pp` placed (its programs in the heap) and built, once; null where the heap has no room
    Pipeline *placeRecord(const PackPipeline &pp, bool caTransform);
    // lock_ held: `pl`'s split offered, its twin made when a draw first takes it (makeSplit)
    void offerSplit(Pipeline &pl, SplitSource &&source);
    void offerVariants(Pipeline &pl, const PackPipeline &pp);
    bool placePackProgram(uint32_t index, PlacedProgram &out);
    std::deque<Pipeline> variantPipelines_;
    std::deque<Pipeline> reachPipelines_;
    std::deque<PackPipeline> reachRecords_;
    // lock_ held: the push words a reach twin adds to its state's methods and to its restore (made with the first twin)
    std::vector<uint32_t> reachMethods_, reachRestore_;
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
        std::vector<uint32_t> blob, spill;
        PackProgram programs[2];
        Pipeline pipeline, turned;
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
    HeldSegment heldSegment_;               // the state segment its registers hold after everything submitted
    StateSegmentHeap segmentHeap_;          // lock_ (place)
    CommandBuffer cb_;
    uint32_t rootPush_[32] = {};  // what the GPU's root table holds at [0x700, 0x780) after everything submitted
    uint32_t restart_ = 0;        // and its primitive restart index
    uint32_t rtLayer_ = ~0u;      // and SET_RT_LAYER (~0: not known)
    DepthStencilRegs ds_;         // and its depth/stencil registers
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, Import *> surfaces_;
    std::vector<std::unique_ptr<NativeSurface>> nativeSurfaces_;
    uint64_t nativeGeneration_ = 0;
    const Texture *nativeRead(const Texture &source, NativeSurface *&owner, int &error);
    void reapNativeSurfaces(bool all = false);
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

    // lock_ held: where block-linear span `s`'s bytes lie under a kind-0 view of its chunk; 0: no view could be bound
    uint64_t blPitchVa(const Span &s);

    struct Arena {
        uint64_t va = 0, bufferVa = 0, bytes = 0;
        uint32_t kind = 0;
        uint8_t *cpu = nullptr;  // its host range, reserved for good; its pages wired and bound unless purged
        Bo bo;
        HeapPlacement place;
        uint32_t leases = 0;  // m2v views of its resources (raw_alias.cpp): never purged or freed under one
        bool used = false, isVolatile = false, purged = false, dying = false;
        bool wc = false;  // its host pages are write-combined (raw_arena.cpp hostPages)

        // its BO bound again under another PTE kind (createArena's comment)
        struct View {
            uint64_t va = 0;
            uint32_t kind = 0;
        };

        std::vector<View> views;
        // trimmed (raw_arena_trim.h): `bo` is empty, its pages are bound as `parts` (each with its offset), and `holes`
        // are unbound, their host pages untouched
        std::vector<std::pair<uint64_t, Bo>> parts;
        std::vector<ArenaRange> holes;
    };

    std::vector<Arena> arenas_;
    std::atomic<uint32_t> purgedArenas_{0};
    bool bindArena(Arena &a);  // lock_ held
    // lock_ held: [offset, offset + bytes) of `a`'s host range imported and bound at its VAs as one more part
    bool bindArenaPart(Arena &a, uint64_t offset, uint64_t bytes);
    void unbindArena(Arena &a);  // lock_ held: its import, whole or in parts, freed
    void trimLocked(Arena &a, const std::atomic<uint32_t> &committed);
    // lock_ held: the holes a placement of [offset, offset + bytes) reaches bound again; false: no memory for them
    bool backPlacement(Arena &a, uint64_t offset, uint64_t bytes);
    void purgeLocked(Arena &a, const std::atomic<uint32_t> &committed);
    void freeArena(Arena &a);  // lock_ held
    void arenaFree(Span &s);   // lock_ held
    bool aliasArena(const Span &s, Lease *&lease, Alias &a);
    // lock_ held: the arena's mapping of `kind` (its own, its pitch view, or a view made now); 0: none
    uint64_t arenaView(Arena &a, uint32_t kind);
    // The kind of the arena's mapping `va` lies in; false: none of its
    static bool arenaKindAt(const Arena &a, uint64_t va, uint64_t &base, uint32_t &kind);
    // The layout every create* but createTexture's own memory gives its kind of texture: t's shape, rows and kind fields
    // with no memory or header, what its memory takes and the PTE kind it is bound with, `chain` a Mips, Volume or Array
    // one's levels. False: the device makes no such texture.
    static bool layout(const TextureAsk &ask, Texture &t, uint64_t &bytes, uint64_t &align, uint32_t &kind,
                       fx::MipChain *chain = nullptr);
    static TextureAsk standInAsk(const Texture &t);
    // `ask` laid out in `at`'s arena, else (any kind but Plain) in the device's chunks of its kind, with its header
    bool createLaidOut(const TextureAsk &ask, Texture &t, const ArenaAt *at);
    // lock_ held: `bytes` of the arena under `kind` as t's memory (Texture::arena)
    bool arenaSpan(const ArenaAt &at, uint64_t bytes, uint64_t align, uint32_t kind, Texture &t);

    // a span of chunks bound with PTE kind `kind` (block-linear images), lock_ held
    bool tiledAlloc(std::vector<Chunk> &chunks, uint32_t kind, uint64_t bytes, uint64_t align, Span &s);
    void tiledFree(std::vector<Chunk> &chunks, Span &s);
    // lock_ held (`unlocked`: the caller's lock, let go for the kernel's wiring); the chunk's index in `chunks`
    bool newTiledChunk(std::vector<Chunk> &chunks, uint32_t kind, uint64_t size, bool dedicated,
                       std::unique_lock<raw::Mutex> *unlocked, uint32_t &index);
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
    // size classes (sizeClassBytes of kBlAlign), the largest one new backings go on, given back with its last view.
    // Layered passes' backings have memory of their own: a layered pass neither grows the one-layer passes' memory of its
    // tag nor retires their backings, and a one-layer backing is never a layered pass's.
    struct PlainMem {
        Span span;
        uint64_t align = 0;
        uint32_t tag = 0, memory = 0;  // kPlainPool, kPlainBl, or kPlainZeta + the zeta format's index
        uint32_t views = 0;
        bool layered = false;

        bool holds(uint32_t t, uint32_t m, bool l) const
        {
            return tag == t && memory == m && layered == l;
        }
    };

    struct Backing {
        Texture texture;
        uint32_t tag = 0;
        std::atomic<uint32_t> lastUse{0};
        std::atomic<bool> submitted{false};
        // commitCount_ and closedCount_ when a pass last asked for it: the encoding thread stores them with no lock
        // (lastBacking), the compressed-memory thread's trim reads them under lock_
        std::atomic<uint64_t> lastCommit{0}, lastClosed{0};
        std::atomic<uint64_t> usedNs{0};  // compClock() when made, and at each commit of a command buffer that used it
        CompMem *comp = nullptr;          // the compressed memory it is a view of (raw_comp.h), else none
        PlainMem *plain = nullptr;        // or the plain memory (a memoryless one's)
        // a compressed backing of its shape, or one on larger memory of its tag, replaced it: no pass takes it again,
        // trimBackings frees it
        bool retired = false;
    };

    static constexpr uint32_t kMemorylessTag = kTransientColorTag;
    static constexpr uint32_t kShaderDepthTag = kMemorylessTag + 8;
    static constexpr uint32_t kShaderStencilTag = kShaderDepthTag + 1;
    static constexpr uint32_t kShaderStencilTempTag = kShaderDepthTag + 2;
    std::array<Backing *, kShaderStencilTempTag + 1> lastBackings_{};

    static constexpr uint32_t kPlainPool = 0, kPlainBl = 1, kPlainZeta = 2;
    std::vector<std::unique_ptr<PlainMem>> plainMems_;
    PlainMem *largestPlain(uint32_t tag, uint32_t memory, bool layered) const;
    // A memoryless backing of `m`'s shape on the largest plain memory of its tag and memory, made where none holds it;
    // nullptr when no memory or header is had
    Backing *plainBacking(const Texture &m, uint32_t tag, bool blockLinear);
    void plainFree(Backing &b);
    void freePlainSpan(PlainMem &mem);
    void dropPlain(PlainMem *mem);  // its span back, and it out of plainMems_

    // createOwnedZeta's memory by its address: its format's index and span, outside Texture::span, which alias() and
    // freeSpan() take for block-linear colour memory
    std::map<uint64_t, std::pair<uint32_t, Span>> ownedZeta_;

    // moveOwnedZeta's textures by their new address: their own memory's address and their header before the move
    struct MovedZeta {
        uint64_t ownedVa;
        uint32_t staleTic;
    };

    std::map<uint64_t, MovedZeta> movedZeta_;

    void freeOwnedZeta(uint64_t va)  // lock_ held
    {
        if (const auto moved = movedZeta_.find(va); moved != movedZeta_.end()) {
            if (moved->second.staleTic)
                freeTic(moved->second.staleTic);
            va = moved->second.ownedVa;
            movedZeta_.erase(moved);
        }
        const auto at = ownedZeta_.find(va);
        if (at == ownedZeta_.end())
            return;
        tiledFree(zChunks_[at->second.first], at->second.second);
        ownedZeta_.erase(at);
    }

    std::vector<Backing *> backings_;  // memoryless attachments' memory, by size and format

    // What backing() answered, kept by the encoding thread for lastBacking
    struct BackingAsk {
        uint32_t width = 0, height = 0, format = 0, tag = 0, layers = 1;
        uint8_t samples = 0;
        // zetaProbe: probeZetaKinds as the ask found it, which compKind reads and no epoch move covers
        bool zeta = false, blockLinear = false, compression = false, copied = false, zetaProbe = false;
        Backing *backing = nullptr;
        uint64_t epoch = 0;

        bool same(const Texture &m, uint32_t t, bool linear, bool compressed, bool copy, bool probe) const
        {
            return backing && width == m.width && height == m.height && format == m.format && tag == t &&
                   layers == std::max(m.layers, 1u) && samples == m.samples && zeta == m.zeta &&
                   blockLinear == linear && compression == compressed && copied == copy && zetaProbe == probe;
        }
    };

    std::array<BackingAsk, 16> asks_{};
    uint32_t nextAsk_ = 0;
    // moved, lock_ held, by everything that changes what backing() answers: memory made, a backing retired, a trim
    std::atomic<uint64_t> backingEpoch_{1};

    void backingsChanged()
    {
        backingEpoch_.fetch_add(1, std::memory_order_seq_cst);
    }

    // trimmed backings, deleted at the encoding thread's next begin(), where no lastBacking reads one; trimmedPending_
    // says there are some without lock_
    std::vector<Backing *> trimmedBackings_;
    std::atomic<bool> trimmedPending_{false};
    // the pool's growth ahead of need (lock_): free bytes of its shared chunks, a chunk being made by the grower thread
    uint64_t poolFree_ = 0;
    bool growing_ = false, stopping_ = false;
    raw::CondVar grow_, grown_;
    std::thread grower_;
    bool newChunk(uint64_t size, bool dedicated, std::unique_lock<raw::Mutex> *unlocked, uint32_t &index);
    void growLoop();
    // Written by the submitting thread alone; a command buffer's backings are marked used before closedCount_ moves
    std::atomic<uint64_t> commitCount_{0};
    std::atomic<uint64_t> closedCount_{0};  // command buffers committed or abandoned

#if SWITCH_RAW_DIAGNOSTICS
    // the probe's ring of rows (raw_glass_mem.h) and the records of the command buffers whose rows it holds, read at a
    // begin() once their fences passed
    Bo glassMemRing_;
    GlassMemCb glassMemCbs_[kGlassMemSlots];
    bool glassMemBusy_[kGlassMemSlots] = {};
    uint32_t glassMemBusyCount_ = 0;
    uint64_t glassMemSummaryAt_ = 0, glassMemSummaryCbs_ = 0, glassMemSummaryLaunches_ = 0, glassMemSummaryNs_ = 0;
    bool glassMemLive_ = false;  // armed for the command buffer being encoded (begin())
    void glassMemFrame();
    void glassMemRead(uint64_t now);
#endif

    // A pass's backing texture (Device::backing) is its Backing's first member
    static Backing &backingOf(Texture &t)
    {
        return *reinterpret_cast<Backing *>(&t);
    }

    uint64_t commits() const
    {
        return commitCount_.load(std::memory_order_acquire);
    }

    // begin()'s work under lock_ where it has any: native surfaces to reap, trimmed backings to delete
    void reapAtBegin();
    // a native surface's cache was dropped (NativeSurface::cached, under lock_): begin() reaps under lock_ until none
    // dropped is left
    std::atomic<bool> nativeStale_{false};

    bool init(std::string &err);
    bool openChannel(std::string &err);
    void growRing(Ring &r, uint64_t most);
    void freeChunk(Chunk &ch);
    uint32_t contextWords() const;
    void emitContext(Push &p);
    bool reopen();
    bool newContext(uint32_t channel, uint32_t &seqno);
    bool waitFence(uint32_t seqno, uint64_t generation);

    // A seqno of a channel reopen() retired: as that channel ended it, its failure's error kept for lostError()
    bool retiredWait(uint32_t seqno, uint64_t generation)
    {
        uint32_t error = 0;
        if (generations_.completed(generation, seqno, &error))
            return true;
        killedError_.store(error, std::memory_order_relaxed);
        return false;
    }

    // the last seqno of a channel that died: it and every one before it failed (or completed before; not told apart),
    // where the kernel writes no kill records

    // The seqnos a kill took (TegraGM20BAbiFence): the live channel's record, then the ones the channels it replaced left,
    // kept until reopen made the record's slot over (a slot's seqnos continue, so a kept record names its channel's alone).
    using Killed = KillRange;

    bool liveKill(Killed &k) const;
    bool killed(uint32_t seqno) const;
    std::vector<KillRange> failedSeqnos() const;
    void forgetKept();
    const volatile uint32_t *keptSlot_ = &kNoFence;  // the fence record of the slot the kept records were written in
    mutable raw::Mutex killedLock_;
    std::atomic<bool> keptKills_{false};
    std::atomic<uint32_t> keptThrough_{0};
    std::array<Killed, 4> killedKept_{};
    uint32_t killedNext_ = 0;
    mutable std::atomic<uint32_t> killedError_{0};
    uint8_t *alloc(Ring &r, uint32_t bytes, uint32_t align, uint64_t &va);
    void retire(Ring &r, uint32_t seqno);
    bool ownWords(const Ring &r, uint64_t at, uint64_t bytes) const;
    bool outgrow(Ring &r, uint32_t bytes, uint64_t &at);
    uint32_t allocTic();
    void freeTic(uint32_t tic);  // lock_ held
    // Two headers at consecutive indices (an over-planes RGB10A8_2P_XR10 texture's), the first's index; 0: none left. lock_ held
    uint32_t allocTicPair();
    void freeTicPair(uint32_t first);
    void writeTic(const Texture &t);
    bool poolAlloc(uint64_t bytes, Span &s);
    void poolFree(Span &s);
    bool tiledSurfaceTexture(uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, Texture &t,
                             Texture *sampled);
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

    // Scratch colors occupy distinct tags within a pass and reuse them after its GPU boundary. Depth and shader-test
    // scratch remain separate. Plain and compressed views keep their own backing and retire after their final fence.
    // `copied`: the copy engine fills or drains it (a shadow), so it compresses only where raw_comp.h proves that safe.
    Texture *backing(const Texture &memoryless, uint32_t tag = 0, bool blockLinear = false, bool compression = true,
                     bool copied = false);
    // The backing backing() last answered the same ask with, taken with no lock while nothing that decides backings has
    // changed since (backingEpoch_); nullptr: ask backing() under lock_. The encoding thread's alone.
    Texture *lastBacking(const Texture &memoryless, uint32_t tag = 0, bool blockLinear = false, bool compression = true,
                         bool copied = false);
    void trimBackings();

    // Compressed backings (raw_comp.cpp), lock_ held: the compressible kind a backing of this shape takes (0: none or
    // compression off); a backing on compressed memory made off this thread, or nullptr after asking compThread_ for some
    CompQueue comp_;
    // made off the render thread on a thread of its own, started at the first ask: the kernel zeroes the pages a free left
    // dirty and clears the comptags at the bind, which takes milliseconds for a full-screen target, and no other thread (the
    // pool's grower included, which a render thread may wait for) waits behind it
    std::thread compThread_;
    raw::CondVar compWake_;
    uint32_t compKind(const Texture &m, bool blockLinear, bool copied) const;

    bool zetaKindsProbed() const
    {
#if SWITCH_RAW_DIAGNOSTICS
        return probeZetaKinds.load(std::memory_order_relaxed);
#else
        return false;
#endif
    }

    Backing *compBacking(const Texture &m, uint32_t tag, uint32_t kind);
    // What was asked for, made, `l` dropped around each kernel allocation
    void compMake(std::unique_lock<raw::Mutex> &l);
    void compStop();
    // Asks compThread_ for `w` unless it was
    void compAsk(const CompWant &w);
    // A new multisampled memoryless attachment's compressed memory, asked for at its creation so its first pass finds it made
    void compPrearm(const Texture &m);
    // compThread_ started where it is not: it trims what an idle process holds when it turns stale (compDeadline)
    void compStart();
    bool compRearm_ = false;  // shared plain memory was made: compThread_ takes its deadline (lock_)
    void compFree(Backing &b);
    // Made buffers no backing took while stale, at `now`
    void compTrim(uint64_t now);
    // When compressed or shared plain memory, a made buffer or a retired backing next turns stale (compClock()), 0 with
    // none held
    uint64_t compDeadline() const;
    // private textures on compressed memory and their memory (raw_private_comp.h), lock_
    std::unordered_map<const Texture *, PrivateComp> privateComp_;
    std::atomic<uint64_t> privateMade_{0}, privateTransitions_{0};
    void releasePrivateCompressed(const Texture &t);  // lock_ held
    // A clear of target t to SET_COLOR_CLEAR_VALUE words `v` (or depth `depth`), `full` it covers t whole: on a compressed
    // target, a value the ZBC table lacks is asked for (raw_zbc.h); hits and misses counted
    void clearZbc(const Texture &t, bool full, const uint32_t v[4]);
    void clearZbcDepth(const Texture &t, bool full, float depth);
    ZbcTable zbc_;
    // zcull's layout where the kernel reports one (raw_zcull.h), from open
    TegraGM20BAbiZcullInfo zcullInfo_ = {};
    bool zcullOn_ = false;
    // Whether a full clear to `e` writes comptags only; one this process never asked for is asked for
    bool zbcHeld(const TegraGM20BAbiZbc &e);
    // compThread_ asks the kernel for the ZBC entries wanted, `l` dropped around each call
    void zbcMake(std::unique_lock<raw::Mutex> &l);
};

// NV topology for an MTLPrimitiveType (point, line, lineStrip, triangle, triangleStrip)
uint32_t nvTopology(uint32_t mtlPrimitive);
// The tessellation factor scale m2v's control stage takes (raw_tessellation.cpp): Metal's, rounded to half; false where it is
// not a positive half
bool tessellationScaleBits(float scale, uint32_t &bits);

}  // namespace raw
