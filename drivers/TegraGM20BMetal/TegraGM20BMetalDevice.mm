// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalRotated.h"
#import "TegraGM20BMetalXrPlanes.h"
#import "TegraGM20BMetalFresh.h"
#import "TegraGM20BMetalDecoded.h"
#include "surface_note.h"
#include "surface_refusal.h"
#include <mach/vm_page_size.h>
#import "TegraGM20BMetalMultisample.h"

@implementation SwitchMetalDevice

- (void)gm20bCommit:(SwitchMetalCommandBuffer *)cb
{
    commitBuffer(cb);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's attribution: this process's command buffers since the device opened, never reset (the rates line's
// windows stay its own). Keys: raw, reopens (channels the kernel killed and this process opened again), reopen_us (the
// last one's time), set_refusals and set_rebuilds (resource set registrations the kernel refused, and those it took
// once made again without what it refused), pipelined_encoders and sealed_encoders (encoders the worker encoded ahead
// of their commit, of all it sealed), pipeline_handoffs and pipeline_handoff_wakes (hand-offs to the worker as encoders
// ended, and those that found it asleep: a wake each), pipeline_abandons.<reason>, failed (the command buffers that
// completed with an error) and failed.<predicate> (those by the predicate their failure named, failed.none where it
// named none; nonzero only, their sum `failed`), skipped (the calls Metal's validation stops that ran as Apple's driver
// runs them, not failures) and skipped.<predicate> (nonzero only, their sum `skipped`).
- (NSDictionary<NSString *, NSNumber *> *)switchMetalCounters
{
    const Stats &s = gRM->stats;
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"gm20b"] = @(s.raw.load());
    d[@"reopens"] = @(gRM->raw->stats.reopens.load());
    const gm20b::ResourceSet::Counts sets = gRM->raw->setCounts();
    d[@"set_refusals"] = @(sets.refusals);
    d[@"set_rebuilds"] = @(sets.rebuilds);
    {
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        d[@"reopen_us"] = @(gRM->raw->stats.reopenTicks.load() * base.numer / base.denom / 1000);
    }
    d[@"pipelined_encoders"] = @(s.pipelinedEncoders.load());
    d[@"sealed_encoders"] = @(s.sealedEncoders.load());
    d[@"pipeline_handoffs"] = @(s.handoffs.load());
    d[@"pipeline_handoff_wakes"] = @(s.handoffWakes.load());
    static const char *const vetoes[VetoKinds] = {"-",        "counters",   "shadows", "variant", "probes",
                                                  "parallel", "ops",        "refused", "planar",  "native",
                                                  "failed",   "other_work", "device",  "zcull",   "channel"};
    for (uint32_t k = 0; k < VetoKinds; ++k)
        if (const uint64_t n = s.pipelineAbandons[k].load())
            d[[NSString stringWithFormat:@"pipeline_abandons.%s", vetoes[k]]] = @(n);
    d[@"split_prologues"] = @(gRM->raw->stats.splitLaunches.load());
    d[@"split_room_refusals"] = @(gRM->raw->stats.splitRoomRefusals.load());
    d[@"heap_full"] = @(gRM->raw->stats.heapFull.load());
    d[@"pack_slots_in_bound_grains"] = @(gRM->raw->stats.packSlotsInBoundGrains.load());
    d[@"store_slots_placed"] = @(gRM->raw->stats.storeSlotsPlaced.load());
    d[@"slot_cleans"] = @(gRM->raw->stats.slotCleans.load());
    d[@"clears_narrowed"] = @(gRM->raw->stats.clearsNarrowed.load());
    d[@"clear_pixels_left"] = @(gRM->raw->stats.clearPixelsLeft.load());
    d[@"interlock_draws"] = @(gRM->raw->stats.interlockDraws.load());
    d[@"pass_pixel_openings"] = @(gRM->raw->stats.passPixelOpenings.load());
    d[@"raster_ordered_draws"] = @(gRM->raw->stats.rasterOrderedDraws.load());
    d[@"banded_draws"] = @(gRM->raw->stats.bandedDraws.load());
    d[@"bands"] = @(gRM->raw->stats.bands.load());
    d[@"learned_draws"] = @(gRM->raw->stats.learnedDraws.load());
    d[@"priced_static"] = @(gRM->raw->stats.pricedStatic.load());
    d[@"priced_learned"] = @(gRM->raw->stats.pricedLearned.load());
    d[@"unpriced_large"] = @(gRM->raw->stats.unpricedLarge.load());
    d[@"banded_dispatches"] = @(gRM->raw->stats.bandedDispatches.load());
    d[@"storage_barriers"] = @(gRM->raw->stats.storageBarriers.load());
    d[@"texture_uploads"] = @(gRM->raw->stats.textureUploads.load());
    d[@"fused_passes"] = @(gRM->raw->stats.fusedPasses.load());
    d[@"texture_readbacks"] = @(gRM->raw->stats.textureReadbacks.load());
    d[@"blend_kill_states"] = @(gRM->raw->stats.blendKillStates.load());
    d[@"blend_kill_binds"] = @(gRM->raw->stats.blendKillBinds.load());
    d[@"resolve_twod"] = @(gRM->raw->stats.resolveTwod.load());
    d[@"fill_twod"] = @(gRM->raw->stats.fillTwod.load());
    d[@"resolve_exact"] = @(gRM->raw->stats.resolveExact.load());
    d[@"resolve_twod_compressed"] = @(gRM->raw->stats.resolveTwodCompressed.load());
    d[@"zcull_passes"] = @(gRM->raw->stats.zcullPasses.load());
    d[@"zcull_invalidations"] = @(gRM->raw->stats.zcullInvalidations.load());
    d[@"zcull_loads"] = @(gRM->raw->stats.zcullLoads.load());
    d[@"state_segment_binds"] = @(gRM->raw->stats.segmentBinds.load());
    d[@"state_segment_words"] = @(gRM->raw->stats.segmentWords.load());
    d[@"state_segment_skips"] = @(gRM->raw->stats.segmentSkips.load());
    d[@"state_segment_extras_losses"] = @(gRM->raw->stats.segmentExtrasLosses.load());
    d[@"state_segment_copies"] = @(gRM->raw->stats.segmentCopies.load());
    {
        const gm20b::EncodeTotals &encode = gRM->raw->stats.encode;
        const double perUs = gm20b::encodeTicksPerUs();
        d[@"encodes"] = @(encode.encodes.load());
        d[@"encode_us"] = @(uint64_t(double(encode.wallTicks.load()) / perUs));
        d[@"exec_us"] = @(uint64_t(double(encode.ticks[size_t(gm20b::EncodePart::Exec)].load()) / perUs));
        d[@"exec_pushes"] = @(encode.execPushes.load());
    }
    d[@"replay_same"] = @(gRM->raw->stats.replay.same.load());
    d[@"replay_recent"] = @(gRM->raw->stats.replay.recent.load());
    d[@"replay_new"] = @(gRM->raw->stats.replay.fresh.load());
    d[@"stencil_bit_draws"] = @(gRM->raw->stats.stencilBitDraws.load());
    d[@"zeta_colour_draws"] = @(gRM->raw->stats.zetaColourDraws.load());
    d[@"mip_twod_levels"] = @(gRM->raw->stats.mipTwodLevels.load());
    d[@"mip_twod_unproven_levels"] = @(gRM->raw->stats.mipTwodUnproven.load());
    d[@"mip_3d_levels"] = @(gRM->raw->stats.mip3dLevels.load());
    d[@"private_compressed"] = @(gRM->raw->privateMade());
    d[@"private_compressed_moves"] = @(gRM->raw->stats.privateCompressedMoves.load());
#if SWITCH_METAL_DIAGNOSTICS
    d[@"loop_continuations"] = @(gRM->stats.loopContinuations.load());
    d[@"rings_outgrown"] = @(gRM->raw->stats.ringsOutgrown.load());
#endif
    d[@"xr_plane_shadows"] = @(gRM->raw->stats.xrPlaneShadows.load());
    d[@"xr_pair_binds"] = @(gRM->raw->stats.xrPairBinds.load());
    const gm20b::FetchCounts &fetch = gRM->raw->stats.fetch;
    d[@"fetch_sequential"] = @(fetch.sequential.load());
    d[@"fetch_triangles"] = @(fetch.sequentialTriangles.load());
    d[@"fetch_interlocked"] = @(fetch.interlocked.load());
    d[@"fetch_rung2"] = @(fetch.fetchFree[0].load());
    d[@"fetch_rung3"] = @(fetch.fetchFree[1].load());
    d[@"fetch_blend_twin"] = @(fetch.blendTwin.load());
    d[@"fetch_twin_rule"] = @(fetch.twinRule.load());
    for (size_t r = 1; r < gm20b::kFetchRefusals; r++)
        if (const uint64_t n = fetch.refusedAtEncode[r].load())
            d[[NSString stringWithFormat:@"fetch_encode.%s", gm20b::kFetchRefusalNames[r]]] = @(n);
    for (size_t r = 1; r < gm20b::kStorageFallbacks; r++)
        if (const uint64_t n = fetch.storageFallback[r].load())
            d[[NSString stringWithFormat:@"fetch_fallback.%s", gm20b::kStorageFallbackNames[r]]] = @(n);
    uint64_t failed = 0;
    for (int p = 0; p < PredCount; p++)
        if (const uint64_t n = gPredFailed[p].load(std::memory_order_relaxed)) {
            failed += n;
            d[[NSString stringWithFormat:@"failed.%s", failedName(SwitchMetalPred(p))]] = @(n);
        }
    d[@"failed"] = @(failed);
    uint64_t skippedCalls = 0;
    for (int p = 1; p < PredCount; p++)
        if (const uint64_t n = gPredSkipped[p].load(std::memory_order_relaxed)) {
            skippedCalls += n;
            d[[NSString stringWithFormat:@"skipped.%s", kPredNames[p]]] = @(n);
        }
    d[@"skipped"] = @(skippedCalls);
#if SWITCH_METAL_DIAGNOSTICS
    // states created that the pack lacked: found in this build's record cache or compiled (states_compiled)
    if (gRM->completion) {
        const gm20b::Compiler::Stats made = gRM->completion->stats();
        d[@"pack_misses"] = @(made.hits + made.misses);
        d[@"states_compiled"] = @(made.misses);
    }
#endif
    return d;
}

// hw_test_app's system.widgets_render and gpu.gm20b.memoryless_burst: the bytes the device holds now. pool: its pool,
// block-linear and depth chunks; wc: its write-combined chunks; heap: the shader heap's host memory (programs copied);
// heap_in_place: the pack's and the shader compiler's store's pages larger programs run from (file pages, outside the
// footprint); backings: memoryless attachments' uncompressed backings (trimBackings' last count); grows: allocations that
// wired a chunk themselves; headers_left: the texture headers not held (8191 at most, gpu.h kTextureHeaders); textures:
// the memory live textures took of their own and their kept blocks', which returns to where it was once they go (pool
// and wc keep a chunk's worth free); imports_made: surface and no-copy imports since open; arenas_made and arena_restores:
// heaps' arenas made and purged ones bound again since open; host_zeroed: the bytes of untouched host pages imported
// since open, each zero-filled on the importing thread.
// hw_test_app's gpu.path.set_survives_trim: the latest resource set registration the kernel refused, its member and why
- (NSString *)switchMetalSetRefusal
{
    return [NSString stringWithUTF8String:gRM->raw->setLastRefusal().c_str()];
}

- (NSDictionary<NSString *, NSNumber *> *)switchMetalMemory
{
    gm20b::Device &d = *gRM->raw;
    return @{
        @"pool" : @(d.stats.poolBytes.load()),
        @"wc" : @(d.wcChunkBytesHeld()),
        @"heap" : @(d.heapBytes()),
        @"heap_in_place" : @(d.heapInPlaceBytes()),
        @"backings" : @(d.stats.backingBytes.load()),
        @"grows" : @(d.stats.poolStalls.load()),
        @"headers_left" : @(d.headersLeft()),
        @"textures" : @(d.stats.textureBytes.load()),
        @"imports_made" : @(d.stats.importsMade.load()),
        @"arenas_made" : @(d.stats.arenasMade.load()),
        @"arena_restores" : @(d.stats.arenaRestores.load()),
        @"host_zeroed" : @(d.stats.hostZeroedBytes.load()),
        @"parked_chunks" : @(d.stats.parkedChunkBytes.load()),
        @"parked_chunk_count" : @(d.stats.parkedChunks.load()),
        @"chunk_parks" : @(d.stats.chunkParks.load()),
        @"chunk_unparks_resident" : @(d.stats.chunkUnparksResident.load()),
        @"chunk_unparks_reclaimed" : @(d.stats.chunkUnparksReclaimed.load()),
        @"parked_heaps" : @(d.stats.parkedHeapBytes.load()),
        @"parked_heap_count" : @(d.stats.parkedHeaps.load()),
        @"heap_parks" : @(d.stats.heapParks.load()),
        @"heap_unparks_resident" : @(d.stats.heapUnparksResident.load()),
        @"heap_unparks_reclaimed" : @(d.stats.heapUnparksReclaimed.load())
    };
}

// hw_test_app's gpu.compression_churn and gpu.gm20b.compressed_targets: the device's compressed memory (comp.h).
// held, held_bytes and views are now; made and refused count since the device opened, and so do the clears of compressed
// targets the ZBC table held or not and the entries this process added (zbc.h); ce_copies: a shadow's fills and stores
// through compressed memory; storage_fallback_compressed: draws whose storage form a compressed attachment turned away.
- (NSDictionary<NSString *, NSNumber *> *)switchMetalCompression
{
    const auto &s = gRM->raw->stats;
    return @{
        @"held" : @(s.compHeld.load()),
        @"held_bytes" : @(s.compHeldBytes.load()),
        @"views" : @(s.compViews.load()),
        @"made" : @(s.compMade.load()),
        @"refused" : @(s.compFailed.load()),
        @"zbc_hits" : @(s.zbcHits.load()),
        @"zbc_misses" : @(s.zbcMisses.load()),
        @"zbc_added" : @(s.zbcAdded.load()),
        @"zbc_full" : @(s.zbcFull.load()),
        @"zbc_inexact" : @(s.zbcInexact.load()),
        @"ce_copies" : @(s.compCopies.load()),
        @"zeta_ce_copies" : @(s.compZetaCopies.load()),
        @"storage_fallback_compressed" : @(s.fetch.storageFallback[size_t(gm20b::StorageFallback::Compressed)].load()),
        @"misses" : @(s.compMisses.load())
    };
}

// hw_test_app's gpu.gm20b.texel_buffer_reach: a texel buffer of `n` texels from `b`'s first byte past the device's
// maxTextureBufferWidth answer (NVK's 2^27; the header's width field holds 32 bits), nil where none is made
- (id<MTLTexture>)switchMetalTexelBuffer:(id<MTLBuffer>)b format:(MTLPixelFormat)f width:(NSUInteger)n
{
    SwitchMetalBuffer *buffer = [(id)b isKindOfClass:[SwitchMetalBuffer class]] ? (SwitchMetalBuffer *)b : nil;
    if (!buffer || n > UINT32_MAX)
        return nil;
    SwitchMetalTexture *t = [SwitchMetalTexture new];
    t->_device = self;
    t->_kind = SwitchMetalTextureBuffer;
    t->_buffer = buffer;
    t->_descriptor = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:f
                                                                            width:n
                                                                  resourceOptions:b.resourceOptions
                                                                            usage:MTLTextureUsageShaderRead];
    t->_gm20bOk = gRM->raw->createTexelBuffer(buffer->_raw, 0, uint32_t(n), uint32_t(f), t->_raw, UINT32_MAX);
    return t->_gm20bOk ? (id<MTLTexture>)t : nil;
}
#endif

// Alignments Core Animation lays its memory out by (QuartzCore 21H461), answered for what the driver takes as it is:
//  - its buffer pool's suballocations (CA::OGL::MetalBufferPool::get_buffer: max(this, the argument's)): the A10X's 4, a
//    uniform buffer the driver binds at the multiple of 256 below it with m2v's residual in its push word (gm20b::PackEntry);
//  (not iosurfaceReadOnlyTextureAlignmentBytes: QuartzCore makes it the render server's needed IOSurface row alignment,
//  CADeviceSetMinimumAlignment, and hands it to every client, CA::Context::connect_remote; MetalContext::update_surface then
//  refuses every surface whose rows miss it, "Row bytes (N) not 128 aligned", and the layer draws nothing (r7p5: 3347 refusals
//  in backboardd, Safari images and widgets blank). Its answer stays 16 (device_answers.h), though the texture
//  header takes rows of 32 and a pitch render target rows of 128.)
//  - the image memory it samples in place (CA::OGL::MetalContext::copy_image_to_texture: offset and rows a multiple of
//    deviceLinearReadOnlyTextureAlignmentBytes, else it copies them into a texture of its own): the texture header's 32,
//    device_answers.h's row;
//  - the rows of its CPU readbacks (CAMetalTextureGetBytesPerRow).
- (NSUInteger)minConstantBufferAlignmentBytes
{
    return 4;
}

- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format
{
    const NSUInteger a = NSUInteger(deviceAnswer(@selector(minimumLinearTextureAlignmentForPixelFormat:), format));
    return a ? std::max<NSUInteger>(a, 32) : a;
}

- (id<MTLCommandQueue>)newCommandQueue
{
    SwitchMetalQueue *q = [SwitchMetalQueue new];
    q->_device = self;
    return (id<MTLCommandQueue>)q;
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n
{
    (void)n;
    return [self newCommandQueue];
}

- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:(id)d
{
    (void)d;
    return [self newCommandQueue];
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    SwitchMetalBuffer *b = [SwitchMetalBuffer new];
    b->_device = self;
    b->_options = o;
    // a shared buffer: write-combined (gm20b::Buffer::wc), as every one Core Animation makes is written by the CPU and read by the
    // GPU (its buffer pool, its gamma and weight tables: QuartzCore MetalBufferPool::allocate_buffer, options 0): nothing to clean
    // before a draw reads it, nothing to scan for the vertices an indexed draw reads (r16: 600-1300 KB cleaned a command buffer)
    const bool wc = (o & MTLResourceStorageModeMask) == MTLResourceStorageModeShared;
    const bool hostVisible = (o & MTLResourceStorageModeMask) != MTLResourceStorageModePrivate;
    if (!gRM->raw->createBuffer(n ? n : 16, b->_raw, wc, hostVisible, wc)) {
        os_log_error(gm20bLog(), "SwitchMetal: %{public}s: no memory for a %lu-byte buffer: none made", getprogname(),
                     (unsigned long)n);
        return nil;
    }
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    id<MTLBuffer> b = [self newBufferWithLength:n options:o];
    if (!b || !p || !n)
        return b;
    SwitchMetalBuffer *buffer = (SwitchMetalBuffer *)b;
    gm20b::HostAccess access;
    if (!buffer->_raw.hostVisible && !gRM->raw->hostAccess(buffer->_raw, true, access))
        return nil;
    memcpy(buffer->_raw.cpu, p, n);
    if (!access.finish())
        return nil;
    return b;
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)p
                                   length:(NSUInteger)n
                                  options:(MTLResourceOptions)o
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
    SWITCH_API(ApiObject);
    SwitchMetalBuffer *b = [SwitchMetalBuffer new];
    b->_device = self;
    b->_options = o;
    // The kernel wires pages the task maps read-only for GPU reads (TegraGM20BUserClientBo.cpp boImport): what it refuses
    // no device of this GPU imports. An M4 Max makes a buffer over an IOSurface's pages that reads and writes its texels
    // as rows; the pages of one the kernel laid out block-linear hold no rows to alias.
    bool tiled = false;
    if (!gRM->raw->createBufferNoCopy(p, n, b->_raw, tiled)) {
        if (tiled)
            os_log_error(
                gm20bLog(),
                "SwitchMetal: %{public}s: newBufferWithBytesNoCopy over %lu bytes at %p: they lie in an IOSurface this "
                "process imported block-linear, whose pages hold its texels in GOBs: no buffer",
                getprogname(), (unsigned long)n, p);
        else
            os_log_error(gm20bLog(),
                         "SwitchMetal: %{public}s: the kernel imported no %lu bytes at %p: no no-copy buffer made",
                         getprogname(), (unsigned long)n, p);
        return nil;
    }
    b->_deallocator = [deallocator copy];
    return (id<MTLBuffer>)b;
}

// (SPI) A surface of one plane as a buffer over its pages, as m2v makes one: nil for a planar, unaligned or block-linear
// surface. The GPU's writes are declared at once, so a CPU lock waits for them.
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)s
{
    SWITCH_API(ApiObject);
    if (!s || IOSurfaceGetPlaneCount(s))
        return nil;
    void *base = IOSurfaceGetBaseAddress(s);
    const size_t page = vm_page_size, allocation = IOSurfaceGetAllocSize(s);
    if (!base || (uintptr_t(base) & (page - 1)) || !allocation || allocation > SIZE_MAX - (page - 1))
        return nil;
    SwitchMetalBuffer *b = [SwitchMetalBuffer new];
    b->_device = self;
    b->_options = MTLResourceStorageModeShared;
    bool tiled = false;
    if (!gRM->raw->createSurfaceBuffer(IOSurfaceGetID(s), base, (allocation + page - 1) & ~(page - 1), s, b->_raw,
                                       tiled)) {
        if (tiled)
            SWITCH_REFUSE_DETAIL("gm20b", "buf.surface.tiled", IOSurfaceGetID(s),
                                 "newBufferWithIOSurface: of IOSurface %u, %zu bytes: the kernel laid it out "
                                 "block-linear, so its pages hold its texels in GOBs: no buffer",
                                 IOSurfaceGetID(s), allocation);
        else
            os_log_error(gm20bLog(),
                         "SwitchMetal: %{public}s: the kernel imported no IOSurface %u: no buffer over it made",
                         getprogname(), IOSurfaceGetID(s));
        return nil;
    }
    if (!gRM->raw->surfaceGpuWrites(IOSurfaceGetID(s)))
        os_log_error(gm20bLog(),
                     "SwitchMetal: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
                     IOSurfaceGetID(s));
    return (id<MTLBuffer>)b;
}

#if SWITCH_FRAME_TIMELINE
static void noteCompositorTarget(MTLTextureDescriptor *d, const void *frame)
{
    constexpr NSUInteger kPanel = 1280 * 720;
    if (!(d.usage & MTLTextureUsageRenderTarget) || d.width * d.height <= kPanel || !gRM || !gRM->raw ||
        gRM->raw->role() != gm20b::Device::Role::RenderServer || !gm20b::noteAllowed())
        return;
    gm20b::postTargetNote({uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), uint32_t(d.usage),
                           uint32_t(d.storageMode), uint32_t(d.sampleCount), uint32_t(d.textureType),
                           switch_frame::callers(frame)});
}
#endif

// A private or shared texture of its own memory is block-linear (TegraGM20BLayout.h), as NVK lays out an optimal image and
// m2v's view of it is (texture_tiled_in_place). The CPU reaches a shared one only through replaceRegion and getBytes,
// which go through the copy engine (transferTexture) or swizzle (cpuTiled); textures over buffers, IOSurfaces and
// caller pages stay pitch, as another binding names those bytes.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
#if SWITCH_FRAME_TIMELINE
    noteCompositorTarget(d, __builtin_frame_address(0));
#endif
    id<MTLTexture> made =
        swizzled([self gm20bNewTexture:d
                                 tiled:d.storageMode == MTLStorageModePrivate || d.storageMode == MTLStorageModeShared],
                 d.swizzle);
    gm20bWantCompressed(made);
    return made;
}

// Metal's, the identity's and GM20B's refusal of a descriptor's texture: one error line per rule, and nil
static bool refusedTexture(const gm20b::shapes::TextureShape &s)
{
    char shape[160];
    snprintf(shape, sizeof shape,
             "%llux%llux%llu of format %u, type %u, %llu levels, %llu samples, array %llu, storage %u",
             (unsigned long long)s.width, (unsigned long long)s.height, (unsigned long long)s.depth, s.format, s.type,
             (unsigned long long)s.levels, (unsigned long long)s.samples, (unsigned long long)s.array, s.storage);
    if (const char *why = gm20b::shapes::metalRefusal(s)) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.metal", uintptr_t(why),
                             "no %s texture: nil, as Metal makes none (%s; MTLDevice.h newTextureWithDescriptor:, "
                             "nullable)",
                             shape, why);
        return true;
    }
    if (const char *why = gm20b::shapes::identityRefusal(s, gm20b::answers::kThisKind)) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.identity", uintptr_t(why),
                             "no %s texture: nil, as the identity's Metal makes none (%s; MTLDevice.h "
                             "newTextureWithDescriptor:, nullable)",
                             shape, why);
        return true;
    }
    if (const char *why = gm20b::shapes::hardwareRefusal(s)) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.gm20b", uintptr_t(why),
                             "no %s texture: nil (%s; MTLDevice.h newTextureWithDescriptor:, nullable)", shape, why);
        return true;
    }
    return false;
}

// The first texture of a kind the driver holds in memory of its own, once per process
static void announceTexture(gm20b::shapes::Route route, const gm20b::shapes::TextureShape &s, const gm20b::Texture &t)
{
    using Route = gm20b::shapes::Route;
    if (route == Route::Chain) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            os_log(gm20bLog(), "SwitchMetal: mip chain on (%llu levels of %llux%llu, %llu bytes)",
                   (unsigned long long)s.levels, (unsigned long long)s.width, (unsigned long long)s.height,
                   (unsigned long long)t.chain->layout.bytes);
    } else if (route == Route::StoredSamples) {
        const bool zeta = gm20b::shapes::storedSamples(s) == gm20b::shapes::StoredSamples::Zeta;
        static std::atomic<bool> said[2];
        if (!said[zeta].exchange(true))
            os_log(gm20bLog(),
                   "SwitchMetal: stored %llux multisampled %{public}s on (first %llux%llu format %u: %llu bytes of "
                   "%{public}s)",
                   (unsigned long long)s.samples, zeta ? "depth/stencil" : "colour", (unsigned long long)s.width,
                   (unsigned long long)s.height, s.format, (unsigned long long)(zeta ? t.storage.bytes : t.span.bytes),
                   zeta ? "zeta memory" : "block-linear memory");
    } else if (route == Route::StoredZeta) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            os_log(
                gm20bLog(),
                "SwitchMetal: stored one-sample depth/stencil on (first %llux%llu format %u, %u layers: %llu bytes of "
                "%{public}s zeta memory)",
                (unsigned long long)s.width, (unsigned long long)s.height, s.format, unsigned(t.layers),
                (unsigned long long)t.storage.bytes, t.comptags ? "compressed" : "plain");
    }
}

- (id<MTLTexture>)gm20bNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled
{
    if (refuseTurnedOffSurface(d))
        return nil;
    using Route = gm20b::shapes::Route;
    gm20b::shapes::TextureShape s = textureShape(d);
    s.ownMemory = true;
    if (refusedTexture(s))
        return nil;
    // X32_Stencil8: Depth32Float_Stencil8's memory read through its stencil alone (NIL's X32_S8X24_UINT: the combined
    // format's header, its stencil in green), a view of a texture of the combined format it keeps
    if (d.pixelFormat == MTLPixelFormatX32_Stencil8) {
        MTLTextureDescriptor *combined = [d copy];
        combined.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        SwitchMetalTexture *memory = (SwitchMetalTexture *)[self gm20bNewTexture:combined tiled:tiled];
        return memory ? [(id<MTLTexture>)memory newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8] : nil;
    }
    // a texture buffer of the device's: its texels in a buffer of its own, under a one-dimensional buffer header (TIC
    // ONE_D_BUFFER, at most 2^27 texels: Device::createTexelBuffer)
    if (const gm20b::FormatInfo *f = gm20b::formatInfo(uint32_t(d.pixelFormat));
        f && d.textureType == MTLTextureTypeTextureBuffer) {
        const uint64_t bytes = gm20b::texelBufferBytes(uint32_t(d.pixelFormat), d.width, f->bpp);
        id<MTLBuffer> memory = [self newBufferWithLength:NSUInteger(bytes) options:d.resourceOptions];
        return memory ? [memory newTextureWithDescriptor:d offset:0 bytesPerRow:d.width * f->bpp] : nil;
    }
    // every shape Metal, the identity and GM20B make has a creator (texture_routes_test walks them all): none is the
    // driver's gap, never the client's
    const Route route = gm20b::shapes::route(s);
    if (route == Route::None) {
        os_log_fault(gm20bLog(),
                     "SwitchMetal: %{public}s: no creator for a %lux%lu texture of format %lu, type %lu: nil",
                     getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                     (unsigned long)d.textureType);
        return nil;
    }
    SwitchMetalTexture *t = [SwitchMetalTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = route == Route::Memoryless ? SwitchMetalTextureMemoryless : SwitchMetalTexturePool;
    const uint32_t kind = tiled || route != Route::Plain ? privateCompressedKind(d) : 0;
    t->_gm20bOk = gm20b::shapes::create(*gRM->raw, s, route, tiled, kind, t->_raw);
    if (t->_gm20bOk)
        announceTexture(route, s, t->_raw);
    else if (route == Route::StoredZeta)
        SWITCH_REFUSE_DETAIL("gm20b", "zeta.owned", d.pixelFormat,
                             "a stored %lux%lu depth/stencil texture of format %lu: no zeta memory or texture header "
                             "for it",
                             (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
    if (t->_gm20bOk && route != Route::Memoryless && !keepExtendedRange(d, t->_raw, false)) {
        gRM->raw->destroyTexture(t->_raw);
        t->_gm20bOk = false;
    }
    if (!t->_gm20bOk) {
        os_log_error(gm20bLog(), "SwitchMetal: %{public}s: no %{public}s for a %lux%lux%lu format %lu texture: nil",
                     getprogname(), gRM->raw->headersLeft() ? "memory" : "texture header (every one is held)",
                     (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth,
                     (unsigned long)d.pixelFormat);
        return nil;
    }
    if (route != Route::Block)
        establishFresh(t, d);
    if (!gm20bKeepDecodedBlocks(t)) {
        os_log_error(gm20bLog(),
                     "SwitchMetal: %{public}s: no memory for the blocks a %lux%lu format %lu texture keeps: nil",
                     getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
        return nil;
    }
    if (kind && !t->_raw.comptags && !t->_raw.chain)
        t->_movableBytes = t->_raw.storageRange().bytes;
    return (id<MTLTexture>)t;
}

@end
