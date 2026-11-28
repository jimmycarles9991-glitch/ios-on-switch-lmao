// SPDX-License-Identifier: MIT
// The driver (r7-raw, r8-rawca): a direct MAXWELL_B renderer for what Core Animation's render server draws, with no
// Vulkan, m2v or NVK at run time. Pipelines come from the pack (pack/make_pack.py: NAK's programs and NVK's register values per
// Core Animation state, read off this build's own m2v + NVK at bundle build time). This file turns Metal-shaped calls into
// MAXWELL_B / MAXWELL_DMA_COPY_A methods in a push buffer and submits them through the TegraGM20B user client (kernel.h).
//
// Memory. Everything the driver draws with is imported once and bound at one VA:
//  - textures Core Animation makes, and buffers: pool spans, page-aligned anonymous memory imported once in big chunks;
//  - IOSurface textures: the surface's pages, imported once per surface while a texture over it lives;
//  - no-copy buffers: the caller's pages.
// What the CPU wrote is cleaned to memory before the GPU reads it (CommandBuffer::clean, replaceRegion), what the GPU wrote
// is invalidated before the CPU reads it (TegraGM20BMetal.mm, lazily). The driver's own rings (push, upload, headers, root table)
// are write-combined.
//
// Lifetime. A texture, buffer or sampler is destroyed only once no submission that uses it can still run: TegraGM20BMetal.mm's command
// buffers retain what they use until they complete, as Metal's do. Destroying it frees its header slot, pool span or import at
// once. The device's own memoryless backings and ring spans are retired by their last submission's fence.
//
// Threads. The Device's resource calls take its lock; one CommandBuffer encodes and commits at a time (TegraGM20BMetal.mm serialises).
#pragma once
#include "gm20b_log.h"
#include "lock.h"
#include "dirty_ranges.h"
#include "batch.h"
#include "mme.h"
#include "icb.h"
#include "state_segment.h"
#include "split_capture.h"
#include "split_reuse.h"
#include "split_batch.h"
#include "split_cost.h"
#include "attachment_order.h"
#include "attachment_view.h"
#include "barrier_forms.h"
#include "pass_hazard.h"
#include "buffer_writes.h"
#include "probe_forms.h"
#include "probe_ops.h"
#include "private_comp.h"
#include "event_release.h"
#include "predication.h"
#include "loop_copies.h"
#include "loop_ranges.h"
#include "visibility.h"
#include "fresh.h"
#include "view.h"
#include "depth_bias.h"
#include "sampler_map.h"
#include <algorithm>
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
#include <set>
#include "word_wait.h"
#include <tuple>
#include <utility>
#include <vector>
#include <IOKit/IOReturn.h>
#include <mach/kern_return.h>
#include "comp.h"
#include "counters.h"
#include "attachment_lifetime.h"
#include "own_states.h"
#include "stats.h"
#include "descriptors.h"
#include "encode_profile.h"
#include "use_memo.h"
#include "transfer.h"
#include "fetch_free.h"
#include "fetch_order.h"
#include "kernel.h"
#include "zcull_slots.h"
#include "channel_recovery.h"
#include "fence.h"
#include "host_access.h"
#include "arena_trim.h"
#include "heap.h"
#include "heap_members.h"
#include "heap_tables.h"
#include "variants.h"
#include "device_open.h"
#include "pack.h"
#include "set_entries.h"
#include "stage_textures.h"
#include "program_pages.h"
#include "push.h"
#include "fetch_order.h"
#include "region.h"
#include "sampler.h"
#include "depth_stencil_state.h"
#include "sample_positions.h"
#include "alpha_control.h"
#include "client_errors.h"
#include "zcull.h"
#include "zcull_planes.h"
#include "xr.h"
#include "ycbcr.h"
#include "rotation.h"
#include "viewports.h"
#include "turn_copy.h"
#include "twin_rule.h"
#include "stencil_bits.h"
#include "reach.h"
#include "draw_bands.h"
#include "band_rates.h"
#include "span_marks.h"
#include "draw_capture.h"
#include "view_cache.h"
#include "viewports.h"
#include "fx/fx_mip.h"
#include "surface_image.h"
#include "resources.h"
#include "pipeline.h"
#include "record_layout.h"
#include "pass_desc.h"
#include "draw_refusal.h"
#include "render_encoder.h"
#include "blit_encoder.h"
#include "command_buffer.h"

namespace gm20b {

struct FillStream;

constexpr uint32_t kTextureHeaders = 8192;  // the texture header pool's entries (Device::allocTic)

class ComputeEncoder;
struct ComputeState;
struct ComputeRecord;

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
    bool open(const char *packPath, uint64_t stack, std::string &err, Role role = Role::RenderServer,
              bool asyncPackWindow = false);

    // What made open() fail (device_open.h)
    OpenFailure openFailure() const
    {
        return openFailure_;
    }

    Role role() const
    {
        return role_;
    }

    // The channel and its context, made now if not yet (a client's first command buffer, Order or mipmapped texture); with it
    // the 2D engine's mip-chain self-test. False: the kernel refused it (`channelError()`), and the device submits nothing
    // until the kernel's channel epoch says one may be made (ChannelRefusal).
    bool ensureChannel();

    // Why the kernel last refused the channel or its context (ensureChannel, or begin() making a killed channel again)
    std::string channelError()
    {
        std::lock_guard<gm20b::UnfairMutex> l(channelLock_);
        return channelError_;
    }

    // the pipeline a draw of `pl` with binding facts `facts` (pack.h PackAssumption) draws with (variants.cpp), of its
    // variant table as loaded now, or as the draw loaded it (`table`)
    Pipeline *variant(Pipeline &pl, uint32_t facts);
    Pipeline *variant(Pipeline &pl, const VariantTable *table, uint32_t facts);

    const Pack &pack() const
    {
        return pack_;
    }

    // a storage form may view a compressed attachment (storage_form.h storageReadsCompressed, or its probe's switch)
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

    // A pack pipeline by its state key's FNV-1a (TegraGM20BMetal.mm gm20bPipelineKey), else the record this process made for that
    // key (completed_; packHolds says which); null: neither.
    // caTransform: its vertex function is Core Animation's fixed_vert_l?h_spc (Pipeline::caTransform)
    Pipeline *pipeline(uint64_t keyHash, PackForm form = PackForm::Default, bool caTransform = false);
    // One of the driver's own states by its name (own_states.h: raw.<family>.<fragment>): pipeline()'s, taken
    // with no lock once a lookup placed it; null: no name, no such state in the pack, or one that could not be placed
    Pipeline *ownPipeline(const char *name);

    // A new depth/stencil state's registers into the segment heap
    void placeSegments(DepthStencilState &state)
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        state.place(segmentHeap_);
    }

    bool packHolds(const Pipeline &pl) const
    {
        return gm20b::packHolds(pack_, pl.rec);
    }

    // pipeline()'s of the state whose records are `run` (Pack::fastRun: no search of the pack)
    Pipeline *pipeline(const PackRun &run, uint64_t keyHash, PackForm form, bool caTransform);
    bool fetchFreeReachable(const PackRun &run) const;
    Pipeline *placePipeline(const PackRun &run, uint64_t keyHash, PackForm form, bool caTransform);
    // The state's pack records for the variants its draws pick (PipelineVariantRecord), each placed, by name: null where it
    // cannot be (its programs find no room in the heap)
    std::vector<std::pair<std::string, Pipeline *>> variantPipelines(uint64_t keyHash, bool caTransform);
    std::vector<std::pair<std::string, Pipeline *>> variantPipelines(const PackRun &run, bool caTransform);
    Pipeline *variantPipeline(const PackRun &run, const std::string &name, bool caTransform);
    // The same of a tier-0 state, placed as a tier-0 hit is (no lock, no page read), or nullptr: its record is not in tier
    // 0, takes the device lock to place, or has a storage form's twin (variantPipeline places those)
    Pipeline *variantPipelineLockFree(const PackRun &run, const std::string &name, bool caTransform);
    // A state the runtime made a record of (record.h), drawn from then on as the pack's are; null: its programs find no room
    // in the heap or its samplers none in the header pool (`why`)
    Pipeline *addRecord(const RuntimeRecord &r, bool caTransform, std::string &why, bool *retry = nullptr);
    // The full form (RequestForm::Full, BarrierFull) of a state addRecord placed, by its keyHash: its split and program
    // variants given to that state's pipeline, which may be drawing, and its stride twin's to that state's stride twin.
    // False with `why`: no such state or twin is placed, or one is of another layout than its placed record
    // (record_layout.h), and nothing is given.
    bool attachFullForm(const RuntimeRecord &full, std::string &why);
    // `pl`'s turned variant (TurnedSource) at a turned draw, built at the first; null where the heap or local memory has no
    // room for it: that draw alone is refused (one error line), and the next turned draw places it afresh
    Pipeline *makeTurned(Pipeline &pl);

    // Resources (thread safe). All textures are 2D, one level, one sample, pitch-linear.
    // pool memory, rows 256-byte aligned; `wc`: write-combined GPU-pool memory (a texture the CPU writes with replaceRegion:
    // no clean after), else (or full) pool memory
    // `tiled`: block-linear (TegraGM20BLayout.h) where its shape tiles and its format is a colour target's, whatever `wc`
    // says (the CPU reaches it through replaceRegion and getBytes alone)
    bool createTexture(uint32_t width, uint32_t height, uint32_t format, Texture &t, bool wc = false,
                       bool tiled = false, const ArenaAt *at = nullptr);

    // Before the GPU first renders or stores into the IOSurface (Kernel::surfaceGpuWrites).
    bool surfaceGpuWrites(uint64_t id)
    {
        return id && k_.surfaceGpuWrites(uint32_t(id));
    }

    bool gpuPriority(uint32_t priority, uint32_t background)
    {
        bool critical = false;
        if (!k_.priority(priority, background, &critical))
            return false;
        criticalPriority_.store(critical, std::memory_order_relaxed);
        return true;
    }

    // Its channel is the kernel's critical level (TegraGM20BSchedPolicy.h tgm_compositor): the render server's, or a
    // connection the kernel answered TegraGM20BPriorityCritical for. Nothing preempts it, so its draws are never banded.
    bool compositorClass() const
    {
        return role_ == Role::RenderServer || criticalPriority_.load(std::memory_order_relaxed);
    }

    uint64_t bandBudgetNs() const
    {
#if SWITCH_METAL_DIAGNOSTICS
        return bandBudgetNs_.load(std::memory_order_relaxed);
#else
        return TGM_APP_BAND_NS;
#endif
    }

    uint64_t channelProgressVa() const
    {
        return progressVa_;
    }

    uint32_t nextProgressSeq()
    {
        return ++progressSeq_ ? progressSeq_ : ++progressSeq_;
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
    // it writes it: TegraGM20BLayout.h) has block-linear textures only, at the decided shape (false: another one).
    // `readable`: the bytes past `base` its rows may reach, past allocBytes into the allocation's last page (0: allocBytes).
    // `pooled`: sampled and rendered through a pool image (Texture::sampled) whatever its rows
    bool createSurfaceTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, uint64_t offset,
                              uint32_t width, uint32_t height, uint32_t pitch, uint32_t format, Texture &t,
                              bool writes = false, uint64_t readable = 0, bool pooled = false);
    // A two-plane YCbCr surface's texture of `width` x `height` as `plan` reads its planes, its image one of the pool
    bool createYcbcrTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                            const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t);
    // Its planes moved into a VA window of `end` bytes past the surface's base, the pages past its import sparse
    // (sparse_window.cpp), and `sink` sparse bytes more at its end, from a page of their own; false: the kernel maps no VA
    // sparse, the import is tiled, no VA is left or a bind was refused. closeSparseWindow unbinds and releases it
    // (destroyTexture)
    bool openSparseWindow(Texture &t, uint64_t end, uint64_t sink = 0);
    void closeSparseWindow(Texture &t);
    // `t`'s window a sparse range of `bytes` alone, which reads 0 and drops every store; false: the kernel maps no VA sparse,
    // no VA is left or the bind was refused. closeSparseWindow releases it too
    bool openSparseSink(Texture &t, uint64_t bytes);
    // The pack's conversion state of a YCbCr texture's output (Texture::ycbcr), null if the pack has none
    Pipeline *ycbcrPipeline(uint8_t output);
    // An extended-range texture's stand-in (Texture::standIn), made if the pack has its conversion state
    bool createStandIn(Texture &t, const ArenaAt *at = nullptr);
    // The pack's conversion state of an extended-range texture's stand-in, null if the pack has none
    Pipeline *xrPipeline(XrForm form, bool twoPlanes = false);
    // The pack's resolve state into a target of `format` (samples.h resolveState), null if the pack has none
    Pipeline *resolvePipeline(uint32_t format, uint32_t samples);
    // The pack's state that draws a chain level of `format` from the level above (mip.h mipState), null if the pack has none
    // Whether the pack converts a Depth16Unorm's depth to and from a Depth32Float_Stencil8's at `samples` samples
    // (turn_copy.h turnDepthState)
    bool depthConvertible(uint32_t samples);
    Pipeline *mipPipeline(uint32_t format);
    Pipeline *mip3dPipeline(uint32_t format);                          // mip.h mip3dState
    Pipeline *depthResolvePipeline(uint32_t format, uint32_t filter);  // samples.h depthResolveState
    Pipeline *stencilBitsPipeline(uint32_t format, uint32_t samples, StencilSample pick);    // stencil_bits.h
    Pipeline *stencilColourPipeline(uint32_t format, uint32_t samples, StencilSample pick);  // zeta_colour.h
    // reach.h: `pl` drawing its geometry into a reach target; null: *why
    Pipeline *reachTwin(Pipeline &pl, const char **why);
    // s's header under address map `map` (Texture::addressMap) or kLevelZeroMap, made now if it is not yet
    bool samplerVariant(Sampler &s, uint8_t map);
    // Every constexpr sampler's headers under the address maps, those of samplers placed since made now; false: no room
    bool constexprMapsPlaced();
    // Every constexpr sampler's header under kLevelZeroMap, made before the first work that samples such a texture (so no
    // other header moves); false: no room
    bool constexprLevelZeroPlaced();
    // s's header for sampling `t` at an opaque border (Sampler::bordered), made now if it is not yet
    bool borderVariant(Sampler &s, const Texture &t);

    // Whether address map `map` leaves the modes of constexpr sampler `index` (sampler_map.h); false for no such sampler.
    // Read without lock_ as constexprTsc_ is.
    bool constexprLeaves(uint32_t index, uint8_t map) const
    {
        return index < constexprCount_ && mapLeavesHeader(constexprWord0_[index], map);
    }

    // A two-plane 4:4:4 surface's texture of RGB8_444_2P (542): its planes at `luma`/`chroma` (offsets in the allocation,
    // with their pitches), sampled through an RGBA8 image of the pool (Texture::planar)
    bool createPlanarTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, uint64_t luma,
                             uint32_t lumaPitch, uint64_t chroma, uint32_t chromaPitch, uint32_t width, uint32_t height,
                             Texture &t);
    // A planar texture's stand-in and its planes' render targets (Texture::planarStandIn); false where the pack lacks the
    // stand-in
    // states or a plane cannot be rendered
    bool createPlanarTarget(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, Texture &t);
    // RGB10A8_2P_XR10 or its sRGB twin (`format`, xr.h twoPlaneXr) over a two-plane surface, its planes at `word` and
    // `alpha` (offsets in the allocation, with the bytes a row the surface states for them, which Metal's GPU reads and
    // stores at its own strides: xr_texture.cpp), held in them (Texture::xrPlanes): the planes imported as luma and
    // chroma; `alphaPitch` 0: over a plane alone, chroma a memoryless target whose header is over a sparse range that reads
    // 0 and drops a kernel's stores; false with *why where a plane cannot be one
    bool createXrPlaneTexture(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface, uint64_t word,
                              uint32_t wordPitch, uint64_t alpha, uint32_t alphaPitch, uint32_t width, uint32_t height,
                              uint32_t format, Texture &t, const char **why);
    // A memoryless RGB10A8_2P_XR10 texture's planes, memoryless too (Texture::xrPlanes)
    bool createXrMemoryless(Texture &t);
    // The pack's stand-in states: the planes' codes into the stand-in, or (`store`) the stand-in into the planes
    Pipeline *standInPipeline(bool store);
    // The pack's store state of a YCbCr texture's luma or chroma plane of `store`'s target (pack/ycbcr_store.metal): of 8-bit,
    // 16-bit or packed words, its steps through the sRGB encode or not
    Pipeline *ycbcrStorePipeline(bool chroma, const YcbcrStoreShape &store);
    // The pack's load state of a loaded pass's stand-in of a packed 10-bit plane of `channels` codes a texel
    // (packed_plane.h packedPlaneLoad)
    Pipeline *ycbcrLoadPipeline(uint32_t channels);
    // The pack's states that merge an RGB10A8_2P_XR10 texture's two planes into an RG32Uint image of its texels, or (`split`)
    // the image back into the planes (pack/xr.metal)
    Pipeline *xrPlanesImagePipeline(bool split);

    // pack/srgb_encode.h, which the _srgb store states read at kSrgbEncodeSlot (ycbcr_store.h)
    const Bo &srgbEncodeTable() const
    {
        return srgbEncode_;
    }

    // Over a buffer's memory (t holds no reference: the caller keeps the buffer); `layers` > 1: a linear 2D array, its
    // layers `pitch` x `height` apart under one pitch header of TWO_D_ARRAY
    bool createBufferTexture(const Buffer &b, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch,
                             uint32_t format, Texture &t, uint32_t layers = 1);
    // MTLTextureTypeTextureBuffer: `elements` texels of `format` from `offset` in a buffer, under a texel buffer header
    // (fx::bufferTic), as createBufferTexture holds no reference; an extended-range one with its stand-in
    // (createTexelStandIn). `limit`: the most texels taken (the device's maxTextureBufferWidth answer)
    bool createTexelBuffer(const Buffer &b, uint64_t offset, uint32_t elements, uint32_t format, Texture &t,
                           uint32_t limit = 1u << 27);
    bool createTexelStandIn(Texture &t);
    // A texel buffer over a surface plane's bytes in order (its import, a texel buffer header from `offset`); over a surface
    // the kernel laid out block-linear, a pool of them (createTexelRows), the plane's rows `bytesPerRow` apart
    bool createSurfaceTexelBuffer(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                                  uint64_t offset, uint32_t bytesPerRow, uint32_t elements, uint32_t format,
                                  Texture &t);
    bool createView(const Texture &of, uint32_t format, Texture &t);  // the same memory as another format
    bool createZetaView(const Texture &of, uint32_t format, Texture &t);
    // Its header's sources read through `channels` (MTLTextureSwizzle each, Texture::swizzle), its header written as it is
    void swizzle(Texture &t, const uint8_t channels[4]);
    // A view of part of a block-linear texture: levels [baseLevel, +levels) and layers [baseLayer, +layers) of a chain (an
    // array's, a cube's or a 3D texture's levels), or layers of an array, cube or a depth texture's array, as a view of
    // `type` (fx::kTexOneD to kTexCubeArray, kTexThreeD of a 3D texture alone). Its image is the part's first level and
    // layer, which copies address and a pass renders into as a 2D view of one of each.
    bool createPartView(const Texture &of, uint32_t format, uint32_t type, uint32_t baseLevel, uint32_t levels,
                        uint32_t baseLayer, uint32_t layers, Texture &t);
    // Memoryless (a pass gives it a backing of the pass's layers); the depth/stencil formats among them, as zeta
    // (Texture::zeta). `layers`: a 2D array's or cube's, of one sample
    bool createMemoryless(uint32_t width, uint32_t height, uint32_t format, Texture &t, uint32_t samples = 1,
                          uint32_t layers = 1);
    // A stored depth/stencil texture of one sample over zeta memory another driver of the process keeps (NVK's image: one
    // allocation both drivers render, sample and copy in place), laid out as `m` says, with a texture header of its depth where
    // `sampled`; false where it is not a layout the driver renders (*why says which way). A 2D array or cube of `layers`
    // layers, layerBytes apart, is rendered alone (no header)
    bool createZetaOver(const ZetaMemory &m, uint32_t width, uint32_t height, uint32_t format, bool sampled, Texture &t,
                        const char **why, uint32_t layers = 1);
    // A stored multisampled colour texture (multisample.cpp): the persistent form of a memoryless one's block-linear
    // backing, in memory of its own, which m2v's twin (NVK's image of the same descriptor) lies over in place
    // `compressKind`: compressed memory of that kind where some is ready (TextureAsk::compressKind)
    bool createStoredMultisample(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                                 const ArenaAt *at = nullptr, uint32_t compressKind = 0);
    // A stored depth/stencil texture of `samples` samples in zeta memory of its own (multisample.cpp), with its header,
    // made here (`sampled`: a 2D array, cube or cube array of one sample laid out as that header names its layers). One of
    // `levels` levels is NIL's chain of them in the same zeta memory (its chain; a pass renders level 0).
    bool createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, bool sampled, Texture &t,
                         uint32_t layers = 1, uint8_t type = fx::kTexTwoD, uint32_t levels = 1);
    // The same, laid out to be sampled (texture2d_ms reads it, and a depth resolve's pass samples it): a multisampled one,
    // or one of a single sample in an arena (an MTLHeap's, whose depth is not NVK's image)
    bool createOwnedZeta(uint32_t width, uint32_t height, uint32_t format, uint32_t samples, Texture &t,
                         const ArenaAt *at = nullptr);
    // A Depth32Float_Stencil8 texture's stencil temp (Texture::stencilTemp), made on its first stencil copy
    bool stencilTemp(Texture &t);
    // A 4x colour texture's sample rows (Texture::sampleRows), made on its first copy with a buffer; true for one of other
    // samples, which needs none
    bool sampleRows(Texture &t);
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
                     uint32_t levels = 1, const ArenaAt *at = nullptr, uint32_t compressKind = 0);
    // A mipmapped 2D texture of `levels` levels: level 0 of its block-linear chain (Texture::chain), its header the chain's.
    // False: not one the driver keeps (not a render target format, extended range, no memory).
    bool createMipTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t levels, Texture &t,
                          const ArenaAt *at = nullptr, uint32_t compressKind = 0);
    // A block-compressed 2D texture (or 2D array, `type`) of `levels` levels and `layers` layers in block-linear memory
    // of its own: NIL's image (fx::blockChainLayout), which the copy engine and the CPU (in its view of kind-0xFE
    // memory) address; a 3D one (fx::kTexThreeD) `depth` planes of blocks (fx::volumeLayout's)
    bool createBlockTexture(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers, uint32_t format,
                            uint8_t type, Texture &t, uint32_t depth = 1, const ArenaAt *at = nullptr);
    // A block-compressed 2D texture (or 2D array) over the caller's `bytes` (newTextureWithBytesNoCopy), already laid out as
    // `layout` (fx::blockChainLayout of its texel size, levels and layers), its pages cleaned to memory. At the layout's
    // alignment (block_base.h) the pages are imported under kind 0xFE with a block-linear header of the texel size over
    // them; off it, the texture is a pool image of the layout over which the caller's pages, imported pitch, are copied
    // (callerPages: copyCallerPages fills it). False: not a block-compressed format, `length` short of the layout, or the
    // import, image or header refused.
    bool createBlockTextureOver(void *bytes, uint64_t length, uint32_t width, uint32_t height, uint32_t format,
                                const fx::MipChain &layout, Texture &t);
    // The copy engine's copy of a createBlockTextureOver texture's caller's pages into its image, ahead of the work after it
    // (TextureTransfers::copyBytes); `owner` held until it completed. True with nothing to copy (the texture is the pages).
    bool copyCallerPages(const Texture &t, std::shared_ptr<void> owner);
    // A shared texture over its IOSurface (surface_image.h): the surface's pages imported under its ID and bound 0xFE,
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
    // the level fetches its color or depth through it); `codes`: an sRGB chain's level read as its codes, with no sRGB conversion
    // (pack/mip.metal decodes them); a 3D texture's level as a volume; 0: none
    uint32_t levelTic(const Texture &t, uint32_t level, bool codes = false);
    // The header a fetch from `part` (gm20b::attachmentTarget of level `level`, slice `slice` of `t`, maybe in a format view's
    // format; or one layer of a multisampled texture, its sample grid's header) reads: levelTic's for a 2D chain's level in
    // t's format, else attachmentHeader's, made on first use and freed with t's header. Color parts use the encode view
    // cache; depth parts are prepared at pass admission. 0: none
    uint32_t attachmentTic(const Texture &t, const Texture &part, uint32_t level, uint32_t slice);
    // A header of plain texture `t`'s memory whose word 0 is `word0` (view.h: m2v's view of it in another format
    // of the same texel size; a storage view of a multisampled texture spans its sample grid), made on first use and freed
    // with t's header; 0: no header slot left. The encoding thread's: what it answered is kept there and answered again
    // with no lock (view_cache.h, keyed as viewTics_)
    uint32_t viewTic(const Texture &t, uint32_t word0, ViewAccess access = ViewAccess::Texture, bool asArray = false);
    uint32_t arrayViewTic(const Texture &t);
    // The header of view `v` of `owner`, an attachment's image (v.part) or the attachment itself (attachment_view.h),
    // made on first use and freed with owner's header; 0: none (`why`)
    uint32_t attachmentViewTic(const Texture &owner, const AttachmentView &v, const char **why);
    // `t` with a texture header (every texture the device makes has its own from creation; a pass's part of one gets one
    // here, a turn copy reading it); false: no header slot left
    bool sampleable(Texture &t);
    // The texture headers not held: 0 names why a creation that found its memory made nothing
    uint32_t headersLeft();

    // The memory no resource holds goes back to the kernel: empty chunks of the pool, of images and of buffers, the kept
    // dedicated ones (the kernel keeps their pages until the work submitted before has completed) and the kept surface
    // imports, which it counts
    struct Trimmed {
        size_t keptImports = 0;
        uint64_t keptImportBytes = 0;
    };

    Trimmed trim();
    // The kept surface imports trim would give back now (debug builds' rates line)
    Trimmed keptImports();

    // What the device holds, by kind (debug builds' memory line). `chunks`: the pool's, block-linear and zeta chunks,
    // each mapped whole into the task; of them `pinned`, shared chunks a quarter or less used, which a few live
    // resources keep from going back, and `reuse`, emptied dedicated chunks kept for the next of their size. `heaps`:
    // shared heaps, the task's own pages; `gpuHeaps`: private heaps, kernel BOs no task maps, which its footprint does
    // not count. False: `wait` unset and another thread held the device's lock.
    struct Held {
        uint64_t chunks = 0, pinned = 0, reuse = 0, writeCombined = 0;
        uint64_t heaps = 0, gpuHeaps = 0, volatileHeaps = 0, kept = 0, tables = 0;
        uint32_t chunkCount = 0, pinnedCount = 0;
#if SWITCH_METAL_DIAGNOSTICS
        uint64_t parkedChunks = 0, parkedHeaps = 0;
        uint32_t parkedChunkCount = 0, parkedHeapCount = 0;
#endif
    };

    bool held(Held &h, bool wait);

    // Whether the device's resource lock is free now (try, never waits: the stall line's)
    bool lockFree()
    {
        std::unique_lock<gm20b::Mutex> l(lock_, std::try_to_lock);
        return l.owns_lock();
    }

    void destroyTexture(Texture &t);
    // Private render targets on compressed memory (private_comp.h): made when compressed memory of the size is
    // ready
    bool createCompressedTexture(uint32_t width, uint32_t height, uint32_t format, uint32_t kind, Texture &t);
    // createOwnedZeta's zeta of `bytes` on compressed memory of kind `kind` where some of its size is ready, lock_ held
    bool createCompressedZeta(uint64_t bytes, uint32_t kind, Texture &t);
    bool privateCompressed(const Texture &t);
    // its compressed memory's kind; 0 where it has none (HW Test's probe facts)
    uint32_t privateKind(const Texture &t);
    // t, made plain where createCompressedTexture found no memory of `kind` ready, wants that memory (private_comp.h);
    // `owner`, the facade's texture, is what the submit worker's look hands back; false: t takes none (its layout, a probe)
    bool wantCompressed(const Texture &t, uint32_t kind, void *owner);
    // Whether t's want is still there: no move took it, and nothing withdrew or dropped it
    bool wantsCompressed(const Texture &t);
    // t's want withdrawn, for a CPU copy: t stays on its plain memory for life, as one with a raw view does. The submit
    // worker's, between its submissions, where no move of t is under way
    void keepPlain(const Texture &t);

    // t as it is between the submit worker's moves of its memory, which write it under lock_ (a debug probe's read)
    Texture lockedCopy(const Texture &t)
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        return t;
    }

    // The submit worker's, before a raw range, with no lock: whether memory made since its last look may serve a want
    bool privateMoveDue() const
    {
        return privateMoveDue_.load(std::memory_order_relaxed);
    }

    // Every Shared span freed since is zeroed at the front of the next command buffer begin() opens (scrubs.cpp). With
    // no command buffer coming (the idle give-back, before trim), the submitting thread makes one for them: true with
    // its job in `seqno` and `generation` (0: none was due); then scrubWait, on the caller's own thread, waits for it and
    // puts its spans back in their chunks.
    bool scrubFreed(uint32_t &seqno, uint64_t &generation);
    void scrubWait(uint32_t seqno, uint64_t generation);

    bool scrubDue() const
    {
        return scrubDue_.load(std::memory_order_relaxed);
    }

    // The idle give-back's (memory pressure, the app gone to the background): the kernel's spares too
    void kernelSparesGiveBack()
    {
        k_.sparesGiveBack();
    }

#if SWITCH_METAL_DIAGNOSTICS
    // HW Test's ui.glass_sidebar (the idle-reclaim switch): the compressed-memory thread's next trim runs as it would
    // once the process had committed nothing for kCompIdleNs, so a test need not wait the idle reclaim out
    void reclaimIdleNow();
#endif

    // The submit worker's look, where privateMoveDue: the flag cleared, and `take(owner)` for each texture ready memory
    // serves now, called under lock_, where an owner whose want is there is not freed yet (destroyTexture erases it)
    void privateMoves(void (*take)(void *owner));
    // The submit worker's, inside its submission with no command buffer open: t onto the compressed memory it wants
    bool compressedFromPlain(Texture &t);

    uint64_t privateMade() const
    {
        return privateMade_.load(std::memory_order_relaxed);
    }

    TextureTransferResult transferTexture(const Texture &texture, uint32_t level, uint32_t slice,
                                          const uint32_t origin[3], const uint32_t size[3], void *bytes, uint64_t row,
                                          uint64_t image, bool write, std::shared_ptr<void> owner);
    // `wc`: from the write-combined GPU pool (Buffer::wc), else (or full) pool memory as before; `hostVisible`: the CPU
    // reads or writes it (Buffer::hostVisible), else it takes no CPU lease; `gpuVolatile`: bind WC memory VOL=1 in its own
    // chunk pool so CPU writes never trigger whole-L2 invalidates and GPU writes flush FB without L2_FLUSH_DIRTY
    bool createBuffer(uint64_t length, Buffer &b, bool wc = false, bool hostVisible = true, bool gpuVolatile = false,
                      bool zero = true);

    // The write-combined chunks held (non-dedicated) and their bytes (test/poolcheck.cpp: they follow what is in them)
    uint32_t wcChunksHeld();
    uint64_t wcChunkBytesHeld();
    // The free bytes of the block-linear chunks (test/poolcheck.cpp: a chunk's worth kept ahead)
    uint64_t blChunkFreeBytes();
    // The thread importing surface `id` at `base` (its mach port name), 0 when none is (test/device_import_test.cpp)
    uint32_t surfaceImporter(uint64_t id, void *base, uint64_t bytes);
    // This process's footprint and its bytes to its memorystatus limit, for a client's grow-ahead (grow_ahead.h); a test
    // gives its own
    std::function<bool(uint64_t &footprint, uint64_t &available)> footprintProbe = readFootprint;

    bool growerRunning()
    {
        std::lock_guard<gm20b::Mutex> l(lock_);
        return grower_.joinable();
    }

    // the shader heap's bound bytes of host memory (the programs copied so far, in steps)
    uint64_t heapBytes() const
    {
        ensurePackWindow();
        return heapBound_.load(std::memory_order_relaxed) - heapCopiesAt_;
    }

    // the pack's and the shader compiler's store's pages bound in the program window, where larger programs run in place
    uint64_t heapInPlaceBytes();

    uint64_t packWindowVa() const
    {
        ensurePackWindow();
        return packWindowVa_;
    }

    uint64_t packWindowBytes() const
    {
        ensurePackWindow();
        return packWindowBytes_;
    }

    const uint8_t *packWindowCpu() const
    {
        ensurePackWindow();
        return packWindowCpu_;
    }

    bool packWindowProgram(uint32_t prog, uint32_t &program, uint64_t &data) const;

    // Where the shader compiler's program slots are, as this process maps its store (TegraGM20BMetalComplete.mm): a slot's bytes found
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

    // The caller's pages as a buffer (newBufferWithBytesNoCopy). False where the kernel refuses the import, or where the
    // pages lie in a surface this task imported block-linear (`tiled`): they hold its texels in GOBs, no buffer's rows
    bool createBufferNoCopy(void *bytes, uint64_t length, Buffer &b, bool &tiled);
    // lock_ held: [bytes, bytes + length) reaches into this device's chunks, which no import may take (one line, naming
    // `what`)
    bool ownChunkMemory(const void *bytes, uint64_t length, const char *what) const;
    // An IOSurface's pages as a buffer of `length` bytes (newBufferWithIOSurface:), sharing its textures' import. False
    // where the kernel refuses the import, or keeps the surface block-linear (`tiled`): its bytes are no pitch buffer then
    bool createSurfaceBuffer(uint64_t id, void *base, uint64_t length, const void *owner, Buffer &b, bool &tiled);
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

    // MTLHeaps (arena.cpp): an arena is one GPU VA reservation bound whole to host pages of its own, under
    // TGM_LAYOUT_KIND for a private heap's block-linear textures or pitch for a shared heap's textures and buffers, its
    // resources placed in it (heap.h). Purged, its pages go back and its VA and CPU range stay reserved, so restoring it
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
    void makeArenaAliasable(Span &span, Lease *&lease);
    void makeArenaAliasable(Texture &t);
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
    bool setArenaVolatile(uint32_t arena, bool isVolatile);  // whether its pages had gone (arena_purge.h)
    bool setArenaVolatileIfPurged(uint32_t arena);
    bool hasBoundVolatileArena();
    void purgeArena(uint32_t arena, const std::atomic<uint32_t> &committed);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    struct PurgeStats {
        uint32_t purged = 0;
        uint64_t purgedBytes = 0;
        uint32_t skipped = 0;
        uint64_t skippedBytes = 0;
        uint32_t committed = 0;
        uint32_t committedCount = 0;
        uint32_t leases = 0;
        uint32_t inFlight = 0;
        uint32_t busy = 0;
        uint32_t nonEmpty = 0;
    };

    void purgeArenas(const std::atomic<uint32_t> &committed, PurgeStats *stats = nullptr);
#else
    void purgeArenas(const std::atomic<uint32_t> &committed);
#endif
    bool arenaPurged(uint32_t arena);
    // Fresh pages where it was (their contents zero); false: no memory, it stays purged
    bool restoreArena(uint32_t arena);
    // The idle give-back of the arenas kept non-volatile: as purgeArenas judges a purge (nothing in flight or committed, no
    // m2v view), the ranges no resource is placed in go back (arena_trim.h); their contents stay
    void trimArenas(const std::atomic<uint32_t> &committed);

    uint64_t arenaBytes();  // bound: purged arenas' are not
    // Of arenaBytes, what their resources are placed in (the rest holds no resource: what an idle give-back could return
    // of a heap kept non-volatile) and what is in heaps made volatile
    void arenaUse(uint64_t &placed, uint64_t &volatileBytes, uint64_t *givenBack = nullptr);

    uint32_t purgedArenas() const
    {
        return purgedArenas_.load();
    }

    bool createSampler(const SamplerDesc &d, Sampler &s);
    void destroySampler(Sampler &s);

    // m2v's handle tables and heap (heap_tables.h): texture header `tic` now names `texture`, sampler header `tsc`
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

    // Which texture an image array of a heap holds (the facade's: heapHolds, heap_fit.h), before any ID is named
    void heapHolds(HeapHolds holds)
    {
        heapHolds_ = holds;
    }

    // The first pool chunk made now (its pages wired and imported), off the render thread's first texture (metal_prepare),
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

    // The channel an encode began on (channelGeneration() then) is the one begin() would hand out now
    bool channelCurrent(uint64_t generation) const
    {
        return channelGeneration() == generation && !contextless_.load(std::memory_order_relaxed) &&
               !k_.lost(channel_.load(std::memory_order_acquire));
    }

    uint32_t completed() const
    {
        return __atomic_load_n(fence_.load(std::memory_order_acquire), __ATOMIC_ACQUIRE);
    }

    // 0 before the channel is made (it may be made on another thread: a client's first mipmapped texture)
    uint32_t submitted() const
    {
        return channelReady_.load(std::memory_order_acquire) ? lastSubmitted() : 0;
    }

    bool done(uint32_t seqno) const
    {
        const volatile uint32_t *fence = fence_.load(std::memory_order_acquire);
        return fence == &kNoFence || (int32_t(__atomic_load_n(fence, __ATOMIC_ACQUIRE) - seqno) >= 0 &&
                                      semanticFenceAt(fence, seqno).status != SemanticFenceStatus::Pending);
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
#if SWITCH_METAL_DIAGNOSTICS
    // The PTIMER value its submission started at, while its slot still holds its stamps
    bool gpuStartUnits(uint32_t stamp, uint64_t &units) const;
#endif
    // a committed command buffer's measured draws, kept until the GPU has run them (band_rates.h)
    void noteCommittedTimings(std::vector<DrawTiming> &timings);
    // the measured draws the GPU has run, each teaching its state's rate: each command buffer's begin calls it
    void learnCompleted();
#if SWITCH_METAL_PROF
    SpanMarks spans_;
    // The render server's completed command buffer's spans of kLongSpanNs or more (span_marks.h), which the facade logs
    // and notes to the kernel for HW Test ui.launch_hitch
    std::vector<StateSpan> longSpans(uint32_t stamp);
#endif
#if SWITCH_METAL_DIAGNOSTICS
    // The frame-weights capture (draw_capture.h): asked for from any thread, its command buffers taken at begin() and
    // handed over at commit, read back by every completer (reportDrawTimes)
    DrawTimes drawTimes_;
    Bo drawTimeMarks_;
    bool countingInvocations_ = false;  // a capture's counter still runs: the next begin() stops it
    void askDrawTimes(uint32_t buffers);
    bool openDrawTimes();
    void beginDrawTimes(CommandBuffer &cb, Push &p);
    void emitDrawTime(Push &p, uint32_t index);
    void committedDrawTimes(CommandBuffer &cb, uint32_t seqno);
    void reportDrawTimes();
    DrawTimesTaken drawTimesTaken();

    int kernelDebugCall(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
    {
        return k_.debugCall(method, in, inSize, out, outSize);
    }

    uint32_t probeChannel()
    {
        return ensureChannel() ? channel_.load(std::memory_order_acquire) : 0u;
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

    // A writer promise (TegraGM20BJobABI.h method 34) of the device's channel for the IOSurface imports `handles`, kept by
    // the job that names it (CommandBuffer::promises) or cancelled
    bool promise(const std::vector<uint64_t> &handles, uint32_t &id, int *result = nullptr)
    {
        if (!ensureChannel())
            return false;
        const uint32_t ch = channel_.load(std::memory_order_acquire);
        const int kr = ch ? k_.promise(ch, handles, id) : kIOReturnNotReady;
        if (result)
            *result = kr;
        return kr == KERN_SUCCESS;
    }

    // withdrawn: its command buffer failed, so the promised writes never come (TegraGM20BPromiseWithdraw). The
    // kern_return_t.
    int cancelPromise(uint32_t id, bool withdrawn = false)
    {
        return k_.cancelPromise(id, withdrawn);
    }

    // The holder's bound on the promises of `mask` (TegraGM20BPromiseArm), `ns` from now (`fails`: their command buffer
    // fails there): the kern_return_t
    int armPromises(uint32_t mask, uint64_t ns, bool fails)
    {
        return k_.armPromises(mask, ns, fails);
    }

    // A failed command buffer's writes of the IOSurface imports `handles` that no promise names, withdrawn: the
    // kern_return_t
    int withdrawWrites(const std::vector<uint64_t> &handles)
    {
        return k_.withdrawWrites(handles);
    }

    // TegraGM20BJobABI.h method 40 on this device's channel: the kern_return_t
    int eventWord(uint64_t handle, uint64_t &address, uint64_t &base, uint32_t &floor)
    {
        if (!ensureChannel())
            return kIOReturnNotReady;
        const uint32_t ch = channel_.load(std::memory_order_acquire);
        return ch ? k_.eventWord(handle, ch, address, base, floor) : kIOReturnNotReady;
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

    // HW Test's gpu.x1.interlock_affinity: what bounds the affinity bytes the interlock's ticket words reach
    // (nak_nir_lower_fs_interlock.c interlock_ticket_word)
    uint64_t interlockTicketBytes() const
    {
        return tickets_.bytes;
    }
#endif

    // A job of no pushes that carries event waits and signals (at most TegraGM20BMaxJobEvents together): the kernel makes the
    // channel acquire each wait's producer, and releases the signals once every earlier submission of this channel
    // completed. Called where submissions are (serialised). `generation`: the channel the caller checked its waits'
    // producers on, 0 for none. A channel made again since has not run them, so the job is not submitted: false with
    // kIOReturnNotResponding, and the caller checks again.
    bool eventJob(const JobEvents &events, uint32_t &seqno, int *result = nullptr, uint64_t generation = 0)
    {
        setView_.refused.clear();
        if (events.empty() || events.waitCount + events.signalCount > TegraGM20BMaxJobEvents || !ensureChannel())
            return false;
        Killed live = {};
        if ((contextless_ || k_.lost(channel_.load(std::memory_order_acquire)) || liveKill(live)) && !reopen())
            return false;
        if (generation && generation != channelGeneration()) {
            if (result)
                *result = kIOReturnNotResponding;
            return false;
        }
        SurfaceList none;
        uint32_t seq = 0;
        if (!job(setView_, channel_.load(std::memory_order_acquire), nullptr, 0, none, JobVisibility::External, seq,
                 result, events))
            return false;
        submittedThrough(seq);
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
#if SWITCH_METAL_DIAGNOSTICS
        std::atomic<uint64_t> commits{0}, pushBytes{0}, uploadBytes{0}, ringWaits{0}, ringsOutgrown{0}, imports{0},
            importBytes{0}, poolBytes{0}, poolChunks{0}, cleanBytes{0}, poolStalls{0};
        std::atomic<uint64_t> textureUploads{0}, textureReadbacks{0}, fusedPasses{0};
        // uncompressed backings held now and their memory (a memoryless one's shared plain memory, a shadow's own), and the
        // retired ones among them and the free bytes of the block-linear and zeta chunks as trimBackings last counted them
        std::atomic<uint64_t> backings{0}, backingBytes{0}, retiredBackings{0}, tiledFreeBytes{0};
        // surface imports made (a kernel import each) and found live, since open
        std::atomic<uint64_t> importsMade{0}, importHits{0};
        // heaps' arenas made and purged ones bound again, and the bytes of untouched host pages imported (a shared heap's),
        // each zero-filled by the kernel's wiring on the importing thread, since open
        std::atomic<uint64_t> arenasMade{0}, arenaRestores{0}, hostZeroedBytes{0};
        std::atomic<uint64_t> parkedChunks{0}, parkedChunkBytes{0}, chunkParks{0}, chunkUnparksResident{0},
            chunkUnparksReclaimed{0};
        std::atomic<uint64_t> parkedHeaps{0}, parkedHeapBytes{0}, heapParks{0}, heapUnparksResident{0},
            heapUnparksReclaimed{0};
        // the memory live textures took of their own and their kept blocks' (Texture::heldBytes, Buffer::keptBytes)
        std::atomic<uint64_t> textureBytes{0};
        // Shared spans zeroed at the front of a command buffer, and the command buffers that carried them (scrubs.cpp)
        std::atomic<uint64_t> scrubbedBytes{0}, scrubbedRuns{0};
        // private textures made in plain memory and moved into compressed memory later (a copy-engine job on the
        // submit worker each), and their bytes
        std::atomic<uint64_t> compressedMoves{0}, compressedMoveBytes{0};
        // diagnostics: color targets the driver rendered whose address or pitch is not a multiple of 128 (NVK renders
        // those through a tiled shadow: nvk_cmd_draw.c nvk_image_plane_aligned_for_linear_attachment), and the last one's pitch
        std::atomic<uint64_t> rtUnaligned{0};
        // color targets in rows the driver rendered through a block-linear shadow, beside a block-linear one in their pass
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
        // passes that drew RGB10A8_2P_XR10 in its two planes (xr_pass.h), the planes those rendered through a shadow, and
        // the draws' binds of a two-plane texture's header pair; the copy engine's merges and splits of a one-image form
        std::atomic<uint64_t> xrPlaneShadows{0}, xrPairBinds{0};
        // passes into a chain level
        std::atomic<uint64_t> levelPasses{0};
        // pack programs placed in the shader heap, and states not made because it was full
        std::atomic<uint64_t> programsPlaced{0}, heapFull{0};
        // program slots run in place: the pack's in grains bound already, the store's slots placed, and slots cleaned
        // (Device::placeInPlace)
        std::atomic<uint64_t> packSlotsInBoundGrains{0}, storeSlotsPlaced{0}, slotCleans{0};
        // framebuffer-fetch barriers encoded, and those left out where the draw's pixels meet none a color write reached
        // since the last barrier (region.h); the barriers one per fetching draw after any color write would have encoded,
        // and those the interlocked storage form would (RenderEncoder::countRules, debug builds)
        std::atomic<uint64_t> fetchBarriers{0}, fetchBarriersElided{0}, fetchBarriersAnyColor{0},
            fetchBarriersInterlocked{0};
        // Clear loads of colors, and those left out because the pass's first draw covers them (RenderEncoder::clears); clears
        // narrowed to the box of the draws that alone see them, and the render area's pixels those left uncleared
        // (clear_box.h)
        std::atomic<uint64_t> clears{0}, clearsElided{0}, clearsNarrowed{0}, clearPixelsLeft{0};
        FetchCounts fetch;
        // compressed memory held now (comp.h CompMem and made buffers no backing took) and its bytes, the backings on it
        // now; buffers made and the kernel's refusals since the device opened; full-target clears of compressed targets to a
        // value the boot ZBC table holds (no texels written) and to one it does not
        std::atomic<uint64_t> compHeld{0}, compHeldBytes{0}, compViews{0}, compMade{0}, compFailed{0}, zbcHits{0},
            zbcMisses{0};
        // private textures made plain for want of compressed memory and moved onto it since (Device::compressedFromPlain)
        std::atomic<uint64_t> privateCompressedMoves{0};
        // passes whose memoryless attachment wanted compressed memory not made yet, and took plain memory
        std::atomic<uint64_t> compMisses{0};
        // ZBC entries this process added, those a full table refused, and full clears of compressed targets to a value no
        // entry can hold (zbc.h)
        std::atomic<uint64_t> zbcAdded{0}, zbcFull{0}, zbcInexact{0};
        // a shadow's fills and stores whose image was compressed memory (the copy engine through a compressible kind), and
        // the shader depth tests' copies of a compressed zeta
        std::atomic<uint64_t> compCopies{0}, compZetaCopies{0};
        // compute launches encoded (compute.h)
        std::atomic<uint64_t> dispatches{0};
        // states made with the zero-source pixel kill on (blend_kill.h), and their binds encoded; prebuilt states whose
        // pack segment pushes another kill than the runtime's rule gives
        std::atomic<uint64_t> blendKillStates{0}, blendKillBinds{0}, blendKillPackDisagreements{0};
        // draws of a vertex amplification count above 1 (each drawn once per amplification: amplification.cpp); draws
        // that wrote a state's SlotGuestBase holes (a rebind, or a hole's slot bound again)
        std::atomic<uint64_t> amplifiedDraws{0}, guestHoleWrites{0};
        std::atomic<uint64_t> fillTwod{0};               // fillTexture: regions by the 2D engine (twod_fill.h)
        std::atomic<uint64_t> resolveTwod{0};            // MSAA resolves by the 2D engine (resolve_twod.cpp)
        std::atomic<uint64_t> resolveExact{0};           // those through the grid's complement (the probe's)
        std::atomic<uint64_t> resolveTwodCompressed{0};  // those whose sample grid has comptags
        // MSAA resolves by route, 2x and 4x: resolve.twod, the probe's complement, the pack's pass (index 2: other
        // sample counts)
        std::atomic<uint64_t> resolveRoute[3][3] = {};
        // passes given a zcull region, and the regions a draw's state invalidated (zcull.h)
        std::atomic<uint64_t> zcullPasses{0}, zcullInvalidations{0}, zcullLoads{0};
        // what gpu.perf.knobs' and gpu.perf.nvn's probes wrote (nvn_probes.h): passes or draws, discards
        std::atomic<uint64_t> nvnBoundsOpened{0}, nvnStencilPasses{0}, nvnFormatPasses{0}, nvnDiscards{0},
            subtileWrites{0}, zcullRulesKept{0};
        // useResidencySet: calls, and the host-visible allocations of used sets a range's seal named
        // (gm20bNameResidencySets)
        std::atomic<uint64_t> residencySetUses{0}, residencyNamed{0};
        // of those loads, the regions an earlier range of the command buffer stored; STORE_ZCULL words made stores and
        // left NO_OPERATIONs (no pass loaded their region); regions parked for a range that found another job submitted
        // first
        std::atomic<uint64_t> zcullCarried{0}, zcullStores{0}, zcullUnstored{0}, zcullCarriesLost{0};
        // stencil plane draws encoded: the eight-draw route's (stencil_bits.h) and the one-draw route's
        // (zeta_colour.h)
        std::atomic<uint64_t> stencilBitDraws{0}, zetaColourDraws{0};
        // draws of states with a split that took their twin, and those that did not by why (RenderEncoder::splitTwin)
        std::atomic<uint64_t> splitTwins{0}, splitWhole[size_t(SplitWhole::Count)]{};
        std::atomic<uint64_t> splitPrologues{0};         // split draws' prologue launches (split.cpp)
        std::atomic<uint64_t> reopenTicks{0};            // the last reopen that made a channel, mach_absolute_time
        std::atomic<uint64_t> bandedDraws{0}, bands{0};  // draw_bands.h
        // mixed passes' draws of MixedDraw::Covering, and of MixedDraw::Split (samples 1-3 with the pixel stage off)
        std::atomic<uint64_t> mixedCovering{0}, mixedStageOff{0};
        std::atomic<uint64_t> learnedDraws{0};      // band_rates.h
        std::atomic<uint64_t> bandedDispatches{0};  // dispatch_bands.h
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        // draws large enough to measure, priced by the static estimate or a learned rate, and those drawn whole
        // without a band decision (fetchDraw, wholeDraw)
        std::atomic<uint64_t> pricedStatic{0}, pricedLearned{0}, unpricedLarge{0};
        std::atomic<uint64_t> splitLaunches{0};
        // split twins that found no room in the heap or local memory at a draw that took them
        std::atomic<uint64_t> splitRoomRefusals{0};
        std::atomic<uint64_t> interlockDraws{0}, storageBarriers{0};
        // draws of a state whose raster order groups run in the pixel interlock (PipelineRasterOrdered)
        std::atomic<uint64_t> rasterOrderedDraws{0};
        // passes committed that opened with PIXEL_SHADER_BARRIER in place of the drain (barrier_forms.h passOpening)
        std::atomic<uint64_t> passPixelOpenings{0};
        // state segment binds, the inline words they replaced, a pass's first binds of the segment the channel still held
        // from the pass or command buffer before (no words; state_segment.h), and passes that could not keep it because
        // their prologue set back the last state's extras (Pipeline::restore)
        std::atomic<uint64_t> segmentBinds{0}, segmentWords{0}, segmentSkips{0}, segmentExtrasLosses{0};
        // binds of a state or depth/stencil state that has a segment, copied into the push ring instead
        std::atomic<uint64_t> segmentCopies{0};
#if SWITCH_METAL_PROF
        // binding slots the encode compared applying recorded state, and the slots of the tables it compared
        // (TegraGM20BMetalPlanApply.h): what a comparison of every slot of each changed table looked at
        std::atomic<uint64_t> slotsCompared{0}, slotsInTables{0};
        EncodeTotals encode;
#endif
#endif
#else
        std::atomic<uint64_t> heapFull{0};
#endif
        // channels made again after the kernel killed one
        std::atomic<uint64_t> reopens{0};
    } stats;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // hw_test_app's hardware probes (TegraGM20BMetalProbe.mm): push words a draw of one instance emits after its state and before
    // BEGIN, and after its END the words that set back what those set, while armed
    struct ProbeWords {
        std::vector<uint32_t> before, after;
        // the registers its words write that the draw sets back after it (probe_restores.h): kProbeRestores' entries.
        // Whatever they write, the raster masks' held word is unknown after it, so the next draw writes them again.
        uint32_t restores = 0;
    };

    std::atomic<bool> probeArmed{false};
    // generateMipmaps of a format the pack's mip state makes, every step on the 2D engine as fx_mip_probe.h's surface
    // format, recorded from now
    std::atomic<bool> probeMipCandidates{false};
    // memoryless zetas the copy engine fills and drains for the shader's depth tests take their compressible kind (comp.h)
    std::atomic<bool> probeZetaKinds{false};
    // stencil planes take stencil_bits.h's eight draws, not the one-draw route (zeta_colour.h)
    std::atomic<bool> probeStencilBits{false};
    // the one-draw route's colour ROP over the zeta's own pages, untiled, not through its pool image (zeta_colour.cpp)
    std::atomic<bool> probeZetaDirect{false};
    // While set (HW Test's zcull and compression probes, zcull-comp LIST 15, 18, 32): a stored Depth32Float 2D array
    // takes its stored zeta kind whatever its layers (comp.h storedZetaKind); a pass's zeta shadow (a colour texture as
    // depth, or a zeta shorter than a layered pass) is backed compressed and its compressed zeta copied undecompressed;
    // a nonzero criterion (SFUNC, SREF and SMASK in bytes 0 to 2) replaces the region's SET_ZCULL_CRITERION of NEVER;
    // a nonzero probeTir is the row a pass of one-sample colour beside a four-sample depth writes (mixed_samples.h
    // kTirProbeZcull .. TirEnd)
    std::atomic<bool> probeZetaLayers{false}, probeZetaShadow{false};
    std::atomic<uint32_t> probeZcullCriterion{0}, probeTir{0};
    std::atomic<uint64_t> probeEmitted{0};  // draws that emitted them since they were armed
    // hw_test_app's gpu.sched.long_draw_bands: the band budget its unbanded arm sets to 0 (TegraGM20BMetalProbe.mm)
    std::atomic<uint64_t> bandBudgetNs_{TGM_APP_BAND_NS};
    std::atomic<bool> probeMacroBands{false};
    // Shared buffers made while set are bound L2-bypassing (volatileWcChunks_), for gpu.perf.rates' cached-against-volatile arms
    std::atomic<bool> probeSharedBypassesL2{false};
    // nonzero: each render pass's prologue writes it to SET_SUBTILING_PERF_KNOB_A (gpu.perf.knobs)
    std::atomic<uint32_t> probeSubtilingKnob{0};
    // nvn_probes.h's bits: the NVN state gpu.perf.nvn A/Bs, for passes encoded from now on
    std::atomic<uint32_t> probeNvn{0};
    // zcull rules gpu.perf.nvn's zcull_rule arms skip (zcull.h kZcullKeep*)
    std::atomic<uint32_t> probeZcullKeep{0};
    // comp.h probeMs4Cbr turned off: its kinds' wants go and their made memory is freed
    void dropProbeCompressedKinds();
    gm20b::Mutex probeLock;
    std::shared_ptr<const ProbeWords> probeWords;

    std::shared_ptr<const ProbeWords> armedProbe()
    {
        if (!probeArmed.load(std::memory_order_relaxed))
            return nullptr;
        std::lock_guard<gm20b::Mutex> hold(probeLock);
        return probeWords;
    }

    // the barrier forms of the command buffer begin() starts next (probe_forms.h), set by its encoder
    ProbeForms nextForms;
    // gpu.x1.job_boundary_idle's header cell: texture `a`'s header describes `to`'s image from the next submission's
    // header invalidate on, or its own again (`to` null); false where either has no header
    bool probeRetarget(const Texture &a, const Texture *to);
    // the pipelines made while set bind their programs' shader data at its exact size (cbuf.h): HW Test's bench
    // measuring what the chip reads of a constant buffer bound off a 256-byte multiple
    std::atomic<bool> probeExactCbufs{false};
    // backings made while set take no compressible memory, for a probe's uncompressed reference
    // (TegraGM20BMetalProbeKinds.mm switchMetalProbePlainMemory:)
    std::atomic<bool> probePlainMemory{false};
    // the next createTexture binds compressible memory of its own (comp.h kinds) where it makes a block-linear
    // target, which probeCompressed holds until destroyTexture; any other createTexture takes the flag and stays plain
    std::atomic<bool> probeCompressNext{false};
    // split draws the capture still takes (split_capture.h); 0: disarmed
    std::atomic<uint32_t> splitCaptureLeft{0};
    // private textures stay plain while set (private_comp.h), for a probe's uncompressed reference
    std::atomic<bool> probePlainPrivate{false};
    std::atomic<bool> probeResolveDraw{false};  // resolves take the pack's resolve pass, never resolve.twod
    // resolves of resolve.twod's shape go through the grid's complement, readable ones too (resolve_rule.h)
    std::atomic<bool> probeResolveExact{false};
    // storage forms view compressed attachments and storage-bound memoryless backings compress (gpu.gm20b.storage_compressed)
    std::atomic<bool> probeStorageCompressed{false};
    // A fetching draw whose state has a fetch-free form of this rung takes it whatever its rule (2, 3), or keeps the storage
    // form (4); a blended storage form's barrier twin (5 a barrier a triangle, 6 one a draw: fetch_rungs.h); 0: the
    // rules decide (fetch_free.h fetchFreeTaken)
    std::atomic<uint8_t> probeFetchRung{0};
#if SWITCH_METAL_DIAGNOSTICS
    // HW Test's wait for the compression thread (TegraGM20BMetalProbe.mm switchMetalProbeCompressionSettled:): `settled` runs once
    // nothing asked of compThread_ is left, each compressed memory and each ZBC entry made or refused, on that thread with
    // lock_ dropped; at once, on the caller's thread, when none is asked
    void compWhenSettled(std::function<void()> settled);
#endif
    std::map<uint64_t, Bo> probeCompressed;  // lock_
    std::atomic<uint64_t> unflushed_{0};     // signalsUnflushed
#endif

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // The fetch barrier's methods, set by the context-switch stress (test/fetchform.cpp) and the ceiling measurement
    // (TegraGM20BMetal.mm's debug notifications): PIXEL_SHADER_BARRIER and 0x0f74 (the shipped form), WAIT_FOR_IDLE and
    // INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI (the form before it, the stress's reference), PIXEL_SHADER_BARRIER alone, which
    // tells a hang's method apart, and none at all. PixelShaderOnly and None let a fetching draw read colors its
    // predecessors have not finished writing: measurements of what the barrier costs, never shipped. Tiles is deko3d's
    // DkBarrier_Tiles (TILED_CACHE_BARRIER, then the pixel-ordered texture invalidate), which orders a tile's earlier
    // pixels without draining the binned batch: the A/B of learn/deko3d.md probe P1, judged by the device
    enum class FetchBarrier : uint8_t {
        PixelShader,
        WaitForIdle,
        PixelShaderOnly,
        None,
        Tiles
    };

    std::atomic<FetchBarrier> fetchBarrier{FetchBarrier::PixelShader};
    static const char *fetchBarrierName(FetchBarrier b);
#endif

private:
    friend class CommandBuffer;
    friend class RenderEncoder;
    bool createOnePlaneYcbcr(uint64_t surfaceId, void *base, uint64_t allocBytes, const void *surface,
                             const YcbcrPlan &plan, uint32_t width, uint32_t height, Texture &t);
    // A YCbCr texture's image header as `output` reads it (`packedCodes`: a packed plane's codes a texel)
    void ycbcrImageHeader(Texture &t, uint8_t output, uint32_t packedCodes);
    // Measured draws' stamp slots taken (stamps.h kDrawStampSlots) and the committed ones not yet learned: the encoding
    // thread's (begin, the encoders, commit), which the submit worker's order makes one thread at a time, so plain
    uint32_t drawStamps_ = 0;
    std::vector<DrawTiming> committedTimings_;
    // A block-compressed image of `layout` at t.va (lock_ held, t.tic allocated): its header of `type` and, past one level,
    // its chain
    void layBlocks(const fx::MipChain &layout, const FormatInfo *f, uint32_t width, uint32_t height, uint8_t type,
                   Texture &t);
    // An array, cube or 3D image of `layout` at t.va (lock_ held, t.tic allocated) under a header of `type`; past one level,
    // level 0 of its chain (Texture::chain), the header every level's
    void layImage(const fx::MipChain &layout, const FormatInfo *f, uint8_t type, Texture &t);
    friend class BlitEncoder;
    friend class ComputeEncoder;
#ifdef SWITCH_METAL_HARNESS_MARKS
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

    enum class ChunkPark : uint8_t {
        Live = 0,
        Parking,
        Parked,
        Unparking,
    };

    enum class ChunkVec : uint8_t {
        Pool = 0,
        Bl,
        Zeta,
        Wc,
        VolatileWc,
    };

    struct Chunk {
        Bo bo;
        void *memory = nullptr;
        uint64_t bytes = 0;
        std::map<uint64_t, uint64_t> free;  // offset -> bytes, coalesced
        uint64_t used = 0;
        uint64_t highWater = 0;
        bool dedicated = false;
        uint64_t pitchVa = 0;  // a block-linear chunk's kind-0 view (blPitchVa), made at its first ask
        // a pool chunk a host-visible buffer was placed in: the CPU writes such a buffer outside any lease, so lines
        // it left may be dirty under a later owner of its span (Device::createBuffer)
        bool hostTouched = false;
        ChunkPark park = ChunkPark::Live;
        uint64_t parkedVa = 0;
    };

    struct ParkCandidate {
        ChunkVec vec = ChunkVec::Pool;
        uint32_t zetaIndex = 0;
        uint32_t index = 0;
        Bo bo;
        uint64_t bytes = 0;
        uint64_t pitchVa = 0;
        uint64_t mapped = 0;
        bool dedicated = false;
        bool parked = false;
    };

    static bool chunkSlotOccupied(const Chunk &ch)
    {
        return ch.memory != nullptr || ch.bo.handle != 0 || ch.park != ChunkPark::Live;
    }

    std::vector<Chunk> &chunkVec(ChunkVec vec, uint32_t zetaIndex = 0);
    bool hasTransientChunksLocked() const;
    void collectParkableChunksLocked(std::vector<ParkCandidate> &out);
    void finishParkChunks(std::vector<ParkCandidate> &candidates);
    bool tryUnparkChunk(std::unique_lock<gm20b::Mutex> &l, ChunkVec vec, uint32_t zetaIndex, uint64_t bytes,
                        bool dedicated, uint64_t align, uint32_t setAccess, uint32_t &index, bool &resident,
                        uint64_t maxBytes = 0);
    void releaseParkedChunksOnClose();

    // A role's memory steps: the shared pool chunk (a span over half of one gets its own), a chunk of block-linear images, a
    // chunk of write-combined buffers, and the rings' first sizes (each doubles where an allocation waited, to the render
    // server's). A client's chunks of a kind start smaller and grow with what it holds (chunkSize).
    struct Sizes {
        uint64_t chunk, blChunk, wcChunk, push, upload;
    };

    uint64_t chunkSize(const std::vector<Chunk> &chunks, uint64_t cap, uint64_t need, uint64_t align) const;

    static const Sizes kSizes[2];
    Role role_ = Role::RenderServer;
    OpenFailure openFailure_ = OpenFailure::Structural;
    std::atomic<bool> criticalPriority_{false};
    Sizes sizes_ = kSizes[0];
    mutable gm20b::Mutex lock_;
    Kernel k_;
    // The BOs only this device's jobs touch (resource_set.h); setView_ is the submitting thread's
    ResourceSet set_;
    SetView setView_;
    TextureTransfers transfers_;

    // One job of `channel` naming the set; `uses`, the job's uses no member holds; `recorded`, the members its command
    // buffer used (resource_set.h submitInSet)
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
    // visibility results' report slots (visibility.h), made at first use
    uint64_t visibilitySlot();
    Buffer visibilityPage_;
    std::once_flag visibilityOnce_;
    std::atomic<uint32_t> visibilityNext_{0};
#if SWITCH_METAL_PROF
    Bo spanMarks_;
#endif
    // m2v's handle tables and roots, made at the first ID named or state drawn through an argument buffer, and each record's
    // set 1 (heap_tables.h), under lock_
    Bo handleTables_, argumentRoots_;
    std::vector<Bo> heapSets_;
    HeapTables heapTables_;
    // the tables' and sets' CPU writes (counted under lock_, read by commit without it), and their count as of the last
    // submission that invalidated the L2 for them (the submitting thread's): an exact job names them host-written while the
    // two differ
    std::atomic<uint64_t> tablesWritten_{0};
    uint64_t tablesInvalidated_ = 0;
    std::atomic<uint64_t> ticWritten_{1}, tscWritten_{1};
    uint64_t ticInvalidated_ = 0, tscInvalidated_ = 0;

    void noteTicWrite()
    {
        ticWritten_.fetch_add(1, std::memory_order_release);
    }

    void noteTscWrite()
    {
        tscWritten_.fetch_add(1, std::memory_order_release);
    }
    HeapHolds heapHolds_ = nullptr;
    bool heapMemory();
    const HeapSet *heapSet(const HeapLayout &layout);
    void bindKernelArguments(ComputeState &c, const PackKernel &k);
    void bindArguments(Pipeline &pl, const PackPipeline &pp);

public:
    // A draw or dispatch that met heapMissing binds its state's argument reads afresh: true where they are bound now
    bool rebindArguments(Pipeline &pl);
    bool rebindKernelArguments(const ComputeState &c);

private:
    void tablesWritten();

    struct HeapPart {
        uint64_t offset = 0, bytes = 0;  // in the heap
        uint8_t *cpu = nullptr;
        Bo bo;
    };

    std::vector<HeapPart> heapParts_;
    uint64_t heapCopiesAt_ = 0;
    std::atomic<uint64_t> heapBound_{0};
    ProgramPages packPages_;
    uint64_t packWindowVa_ = 0, packWindowBytes_ = 0;
    const uint8_t *packWindowCpu_ = nullptr;

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
    // Written by openChannel (under channelLock_) and reopen (submit thread); read unlocked by waitFence, promise,
    // eventWord, channelCurrent and captureCpuCompletion on other threads.
    std::atomic<uint32_t> channel_{0};
    uint64_t progressVa_ = 0;
    uint32_t progressSeq_ = 0;
    // Stored only by whoever submits (the submit worker, or openChannel before channelReady_), one release store a job;
    // read by any thread, the memory release's purge among them, which holds lock_
    std::atomic<uint32_t> lastSeqno_{0};

    void submittedThrough(uint32_t seqno)
    {
        lastSeqno_.store(seqno, std::memory_order_release);
    }

    uint32_t lastSubmitted() const
    {
        return lastSeqno_.load(std::memory_order_acquire);
    }

    Bo bufferBoLocked(const Buffer &buffer) const;
    bool arenaCpuRangesLocked(const ArenaStorage &storage, const void *cpu, uint64_t bytes,
                              std::vector<HostAccess::Range> &ranges) const;
    bool hostArenaAccess(const ArenaStorage &storage, const void *cpu, uint64_t bytes, bool write, HostAccess &access);
    static void releaseHostArena(void *owner, uint32_t arena);
    // the channel's fence word; before there is a channel a zero, which every seqno submitted so far (none) has passed.
    // Written with release by openChannel and reopen once the fence page is mapped and the context submitted; read with
    // acquire by done, completed, semanticFence, liveKill and captureCpuCompletion from the completion, stall-tick,
    // memory-release and compressed-memory threads. The GPU's semaphore release writes the word itself, loaded with
    // acquire (SwitchGpuTimeline.cpp) so what the job wrote before its release is seen after the seqno.
    std::atomic<const volatile uint32_t *> fence_{&kNoFence};
    static constexpr uint32_t kNoFence = 0;

    static SemanticFenceRead semanticFenceAt(const volatile uint32_t *fence, uint32_t seqno)
    {
        if (fence == &kNoFence)
            return {};
        const auto *record = reinterpret_cast<const volatile TegraGM20BAbiFence *>(fence);
        return readSemanticFence(reinterpret_cast<const volatile TegraGM20BAbiFenceSemantic *>(record + 1), seqno);
    }

    SemanticFenceRead semanticFence(uint32_t seqno) const
    {
        return semanticFenceAt(fence_.load(std::memory_order_acquire), seqno);
    }

    gm20b::UnfairMutex channelLock_;
    std::atomic<bool> channelReady_{false};
    ChannelGenerations generations_;
    // channelLock_'s until the first channel is made (ensureChannel), then the submit thread's (reopen), as contextless_
    ChannelRefusal channelRefusal_;
    std::string channelError_;
    // The channel died and the one made since has no context yet (reopen gave up before the context's Exec): begin() makes it
    // again before it gives out a command buffer. channel_ is 0 while the kernel holds none of this device's.
    std::atomic<bool> contextless_{false};
    std::vector<uint32_t> freeTic_, freeTsc_, freeTicPairs_;

    struct SharedHeader {
        uint32_t slot, users;
    };

    std::map<std::array<uint32_t, 8>, SharedHeader> sharedTsc_;         // lock_
    std::unordered_map<uint32_t, std::array<uint32_t, 8>> tscHeaders_;  // lock_: a shared slot's header
    ViewCache views_{kTextureHeaders};                                  // viewTic's answers, the encoding thread's
    // by header: its views' word 0 (above it, the sample grid of a storage view: view.h viewHeader) and header (viewTic)
    std::unordered_multimap<uint32_t, std::pair<uint64_t, uint32_t>> viewTics_;
    // by header: its attachment views and their headers (attachmentViewTic)
    std::unordered_multimap<uint32_t, std::pair<AttachmentView, uint32_t>> attachmentViewTics_;
    // by header: its attachment parts' key and header (attachmentTic)
    std::unordered_multimap<uint32_t, std::pair<uint64_t, uint32_t>> partTics_;
    uint32_t nextTic_ = 1, nextTsc_ = 1, neutralTsc_ = 0, neutralCompareTsc_ = 0;

    // Tier-0 pack pipelines published lock-free via atomic CAS on first hit; owned here for the device's lifetime.
    struct Tier0PipelineSlots {
        std::unique_ptr<std::atomic<Pipeline *>[]> slots;
        uint32_t count = 0;

        void reset(uint32_t n)
        {
            clear();
            count = n;
            if (n)
                slots = std::make_unique<std::atomic<Pipeline *>[]>(n);
        }

        void clear()
        {
            if (slots)
                for (uint32_t i = 0; i < count; ++i)
                    delete slots[i].exchange(nullptr, std::memory_order_acq_rel);
            slots.reset();
            count = 0;
        }

        ~Tier0PipelineSlots()
        {
            clear();
        }
    };

    Tier0PipelineSlots tier0Pipelines_;
    // the pack's, by record index, each made at its first lookup (a process uses a few of the pack's thousands); owned
    // individually so placeRecord builds off a private Pipeline while lock_ may drop, and pointers stay valid
    std::unordered_map<uint32_t, std::unique_ptr<Pipeline>> pipelines_;
    // each own state's (ownPipeline) once a lookup placed it: any recording thread may place it first, the others read
    // it without lock_ after
    std::array<std::atomic<Pipeline *>, std::size(kOwnStates)> own_{};
    std::unordered_map<uint32_t, uint32_t> placedCold_;
    uint64_t heapPlaced_ = 0;
    // Programs placed so far, in place or copied, and that count as of the last submission whose prologue invalidated the
    // SMs' instruction caches (~0: none on this channel). The instruction cache prefetches past a program's end, where the
    // next program is placed.
    std::atomic<uint64_t> heapEpoch_{0};
    uint64_t heapInvalidated_ = ~0ull;
    // Shader local memory (slm.h), under lock_: the area and the most a placed program or kernel asks of a warp. An area
    // it outgrew is kept: a submission already made may still use it.
    Bo slm_;
    std::atomic<uint64_t> slmPerWarp_{0}, slmVa_{0};
    std::vector<Bo> slmOutgrown_;
    // Off-lock placement state under lock_: deduplicates concurrent grain imports and serializes heap/SLM growth.
    std::set<std::pair<const uint8_t *, uint64_t>> importingGrains_;
    bool heapGrowing_ = false, slmGrowing_ = false;
    uint32_t heapWaiters_ = 0;
    gm20b::CondVar heapReady_;
    // the per-warp size the channel was last given (commit; 0 on a new channel): the submitting thread's
    uint64_t slmChannel_ = 0;
    uint32_t ctWrite_[8] = {};  // SET_CT_WRITE(i) as the baseline leaves it
    uint32_t blendPerTarget_[6] = {};
    uint8_t blendPerTargetSeen_ = 0;

    bool slmReady(uint64_t perWarp) const
    {
        return perWarp <= slmPerWarp_.load(std::memory_order_acquire);
    }

    bool slmNeed(uint64_t perWarp);
    uint64_t programSlm(const PlacedProgram &p) const;
    std::atomic<bool> packWindowReady_{true}, packInitDone_{true};
    bool packWindowOk_ = true;
    mutable gm20b::CondVar packWindowCv_;

    void waitPackWindow() const;
    void runAsyncPackWindow();

    void ensurePackWindow() const
    {
        if (__builtin_expect(packWindowReady_.load(std::memory_order_acquire), 1))
            return;
        waitPackWindow();
    }

    bool finishPackWindow(std::string &err);
    void warmRenderServer();
    bool initPackWindow(std::string &err);
    bool place(uint32_t prog, uint32_t &program, uint64_t &data);
    bool heapGrow(uint64_t end);
    const HeapPart *heapPartAt(uint64_t at) const;
    bool heapPlace(const uint8_t *bytes, uint32_t size, uint64_t &offset);
    bool placeInPlace(ProgramPages &pages, const uint8_t *file, uint64_t fileOffset, uint32_t size, bool written,
                      uint64_t &offset);
    bool placeFromStore(const uint8_t *slot, uint32_t size, uint64_t &offset);
    const uint8_t *heapCpu(uint64_t at) const;
    // `spills`: its argument tables' words (PackPipeline::spillWords), null where it has none. `far`: its far block
    // (set_entries.h), null where it has kPackEntries entries or fewer. `tess`: a tessellation state's control and
    // evaluation programs (null: not one). `prebuilt`: PackSegments entry in tier 0 of PackHeap (null: build dynamically).
    void build(Pipeline &pl, const PackPipeline &pp, const PlacedProgram &vs, const PlacedProgram &fs,
               const uint32_t *blob, const char *keyString, bool caTransform, const uint32_t *spills,
               const uint32_t *far, const PlacedProgram *tess = nullptr, const PackSegment *prebuilt = nullptr);
    const PackSegment *prebuiltPipelineSegment(uint32_t recIndex) const;
    const PackSegment *prebuiltVariantSegment(uint32_t recIndex, uint32_t comboIndex) const;
    // Lock-free tier-0 pack placement when all programs, SLM and prebuilt segments reside in the pack window; falls back to
    // lock_ + placeRecord otherwise.
    Pipeline *placePackedRecord(const PackPipeline &pp, bool caTransform);
    std::unique_ptr<Pipeline> tryBuildTier0LockFree(const PackPipeline &pp, uint32_t recIndex, bool caTransform);
    // Tier-0 record `idx`'s pipeline: the slot's, or `built` published in it
    Pipeline *publishTier0(uint32_t idx, std::unique_ptr<Pipeline> built);
    // lock_ held: pack record `pp` placed (its programs in the heap) and built, once; null where the heap has no room
    Pipeline *placeRecord(const PackPipeline &pp, bool caTransform);
    // `pl`'s split offered, its twin made when a draw first takes it (makeSplit)
    void offerSplit(Pipeline &pl, SplitSource &&source);
    void offerVariants(Pipeline &pl, const PackPipeline &pp, uint32_t recIndex);
    bool offerVariantsLockFree(Pipeline &pl, const PackPipeline &pp, uint32_t recIndex);
    bool placeRequiredVariants(const PackPipeline &pp, uint64_t &slm);
    Pipeline *placeVariant(Pipeline &pl, VariantTable &t, const std::array<uint32_t, 2> &programs, uint32_t setReads,
                           uint32_t recIndex, uint32_t comboIndex);
    // A pipeline of `t` (VariantTable::pipelines) of `pl`'s programs `placed`: `setReads` (its variants'
    // PackVariant::memoryEntries) added; `pl`'s restore and stride twin
    Pipeline &buildCombination(VariantTable &t, const Pipeline &pl, const PackPipeline &pp,
                               const PlacedProgram (&placed)[2], uint32_t setReads, const uint32_t *blob,
                               const char *keyString, const uint32_t *spills, const uint32_t *far,
                               const PackSegment *prebuilt);
    bool placePackProgram(uint32_t index, PlacedProgram &out);
    bool placePackWindowProgram(uint32_t index, PlacedProgram &out);
    std::deque<Pipeline> reachPipelines_;
    std::deque<PackPipeline> reachRecords_;
    std::deque<std::vector<uint32_t>> reachFar_;  // their far blocks, each its record's
    // lock_ held: the push words a reach twin adds to its state's methods and to its restore (made with the first twin)
    std::vector<uint32_t> reachMethods_, reachRestore_;
    SplitState *makeSplit(Pipeline &pl);
    bool placeSplitPrograms(const SplitSource &source, PlacedProgram &fs, uint32_t &prologue, uint64_t &prologueData,
                            uint32_t &prologueDataBytes);
    std::deque<SplitState> splitStates_;
    // The constexpr samplers' header indices, the pack's then the runtime records' (addRecord, under lock_). A slot is written
    // once, before the state that names it is published, and never moves: a draw reads it without lock_.
    static constexpr uint32_t kConstexprSamplers = 256;  // PackEntry::sampler is a byte
    uint32_t constexprTsc_[kConstexprSamplers] = {};
    // and each one's headers under a planar texture's address maps 1 to 3 (mapSamplerAddresses): the pack's placed with
    // them, a runtime record's by constexprMapsPlaced before the work that samples a planar texture; and at kLevelZeroMap
    // each one's by constexprLevelZeroPlaced
    uint32_t constexprMapped_[kConstexprSamplers][kLevelZeroMap + 1] = {};
    uint32_t constexprCount_ = 0, constexprMappedCount_ = 0, constexprLevelZeroCount_ = 0;
    uint32_t constexprWord0_[kConstexprSamplers] = {};  // and their headers' word 0, written with them
    // constexprMapped_[index] from its header `words` (lock_ held, or the device not yet open); false: no room
    bool placeConstexprMaps(uint32_t index, const uint32_t words[8]);

    // the states the runtime made (addRecord), never removed: Pipeline::rec points into them. What attachFullForm reads
    // of them is written once, before the state is published.
    struct Completed {
        PackPipeline record;
        std::string keyString;
        std::vector<uint32_t> blob, spill, far;
        PackProgram programs[2];
        Pipeline pipeline, turned;
        TurnedSource turnedSource;
        PlacedProgram vs, fs;
        RecordLayout layout;  // its record's as given, before its constexpr samplers were renumbered as the device's
    };

    std::deque<Completed> completedStore_;
    std::unordered_map<uint64_t, Completed *> completed_;
    std::map<std::array<uint32_t, 8>, uint8_t>
        completedSamplers_;  // their constexpr samplers' headers, by constexprTsc_ index

    struct CompletedProgram {
        PackProgram info;
        PlacedProgram placed;
        uint8_t header[80];
        std::vector<uint16_t> farUbos;
    };

    // Never overwrites a live entry whose header/info pointers may already be held by a published Pipeline when two
    // callers race through placeCompletedSlot across an off-lock heap/SLM allocation.
    struct CompletedProgramMap {
        std::map<std::string, CompletedProgram> map;
        std::deque<CompletedProgram> duplicates;

        auto find(const std::string &key)
        {
            return map.find(key);
        }

        auto end()
        {
            return map.end();
        }

        CompletedProgram &operator[](const std::string &key)
        {
            const auto [it, inserted] = map.emplace(key, CompletedProgram{});
            return inserted ? it->second : duplicates.emplace_back();
        }
    };

    CompletedProgramMap completedPrograms_;  // by programSlotKey
    bool placeCompleted(const CapturedProgram &p, PlacedProgram &out);
    static CompletedSlot completedSlot(const CapturedProgram &p);
    bool placeCompletedSlot(const CompletedSlot &slot, PlacedProgram &out);
    // The constexprTsc_ index of sampler header `words`: the pack's own, one a runtime record placed, or placed now (under
    // lock_); false: no room
    bool constexprSampler(const std::array<uint32_t, 8> &words, uint32_t &index);

    void offerCompletedSplit(Completed &c, const RuntimeRecord &r, const PlacedProgram &vs, bool caTransform);
    void offerCompletedVariants(Completed &c, const RuntimeRecord &r);
    bool takeTsc(uint32_t &tsc);
    // A sampler header slot holding `words` (lock_): one slot for every sampler's compare form and address-map variant of
    // the same 32 bytes, given back when its last user drops it; false: the table is full
    bool takeHeader(const uint32_t words[8], uint32_t &slot);
    void dropHeader(uint32_t slot);
    const Pipeline *extrasLive_ = nullptr;  // the state whose extras (Pipeline::restore) the channel holds
    HeldSegment heldSegment_;               // the state segment its registers hold after everything submitted
    StateSegmentHeap segmentHeap_;          // lock_ (place)
    CommandBuffer cb_;
    uint32_t rootPush_[32] = {};     // what the GPU's root table holds at [0x700, 0x780) after everything submitted
    uint32_t restart_ = 0;           // and its primitive restart index
    uint32_t rtLayer_ = ~0u;         // and SET_RT_LAYER (~0: not known)
    uint32_t alphaHeld_ = ~0u;       // and SET_ANTI_ALIAS_ALPHA_CONTROL (~0: not known)
    uint32_t sampleMaskHeld_ = ~0u;  // and SET_SAMPLE_MASK_X0_Y0..X1_Y1 (~0: not known)
    bool clampRangeHeld_ = false;
    uint32_t clampRange_[2] = {};
    DepthStencilRegs ds_;            // and its depth/stencil registers
    uint32_t tiledCacheSize_ = kTiledCacheDefaultSize;
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, Import *> surfaces_;
    // The surfaces being imported with lock_ released (importSurface), each owned by its importing thread until it is
    // published or failed: a thread that waits for one sleeps on it with lock_ let go and lends the importer its priority
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::shared_ptr<gm20b::OwnedWord>> importing_;
    void waitImport(std::unique_lock<gm20b::Mutex> &l, std::shared_ptr<gm20b::OwnedWord> import);
    // Surface imports no texture holds, kept for the surface's next texture: Core Animation makes a texture over its
    // pooled surface every frame after it drops the last, and an import made again is bound, untiled, cleaned and laid
    // out again (boot 21: 3.4-4 ms of each app-close frame). Oldest first, at most kKeptImports and kKeptImportBytes;
    // trim (idle, memory pressure) gives them all back. Each holds its surface (Import::owner) until then.
    static constexpr size_t kKeptImports = 32;
    static constexpr uint64_t kKeptImportBytes = 64ull << 20;
    std::vector<Import *> keptImports_;
    uint64_t keptImportBytes_ = 0;
    std::vector<std::unique_ptr<NativeSurface>> nativeSurfaces_;
    uint64_t nativeGeneration_ = 0;
    const Texture *nativeRead(const Texture &source, NativeSurface *&owner, int &error);
    const Texture *nativeWrite(const Texture &source, NativeSurface *&owner, int &error);
    // What teardown, trim, purge or creation rollback detaches under lock_ and frees synchronously once lock_ is let go
    // (alias.cpp): BOs, VA reservations, arena host ranges and CF owner references, freed in FIFO order.
    enum class RetireKind : uint8_t {
        None = 0,
        Chunk,
        WcShared,
        WcDedicated,
        SharedHeap,
        GpuHeap,
        KeptImport,
    };

    struct Retired {
        Bo bo;
        uint64_t va = 0, vaBytes = 0;
        void *host = nullptr;
        uint64_t hostBytes = 0;
        const void *owner = nullptr;
        uint64_t accountBytes = 0;
        uint64_t accountMapped = 0;
        RetireKind kind = RetireKind::None;
    };

    class DeferredFrees {
    public:
        explicit DeferredFrees(Device &dev) : dev_(dev)
        {}

        DeferredFrees(const DeferredFrees &) = delete;
        DeferredFrees &operator=(const DeferredFrees &) = delete;

        ~DeferredFrees()
        {
            flush();
        }

        bool empty() const
        {
            return items_.empty() && !compFreed_;
        }

        void add(const Retired &r);

        void noteCompFreed()
        {
            compFreed_ = true;
        }

        void flush();

    private:
        Device &dev_;
        std::vector<Retired> items_;
        bool compFreed_ = false;
    };

    // Bytes and chunks detached from Device tables under lock_ whose off-lock BoFree/vm_deallocate/CFRelease has not
    // completed yet: incremented under lock_ before clearing the slot and decremented off lock_ after freeing so
    // Device::held(), wcChunkBytesHeld() and controlMappedBytes() neither under-report still-mapped memory nor
    // double-count it.
    struct Retiring {
        std::atomic<uint64_t> chunkBytes{0};
        std::atomic<uint32_t> chunkCount{0};
        std::atomic<uint64_t> wcBytes{0};
        std::atomic<uint64_t> wcSharedBytes{0};
        std::atomic<uint64_t> wcMapped{0};
        std::atomic<uint64_t> heapBytes{0};
        std::atomic<uint64_t> gpuHeapBytes{0};
        std::atomic<uint64_t> keptBytes{0};
    };

    Retiring retiring_;
    void freeNow(Retired &r);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // createTexture's block-linear target over compressible memory of its own (probeCompressNext), under lock_
    bool probeCompressible(const tgm_layout &bl, Texture &t, DeferredFrees &deferred);
#endif
    void reapNativeSurfaces(std::vector<Bo> &detached, bool all = false);
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
    // separate VOL=1 WC chunks for streaming shared buffers so shared textures in wcChunks_ stay L2-cached
    std::vector<Chunk> volatileWcChunks_;

    // lock_ (scrub.h)
    std::vector<Scrub> scrubWanted_, scrubbing_;
    // begin()'s, a successful commit()'s (with its closing job, whose end flushes the zeros) and abandon()'s, on the
    // submitting thread: the spans a command buffer fills at its front
    void scrubInto(CommandBuffer &cb);
    void scrubsSubmitted(CommandBuffer &cb, uint32_t seqno);
    void scrubsReturned(CommandBuffer &cb);
    std::atomic<bool> scrubDue_{false};
    // lock_ held. Null `deferred`: an emptied chunk is kept (the allocation that asked will use it)
    void retireScrubs(DeferredFrees *deferred);
    void scrubbedFree(const Scrub &s, DeferredFrees *deferred);
    bool wcPoolAlloc(std::unique_lock<gm20b::Mutex> &l, std::vector<Chunk> &pool, uint64_t bytes, bool gpuVolatile,
                     Span &s, bool *dirty = nullptr);
    void wcPoolFree(std::vector<Chunk> &pool, Span &s, DeferredFrees &deferred);
    // An empty live shared chunk of `pool` besides `ch`: the one an emptied chunk leaves for the next burst
    static bool otherEmptyChunk(const std::vector<Chunk> &pool, const Chunk &ch);
    bool wcAlloc(std::unique_lock<gm20b::Mutex> &l, uint64_t bytes, Span &s, bool *dirty = nullptr);
    void wcFree(Span &s, DeferredFrees &deferred);
    bool volatileWcAlloc(std::unique_lock<gm20b::Mutex> &l, uint64_t bytes, Span &s, bool *dirty = nullptr);
    void volatileWcFree(Span &s, DeferredFrees &deferred);

    bool blAlloc(std::unique_lock<gm20b::Mutex> &l, uint64_t bytes, uint64_t align, Span &s)
    {
        return tiledAlloc(l, blChunks_, fx::kBlockLinearKind, bytes, align, s);
    }

    void blFree(Span &s, DeferredFrees &deferred)
    {
        tiledFree(blChunks_, s, deferred);
    }

    // lock_ held: where block-linear span `s`'s bytes lie under a kind-0 view of its chunk; 0: no view could be bound
    uint64_t blPitchVa(const Span &s);

    struct Arena {
        uint64_t id = 0;
        uint64_t va = 0, bufferVa = 0, bytes = 0;
        uint32_t kind = 0;
        uint8_t *cpu = nullptr;  // its host range, reserved for good; its pages wired and bound unless purged
        Bo bo;
        HeapPlacement place;
        std::shared_ptr<std::atomic<uint64_t>> multiKindGen;
        uint32_t leases = 0;       // active CPU accesses (hostArenaAccess): blocks purge, trim and teardown
        uint32_t aliasLeases = 0;  // live aliasable placements: blocks trim and teardown, not volatile purge
        bool used = false, isVolatile = false, purged = false, dying = false;
        bool wc = false;    // its host pages are write-combined (arena.cpp hostPages)
        bool busy = false;  // an off-lock purge, trim or restore is in flight on this slot (lock_)
        bool kernelBo = false;
        bool boParked = false;

        // its BO bound again under another PTE kind (createArena's comment)
        struct View {
            uint64_t va = 0;
            uint32_t kind = 0;
        };

        std::vector<View> views;
        // trimmed (arena_trim.h): `bo` is empty, its pages are bound as `parts` (each with its offset), and `holes`
        // are unbound, their host pages untouched
        std::vector<std::pair<uint64_t, Bo>> parts;
        std::vector<ArenaRange> holes;
    };

    std::vector<Arena> arenas_;
    uint64_t nextArenaId_ = 0;
    // Encode-thread-only tracking of multi-kind arenas (empty unless an arena has overlapping different-kind placements).
    uint64_t l2InvalidatedBeforeCb_ = 0;
    std::map<uint64_t, KindRanges> arenaKindUses_;
    std::map<uint64_t, uint64_t> arenaCrossSeen_;

    uint64_t arenaCrossSeen(uint64_t arenaId) const
    {
        const auto it = arenaCrossSeen_.find(arenaId);
        return it == arenaCrossSeen_.end() ? 0 : it->second;
    }

    gm20b::CondVar arenaReady_;
    std::atomic<uint32_t> purgedArenas_{0};
    void waitArena(std::unique_lock<gm20b::Mutex> &l, uint32_t index);
    bool restoreArenaLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index);
    bool restoreKernelArenaOffLock(bool wasParked, Bo &oldBo, uint8_t *cpu, uint64_t va, uint64_t bufferVa,
                                   uint64_t bytes, uint32_t kind, const std::vector<Arena::View> &views, Bo &outBo,
                                   bool &unparkResident, bool &unparkReclaimed, bool &unparkMismatch);
    bool bindArenaOffLock(Arena &a);
    void unbindArena(Arena &a, DeferredFrees &deferred);  // lock_ held: its import, whole or in parts, detached
    void trimLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index, const std::atomic<uint32_t> &committed);
    // lock_ held through `l` (released across kernel imports/binds): the holes a placement of [offset, offset + bytes)
    // reaches bound again; false: no memory for them
    bool backPlacement(std::unique_lock<gm20b::Mutex> &l, uint32_t index, uint64_t offset, uint64_t bytes);
    bool purgeKernelArenaLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index);
    bool purgeLocked(std::unique_lock<gm20b::Mutex> &l, uint32_t index, const std::atomic<uint32_t> &committed,
                     bool *parkBusy = nullptr);
    void freeArena(Arena &a, DeferredFrees &deferred);                                    // lock_ held
    void arenaFree(std::unique_lock<gm20b::Mutex> &l, Span &s, DeferredFrees &deferred);  // lock_ held
    const ArenaStorage *arenaStorage(const Arena &arena, uint32_t index, uint64_t offset, uint64_t bytes, uint64_t va,
                                     uint32_t kind) const;
    // lock_ held through `l` (released across kernel VA/alias calls): the arena's mapping of `kind`; 0: none
    uint64_t arenaView(std::unique_lock<gm20b::Mutex> &l, uint32_t index, uint32_t kind);
    // The layout every create* but createTexture's own memory gives its kind of texture: t's shape, rows and kind fields
    // with no memory or header, what its memory takes and the PTE kind it is bound with, `chain` a Mips, Volume or Array
    // one's levels. False: the device makes no such texture.
    static bool layout(const TextureAsk &ask, Texture &t, uint64_t &bytes, uint64_t &align, uint32_t &kind,
                       fx::MipChain *chain = nullptr);
    static TextureAsk standInAsk(const Texture &t);
    // `ask` laid out in `at`'s arena, else (any kind but Plain) in the device's chunks of its kind, with its header
    bool createLaidOut(const TextureAsk &ask, Texture &t, const ArenaAt *at);
    // lock_ held: `bytes` of the arena under `kind` as t's memory (Texture::arena)
    bool arenaSpan(std::unique_lock<gm20b::Mutex> &l, const ArenaAt &at, uint64_t bytes, uint64_t align, uint32_t kind,
                   Texture &t);

    // a span of chunks bound with PTE kind `kind` (block-linear images), lock_ held through `l`
    bool tiledAlloc(std::unique_lock<gm20b::Mutex> &l, std::vector<Chunk> &chunks, uint32_t kind, uint64_t bytes,
                    uint64_t align, Span &s);
    void tiledFree(std::vector<Chunk> &chunks, Span &s, DeferredFrees &deferred);
    // lock_ held through `l` (released across the kernel's call); the chunk's index in `chunks`
    bool newTiledChunk(std::vector<Chunk> &chunks, uint32_t kind, uint64_t size, bool dedicated,
                       std::unique_lock<gm20b::Mutex> &l, uint32_t &index);
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
    // each its own view (texture header, layout) of the same bytes, as compressed memory is (comp.h CompMem): taken in
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
        CompMem *comp = nullptr;          // the compressed memory it is a view of (comp.h), else none
        PlainMem *plain = nullptr;        // or the plain memory (a memoryless one's)
        // a compressed backing of its shape, or one on larger memory of its tag, replaced it: no pass takes it again,
        // trimBackings frees it
        bool retired = false;
    };

    // backing()'s tags, each kind of backing its own, and the backing it last answered each with: the device's below,
    // then the command buffer's scratch ranges (turnScratch), from kTurnTag's (turned_pass.cpp) to
    // kFormatStandInTag's eight colours and their eight resolve targets' (kFormatResolveSlot), the last
    enum BackingTag : uint32_t {
        kMemorylessTag = kTransientColorTag,
        kShaderDepthTag = kMemorylessTag + 8,
        kShaderStencilTag,
        kShaderStencilTempTag,
        kSeparateZetaTag,
        kSeparateTempTag,
        kBackingTags = kFormatStandInTag + 2 * 8
    };

    std::array<Backing *, kBackingTags> lastBackings_{};

    static constexpr uint32_t kPlainPool = 0, kPlainBl = 1, kPlainZeta = 2;
    std::vector<std::unique_ptr<PlainMem>> plainMems_;
    PlainMem *largestPlain(uint32_t tag, uint32_t memory, bool layered) const;
    // A memoryless backing of `m`'s shape on the largest plain memory of its tag and memory, made where none holds it;
    // nullptr when no memory or header is had
    Backing *plainBacking(std::unique_lock<gm20b::Mutex> &l, const Texture &m, uint32_t tag, bool blockLinear,
                          DeferredFrees &deferred);
    void plainFree(Backing &b, DeferredFrees &deferred);
    void freePlainSpan(PlainMem &mem, DeferredFrees &deferred);
    void dropPlain(PlainMem *mem, DeferredFrees &deferred);  // its span back, and it out of plainMems_

    // createOwnedZeta's memory by its address: its format's index and span, outside Texture::span, which alias() and
    // freeSpan() take for block-linear colour memory
    std::map<uint64_t, std::pair<uint32_t, Span>> ownedZeta_;
    // createCompressedZeta's memory, or a moved zeta's (compressedFromPlain), by its address
    std::map<uint64_t, PrivateComp> compZeta_;
    void freeCompressedZeta(uint64_t va, DeferredFrees &deferred);  // lock_ held

    void freeOwnedZeta(uint64_t va, DeferredFrees &deferred)  // lock_ held
    {
        freeCompressedZeta(va, deferred);
        const auto at = ownedZeta_.find(va);
        if (at == ownedZeta_.end())
            return;
        tiledFree(zChunks_[at->second.first], at->second.second, deferred);
        ownedZeta_.erase(at);
    }

    std::vector<Backing *> backings_;  // memoryless attachments' memory, by size and format

    // What backing() answered, kept by the encoding thread for lastBacking
    struct BackingAsk {
        uint32_t width = 0, height = 0, format = 0, tag = 0, layers = 1;
        uint8_t samples = 0;
        bool zeta = false, blockLinear = false, compression = false, copied = false;
        // kindProbes() as the ask found them, which compKind reads and no epoch move covers
        uint8_t probes = 0;
        Backing *backing = nullptr;
        uint64_t epoch = 0;

        bool same(const Texture &m, uint32_t t, bool linear, bool compressed, bool copy, uint8_t probed) const
        {
            return backing && width == m.width && height == m.height && format == m.format && tag == t &&
                   layers == std::max(m.layers, 1u) && samples == m.samples && zeta == m.zeta &&
                   blockLinear == linear && compression == compressed && copied == copy && probes == probed;
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
    bool growing_ = false, stopping_ = false, closing_ = false;
    uint32_t offLockChunkOps_ = 0;
    gm20b::CondVar grow_, grown_, chunkReady_;
    std::thread grower_;
    uint32_t clientStalls_ = 0;
    bool newChunk(uint64_t size, bool dedicated, std::unique_lock<gm20b::Mutex> &l, uint32_t &index);
    void growLoop();
    void startGrower();
    void clientStalled();
    uint64_t growChunk(const std::vector<Chunk> &chunks, uint64_t cap, uint64_t align) const;
    bool growAheadRoom(uint64_t chunk);
    static bool readFootprint(uint64_t &footprint, uint64_t &available);
    // Written by the submitting thread alone (the submit worker, which runs every move too), each a load and a store; a
    // command buffer's backings are marked used before closedCount_ moves
    std::atomic<uint64_t> commitCount_{0};
    uint64_t begun_ = 0;                    // CommandBuffer::serial's source: Device::begin's, on the submit worker
    std::atomic<uint64_t> closedCount_{0};  // command buffers committed or abandoned

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

    bool init(std::string &err, bool asyncPackWindow);
    bool openChannel(std::string &err, bool &gaveBack);
    void growRing(Ring &r, uint64_t most);
    void freeChunk(Chunk &ch, DeferredFrees &deferred);
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
    mutable gm20b::UnfairMutex killedLock_;
    std::atomic<bool> keptKills_{false};
    std::atomic<uint32_t> keptThrough_{0};
    std::array<Killed, 4> killedKept_{};
    uint32_t killedNext_ = 0;
    mutable std::atomic<uint32_t> killedError_{0};
    uint8_t *alloc(Ring &r, uint32_t bytes, uint32_t align, uint64_t &va);
    void retire(Ring &r, uint32_t seqno);
    bool ownWords(const Ring &r, uint64_t at, uint64_t bytes) const;
    bool outgrow(Ring &r, uint32_t bytes, uint64_t &at);
    // The GPU address of a word the open command buffer wrote into the push ring: the ring's own, or that of a ring it
    // grew out of since (Device::outgrow), which keeps its words until the command buffer's work completes
    uint64_t pushVa(const uint32_t *word) const;
    uint32_t allocTic();
    void freeTic(uint32_t tic);  // lock_ held
    // Two headers at consecutive indices (an over-planes RGB10A8_2P_XR10 texture's), the first's index; 0: none left. lock_ held
    uint32_t allocTicPair();
    void freeTicPair(uint32_t first);
    void writeTic(const Texture &t);
    bool poolAlloc(std::unique_lock<gm20b::Mutex> &l, uint64_t bytes, Span &s, bool *dirty = nullptr);
    void poolFree(Span &s, DeferredFrees &deferred);
    bool tiledSurfaceTexture(uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, Texture &t,
                             Texture *sampled, DeferredFrees &deferred);
    // texel_rows.cpp. lock_ held: `t` a texel buffer of `elements` texels of `format` held in a pool of import `i`'s
    // bytes from `offset`, its rows `bytesPerRow` apart, taking `i`'s hold
    bool createTexelRows(std::unique_lock<gm20b::Mutex> &l, Import *i, uint64_t offset, uint32_t bytesPerRow,
                         uint32_t elements, uint32_t format, Texture &t, DeferredFrees &deferred);
    TextureTransferResult transferTexelRows(const Texture &t, const uint32_t origin[3], const uint32_t size[3],
                                            void *bytes, bool write, std::shared_ptr<void> owner);
    Import *importSurface(std::unique_lock<gm20b::Mutex> &l, uint64_t id, void *base, uint64_t bytes, const void *owner,
                          const LayoutAsk *ask = nullptr);
    Import *makeImport(uint64_t id, void *base, uint64_t bytes, const void *owner, const LayoutAsk *ask);
    // lock_ held: a texture's hold on its import goes. A surface's import no texture holds is kept (keepImport)
    void release(Import *i, DeferredFrees &deferred);
    void keepImport(Import *i, DeferredFrees &deferred);
    void dropImport(Import *i, DeferredFrees &deferred, bool wasKept = false);
    Trimmed dropKeptImports(DeferredFrees &deferred);
    // lock_ held (alias.cpp): a hold on a span's lease, made at its first alias; the last drop frees what the lease holds
    Lease *leaseSpan(Lease *&lease, const Span &span, Memory from);
    void dropLocked(std::unique_lock<gm20b::Mutex> &l, Lease *lease, DeferredFrees &deferred);
    // lock_ held: the span goes back to where it came from now, or at its lease's last drop
    void freeSpan(std::unique_lock<gm20b::Mutex> &l, Span &span, Memory from, Lease *&lease, DeferredFrees &deferred);

    static Memory memoryOf(const Texture &t)
    {
        return t.arena ? Memory::Arena : t.blockLinear ? Memory::Bl : t.wc ? Memory::Wc : Memory::Pool;
    }

    static Memory memoryOf(const Buffer &b)
    {
        return b.arena ? Memory::Arena : b.wc ? (b.gpuVolatile ? Memory::VolatileWc : Memory::Wc) : Memory::Pool;
    }

    // Scratch colors occupy distinct tags within a pass and reuse them after its GPU boundary. Depth and shader-test
    // scratch remain separate. Plain and compressed views keep their own backing and retire after their final fence.
    // `copied`: the copy engine fills or drains it (a shadow), so it compresses only where comp.h proves that safe.
    Texture *backing(std::unique_lock<gm20b::Mutex> &l, DeferredFrees &deferred, const Texture &memoryless,
                     uint32_t tag = 0, bool blockLinear = false, bool compression = true, bool copied = false);
    Texture *copyScratch(std::unique_lock<gm20b::Mutex> &l, DeferredFrees &deferred, uint64_t bytes, uint32_t tag);
    // The backing backing() last answered the same ask with, taken with no lock while nothing that decides backings has
    // changed since (backingEpoch_); nullptr: ask backing() under lock_. The encoding thread's alone.
    Texture *lastBacking(const Texture &memoryless, uint32_t tag = 0, bool blockLinear = false, bool compression = true,
                         bool copied = false);
    void trimBackings(DeferredFrees &deferred);

    // Compressed backings (comp.cpp), lock_ held: the compressible kind a backing of this shape takes (0: none or
    // compression off); a backing on compressed memory made off this thread, or nullptr after asking compThread_ for some
    CompQueue comp_;
    // made off the render thread on a thread of its own, started at the first ask: the kernel zeroes the pages a free left
    // dirty and clears the comptags at the bind, which takes milliseconds for a full-screen target, and no other thread (the
    // pool's grower included, which a render thread may wait for) waits behind it
    std::thread compThread_;
    gm20b::CondVar compWake_;
    uint32_t compKind(const Texture &m, bool blockLinear, bool copied) const;

    // probeZetaKinds (bit 0) and probePlainMemory (bit 1), the debug switches compKind reads
    uint8_t kindProbes() const
    {
        uint8_t probes = 0;
#if SWITCH_METAL_DIAGNOSTICS
        probes |= probeZetaKinds.load(std::memory_order_relaxed) ? 1 : 0;
#endif
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        probes |= probePlainMemory.load(std::memory_order_relaxed) ? 2 : 0;
#endif
        return probes;
    }

    Backing *compBacking(const Texture &m, uint32_t tag, uint32_t kind);
    // What was asked for, made, `l` dropped around each kernel allocation
    void compMake(std::unique_lock<gm20b::Mutex> &l);
    uint64_t compEvictIdleReady(const CompWant &w, bool comptagExhausted, std::vector<Bo> &evict);
    void compStop();
    // Asks compThread_ for `w` unless it was
    void compAsk(const CompWant &w);
    // A new multisampled memoryless attachment's compressed memory, asked for at its creation so its first pass finds it made
    void compPrearm(const Texture &m);
    // compThread_ started where it is not: it trims what an idle process holds when it turns stale (compDeadline)
    void compStart();
    bool compRearm_ = false;  // shared plain memory was made: compThread_ takes its deadline (lock_)
#if SWITCH_METAL_DIAGNOSTICS
    uint64_t reclaimAhead_ = 0;  // lock_: how far ahead of the clock the next trimBackings judges (reclaimIdleNow)
#endif
#if SWITCH_METAL_DIAGNOSTICS
    std::vector<std::function<void()>> compSettled_;  // lock_
    bool compUnsettled();                             // lock_
    void compSettledRun(std::unique_lock<gm20b::Mutex> &l);
#endif
    void compFree(Backing &b, DeferredFrees &deferred);
    // This process gave compressed memory back, lock_ held: the asks refused for a shortage may be made again, and each
    // a plain private texture still wants is (the one event that can change the kernel's answer that this process sees;
    // another process's give-back raises none)
    void compGaveBack(const CompWant *keepRefused = nullptr);
    // Made buffers no backing took while stale, at `now`
    void compTrim(uint64_t now, DeferredFrees &deferred);
    // When compressed or shared plain memory, a made buffer or a retired backing next turns stale (compClock()), 0 with
    // none held
    uint64_t compDeadline() const;

    // `view` of memory `of` names: one of a plain texture wanting compressed memory keeps that texture plain
    // (compressedFromPlain), lock_ held
    void noteView(const Texture &of, Texture &view);
    void forgetView(const Texture &view);  // lock_ held
    // private textures on compressed memory and their memory (private_comp.h), lock_
    std::unordered_map<const Texture *, PrivateComp> privateComp_;
    // Compressed memory of `kind` holding `bytes`, taken from what comp_ made; false (and asked for) where none is
    // ready or a debug probe holds private textures plain, lock_ held
    bool takeCompressed(uint64_t bytes, uint32_t kind, Bo &bo);
    // `t` laid out as `ask` on memory `bo` of kind ask.compressKind (createLaidOut), recorded as a private compressed
    // texture, lock_ held
    void noteCompressed(const TextureAsk &ask, const Bo &bo, uint64_t bytes, Texture &t);
    // written under lock_, read without it by the counters
    std::atomic<uint64_t> privateMade_{0};
    void releasePrivateCompressed(const Texture &t, DeferredFrees &deferred);  // lock_ held
    // A closed private texture's compressed memory, lock_ held: kept made for the next of its kind where that holds
    // back no ask refused for a shortage, else given back
    void compRelease(PrivateComp comp, DeferredFrees &deferred);

    // plain private textures wanting compressed memory (wantCompressed), lock_
    struct PrivateWant {
        uint32_t kind = 0;
        uint64_t bytes = 0;            // its block-linear image's
        void *owner = nullptr;         // wantCompressed's
        std::vector<Texture *> views;  // raw views of its plain memory (createView, createPartView)
    };

    std::unordered_map<const Texture *, PrivateWant> privateWants_;
    // Set under lock_ where a want and made memory that serves it meet (compReadyPush, wantCompressed) and cleared by the
    // worker's look (privateMoves), read by the submit worker with no lock before each raw range: an atomic, as the worker
    // holds no lock there. A want no ready memory serves leaves it clear.
    std::atomic<bool> privateMoveDue_{false};
    // lock_ held: `bo` of `kind` among the made buffers; the look is due where it serves a want
    void compReadyPush(const Bo &bo, uint32_t kind);
    // Header `tic` and those made from it, which freeTic frees with it (viewTic's, attachmentViewTic's, attachmentTic's
    // parts and theirs), into `tics`, lock_ held
    void headersOf(uint32_t tic, std::vector<uint32_t> &tics) const;
    // Header `tic`'s words as work from now on reads them, lock_ held: a move whose job has not run yet holds them
    const uint8_t *headerNow(uint32_t tic) const;

    // A moved texture's plain memory and the headers its job copied over `tics` (none once the texture went), freed at a
    // begin() once that job completed. Added and removed by begin()'s caller, who holds the submit gate; lock_
    struct MovedPlain {
        const Texture *texture = nullptr;
        Span span;
        Buffer headers;
        std::vector<uint32_t> tics;
        uint32_t seqno = 0;
        uint64_t generation = 0;
        uint32_t zeta = kZetaFormats;  // a zeta's format index: its span is zChunks_[zeta]'s
    };

    std::vector<MovedPlain> movedPlain_;
    void freeMovedPlain();
    // A clear of target t to SET_COLOR_CLEAR_VALUE words `v` (or depth `depth`), `full` it covers t whole: on a compressed
    // target, a value the ZBC table lacks is asked for (zbc.h); hits and misses counted
    void clearZbc(const Texture &t, bool full, const uint32_t v[4]);
    void clearZbcDepth(const Texture &t, bool full, float depth);
    ZbcTable zbc_;
    // zcull's layout where the kernel reports one (zcull.h), from open
    TegraGM20BAbiZcullInfo zcullInfo_ = {};
    bool zcullOn_ = false;
    // The zcull regions' storage, the regions parked for a command buffer's next range, and the last job's slot owner
    // (zcull_slots.h, zcull_planes.h): the encoding thread's alone
    ZcullSlots zcullSlots_;
    ZcullCarry zcullCarry_;
    uint64_t zcullOwners_ = 0;
    // A slot's storage, cached and in the resource set (ControlUse::ZcullStorage); false: none
    bool makeZcullSlot(Bo &bo, uint64_t bytes);
    // Whether a full clear to `e` writes comptags only; one this process never asked for is asked for
    bool zbcHeld(const TegraGM20BAbiZbc &e);
    // compThread_ asks the kernel for the ZBC entries wanted, `l` dropped around each call
    void zbcMake(std::unique_lock<gm20b::Mutex> &l);
};

// NV topology for an MTLPrimitiveType (point, line, lineStrip, triangle, triangleStrip)
uint32_t nvTopology(uint32_t mtlPrimitive);
// The tessellation factor scale m2v's control stage takes (tessellation.cpp): Metal's, rounded to half; false where it is
// not a positive half
bool tessellationScaleBits(float scale, uint32_t &bits);

}  // namespace gm20b
