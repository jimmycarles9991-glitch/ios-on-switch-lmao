// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>
#include "fx/fx_mip.h"
#include "kernel.h"
#include "sampler.h"
#include "sampler_map.h"
#include "stats.h"
#include "xr.h"
#include "ycbcr.h"
#include "ycbcr_store.h"
#include "zeta.h"

namespace gm20b {

struct NativeSurface;

constexpr uint32_t kPteKindPitch = 0;

struct Lease;      // alias.cpp
struct TexelRows;  // texel_rows.h

// Where memory comes from: the pool, the write-combined GPU pool, an import, the block-linear chunks (kind 0xFE), an MTLHeap's
// span (arena.cpp)
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

// A blit's aspect of a depth/stencil texture, numbered as the MTLBlitOption that names it (TegraGM20BMetalBlit.mm records the option):
// DepthFromDepthStencil (1) or StencilFromDepthStencil (2), else the whole texel (0)
enum : uint8_t {
    kAspectWhole = 0,
    kAspectDepth = 1,
    kAspectStencil = 2
};

// MTLPixelFormat -> NV formats: the texture header's word 0 NVK writes (read off the twin: pack/mkformats.py), its render target
// format when the driver may render to it (0: sampled only), bytes per pixel, sRGB.
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
// A depth texture's layers seen as `type` (an array or cube): NIL's array header, where its layers lie as NIL's array lays
// them; false where they do not
bool zetaArrayHeader(const Texture &t, uint32_t type, uint32_t w[8]);
// A stored depth/stencil texture's image (zetaShape's) of `layers` layers seen as `type` and `levels` levels, `t`
// reset to it: one level's layers layerBytes apart; several levels NIL's 2D array of them in `c` (fx::arrayLayout),
// level 0's rows and block height in `t`. False where neither lays it out, or where `sampled` and one level's layers
// lie where no array header names them
bool zetaImageLayout(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, uint32_t layers,
                     uint32_t levels, uint8_t type, bool sampled, Texture &t, fx::MipChain &c, uint64_t &bytes,
                     uint64_t &align);

// A texture of this size and format the driver can lay out: `t` reset to it, with no memory yet
bool shape(uint32_t width, uint32_t height, uint32_t format, Texture &t);

constexpr uint64_t kBlAlign = 16384;         // NVK's image alignment here (r12-cafx niloracle), a multiple of NIL's
constexpr uint64_t kChunkVaAlign = 0x10000;  // the GMMU big page, a multiple of kBlAlign

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

// The first texture's shape a new import asks the kernel the surface's layout with (kernel.h surfaceLayout)
struct LayoutAsk {
    uint32_t width = 0, height = 0, bpp = 0, bytesPerRow = 0;
    bool writes = false;
};

struct StoragePiece {
    uint64_t va, bytes;
};

// While its allocation is live, later trim/restoration cannot split these cuts.
struct ArenaStorage {
    uint32_t arena;
    uint64_t offset;
    uint8_t *cpu;
    StoragePiece first = {};
    std::vector<StoragePiece> rest{};

    uint64_t va() const
    {
        return first.va;
    }

    uint64_t bytes() const
    {
        const StoragePiece &last = rest.empty() ? first : rest.back();
        return last.va + last.bytes - va();
    }

    template <typename Each> void each(Each &&visit) const
    {
        visit(first);
        for (const StoragePiece &piece : rest)
            visit(piece);
    }
};

// A page-aligned span of a pool chunk.
struct Span {
    uint32_t chunk = ~0u;
    uint64_t offset = 0, bytes = 0, va = 0;
    uint8_t *cpu = nullptr;
    const ArenaStorage *storage = nullptr;

    bool valid() const
    {
        return chunk != ~0u;
    }
};

// An MTLHeap's arena a texture is made in (arena.cpp): at `offset` of a placement heap's, else wherever it fits
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
        Zeta,         // createOwnedZeta: `sampled` (its layers laid out as a texture header names them)
        Block         // createBlockTexture: `levels`, `layers`, `type` (its header's), `depth` (3D: planes)
    } kind = Plain;

    uint32_t width = 0, height = 1, depth = 1, layers = 1, levels = 1, samples = 1, format = 0;
    uint8_t type = 0;
    bool tiled = false, view2d = false, sampled = false;
    bool standIn = false;  // an extended-range Plain one's stand-in (createStandIn) beside it
    // Mips, Array or Multisample with no arena: compressed memory of this kind where some of its size is ready
    // (private_comp.h), else plain
    uint32_t compressKind = 0;
};

struct Texture {
    struct StorageRange {
        uint64_t va = 0, bytes = 0;
        bool hostVisible = false;
        const ArenaStorage *arena = nullptr;
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
    // A colour view of a zeta's memory (zeta_colour.h): its pages keep the zeta's PTE kind, which the colour ROP's writes
    // do not reach (boot 21's gpu.gm20b.zeta_colour), so a pass renders it like a shadow target, through a plain pool image
    bool zetaPages = false;
    bool memoryless = false;  // no memory: a pass gives it a backing (Device::backing)
    bool wc =
        false;  // its span is write-combined GPU-pool memory (createTexture wc): CPU writes need no clean (Buffer::wc)
    uint32_t tic = 0;  // texture header index, 0: none (not sampleable)
    uint32_t nativePlane = ~0u;
    uint64_t nativeGeneration = 0;
    Import *import = nullptr;  // surface or no-copy pages, else
    Span span;                 // pool memory, else a view (memory of the texture it views)
    Lease *lease = nullptr;  // span's, once it was made aliasable (makeArenaAliasable): span is freed at its last drop
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t heldBytes = 0;  // the memory of its own it took at creation, in Stats::textureBytes until destroyTexture
#endif

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
    // A two-plane YCbCr texture rendered into (ycbcr_store.h) renders through planarStandIn too, a memoryless RGBA32Float
    // attachment whose backing is the pass's; the pack's store states then draw the luma plane and the chroma blocks from that
    // backing, ycbcrStore their fragment buffer
    YcbcrStoreShape *ycbcrStore = nullptr;
    // A two-plane YCbCr surface's texture (ycbcr.h): the image sampled is span (RGBA16Float, RGBA32Float, RGBA8Unorm,
    // RGB10A2Unorm), which a pass of the pack's conversion state draws from the planes' textures, luma (R8Unorm, R16Unorm) and
    // chroma (RG8Unorm, RG16Unorm; none over plane 1), before the work that samples it (pack/ycbcr.metal; shape: its fragment
    // buffer 0)
    Texture *luma = nullptr, *chroma = nullptr;
    // A one-plane 4:2:2 YCbCr texture (YcbcrPlan::onePlane): luma and chroma are pool images BlitEncoder::splitPlane fills
    // from planes.luma (the surface's plane, import) before each conversion, its pairs Y0 Cb Y1 Cr or, pairsReversed,
    // Cb Y0 Cr Y1
    bool onePlane = false;
    bool pairsReversed = false;
    uint32_t shape[kYcbcrShapeWords] = {};
    uint8_t ycbcr = 0;  // YcbcrOutput, 0: none
    // m2v's address map of a planar texture's samplers (metal2vulkan texture/ycbcr_image.rs `ycbcr_modes`): 1 the wrapping modes
    // (repeat, mirrored repeat, mirror-clamp) clamp to the edge (4:2:0 converted), 2 clamp to zero and to the border clamp to the
    // edge (4:4:4 codes), 3 both (4:2:0 codes)
    uint8_t addressMap = 0;
    // R10Unorm_PACKED or RG10Unorm_PACKED of several levels (packed_plane.h): every lod reads level 0, so each level L of
    // its chain holds level 0's top-left corner (level_zero.h copyLevelZero) and its samplers read level 0 alone
    // (Sampler::mapped[kLevelZeroMap]); its address map is 0
    bool levelZero = false;

    // A YCbCr texture whose plane rows run past its surface's last page (YcbcrPlan::window): the GPU VA its planes are read
    // through, its import's `pages` bound again at its start and the rest sparse (Device::openSparseWindow); 0 bytes: none.
    // A job's use names only those pages: the kernel refuses a use no one binding holds
    struct Window {
        uint64_t va = 0, bytes = 0, pages = 0;
    } window;

    // Its image is converted from a surface's planes: Metal's copies of it move the planes' bytes
    bool converted() const
    {
        return planar || ycbcr;
    }

    // Rows 16 but not 32 bytes apart (or an address off 32): the texture header's pitch takes multiples of 32, so it is sampled
    // through `sampled`, a block-linear pool image of its texels the copy engine refreshes from it before work that samples
    // it, as m2v samples such a surface through a shadow image it copies in. tic is sampled's.
    Texture *sampled = nullptr;
    // A plain format over a surface plane whose rows overlap or lie under 16 bytes (plain_rows.h): `sampled` read as
    // integer texels of its size, the view the pack's overlap state stores its rows from after a pass renders the pool
    Texture *storeView = nullptr;
    // A no-copy block texture whose caller's memory is off its image's alignment (block_base.h) is a pool image of the
    // same layout: callerPages are the caller's pages, bound pitch, that the copy engine fills the image from
    // (Device::copyCallerPages) and that Metal's copies into the image go on to. Both copies reach the image through
    // imagePitchVa, a kind-0 view of its chunk (Device::blPitchVa): each kind places a GOB's bytes in DRAM its own way
    // (TegraGM20BRelayout.h), so only kind 0 on both sides moves the DRAM bytes unchanged. A view's are its parent's.
    Import *callerPages = nullptr;
    uint64_t imagePitchVa = 0;
    bool surfaceView = false;
    uint32_t surfaceX = 0, surfaceY = 0;
    // A texel buffer over a surface the kernel laid out block-linear (texel_rows.h): this texture is a pitch pool of its
    // bytes, which the copy engine fills from the surface's rows and copies back into them
    TexelRows *texelRows = nullptr;

    // An extended-range texture's (xr.h): the image a draw samples, its codes' values as RGBA16Float, which a pass of the
    // pack's conversion state draws from the codes (tic) before the work that samples it. Null: not sampled.
    Texture *standIn = nullptr;
    // RGB10A8_2P_XR10 held in Metal's two planes (xr_pass.h): luma the words (R32Uint), chroma the alpha bytes (R8Uint),
    // each a target and a fetch's header; this texture has no memory, and its header (tic, an over-planes one's) is plane
    // 0's words read as RGB10A2Unorm codes, plane 1's bytes as R8Unorm at the next index. xrPlane on luma (1) and chroma (2)
    bool xrPlanes = false;
    uint8_t xrPlane = 0;
    // The bytes between a plane's rows as a GPU store lands them where they are not `pitch`, its rows as reads take them:
    // an RGB10A8_2P_XR10 plane of rows under 16 bytes, which an M4 Max reads 1 MiB apart and stores 16 apart
    // (xr_pools.h storedRows). 0: stores land at `pitch`
    uint32_t storePitch = 0;
    // Metal keeps it losslessly compressed (m2v nan_clear.rs keeps_nan_sign): an extended-range Clear in a pass that records
    // no draw call stores Metal's straight route's codes
    bool compressed = false;
    // Its memory is bound with a compressible kind (comp.h): its passes enable colour, or Z and stencil, compression
    bool comptags = false;
    // the command buffer (CommandBuffer::serial) that last blended into it through a rung-2 form (rule fetch.blend's
    // accumulates), 0: none; read and written relaxed, from any encoding thread
    mutable uint64_t ropBlendSerial = 0;
#if SWITCH_METAL_DIAGNOSTICS
    // A pass, copy, dispatch or upload of this process wrote it (glass_probe.h)
    bool probeWritten = false;
#endif

    // A mipmapped texture's levels (r12, fx): `chain` is the block-linear image of every level, NIL's layout, and this texture
    // is its level 0 (va, span, pitch and block height level 0's; its header, tic, the chain's). The 2D engine fills the later
    // levels from level 0 in place (BlitEncoder::generate); m2v's twin is a texture of every level over the same memory, so
    // whatever either driver writes into a level is the level.
    struct Chain {
        fx::MipChain layout;
        uint64_t va = 0;
        uint32_t fmt2d = 0;  // the 2D engine's surface format (fx::twodChainFormat); 0: it makes none of its steps
        uint32_t levelTic[fx::kMaxLevels] = {};  // a view of each level alone (Device::levelTic), made on first use
        uint32_t levelCodesTic[fx::kMaxLevels] = {};  // the same of an sRGB chain's codes, with no sRGB conversion
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

    // A depth/stencil format (zeta.h kZetaFormatInfo): block-linear zeta memory, bound with its format's PTE kind; never a
    // colour target, sampled (tic) only where it was made to be. A stored one (not memoryless) is NVK's image, rendered,
    // sampled and copied in place, by aspect
    // (BlitEncoder::copyBufferTexels): a Depth32Float_Stencil8 one's stencil through stencilTemp, two bytes a texel (NVK's
    // stencil_copy_temp)
    bool zeta = false;
    Texture *stencilTemp = nullptr;
    // A 4x colour texture's sample grid as pitch rows, which its copies with a buffer go through
    // (BlitEncoder::copyBufferSamples), made on its first
    Texture *sampleRows = nullptr;

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
    // Metal's rotation of a texture over a surface plane (rotation.h): width and height are the plane's, the frame a pass
    // draws is turned from them
    uint8_t rotation = 0;
};

struct NativeSurface {
    Bo bo;
    TegraGM20BAbiNativeSurfaceViewReply layout = {};
    using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    std::map<Key, Texture> textures;
    uint64_t channelGeneration = 0, generation = 0;
    uint32_t lastUse = 0, encodingReferences = 0, seed = 0;
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
// depth 4, its stencil 1), RGB10A8_2P_XR10's color word (xr.h), else the texel's
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
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t keptBytes = 0;  // a decoded texture's kept blocks: in Stats::textureBytes until destroyBuffer
#endif
};

// Level `level` (1 .. levels - 1) of mipmapped texture `t`'s chain as a render target (target.h: NVK's block-linear
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
    // sampler with the map applied (Device::samplerVariant makes them; the same header where the map changes nothing), and at
    // kLevelZeroMap its header for a texture whose every lod reads level 0 (Texture::levelZero)
    uint32_t mapped[kLevelZeroMap + 1] = {};
    // and its compare header under those maps, which a state sampling a planar texture by comparison reads
    uint32_t compareMapped[kLevelZeroMap + 1] = {};
    // an opaque border's headers by the texel a texture reads at it (borderTexel), where that is not tsc's
    // (Device::borderVariant makes them before the work that samples the texture through the sampler)
    uint32_t bordered[32] = {};
};

// The border texel a draw sampling `t` through `s` reads (sampler.h borderTexel), 0 for a sampler of no opaque border
uint8_t samplerBorderTexel(const Sampler &s, const Texture &t);

// The header a draw sampling `t` through `s` takes: its own, or its border's for `t` (0: not made yet)
inline uint32_t borderedTsc(const Sampler &s, const Texture *t)
{
    if (!s.desc.border || !t)
        return s.tsc;
    const uint8_t texel = samplerBorderTexel(s, *t);
    return texel == ownBorderTexel(s.desc.border) ? s.tsc : s.bordered[texel];
}

}  // namespace gm20b
