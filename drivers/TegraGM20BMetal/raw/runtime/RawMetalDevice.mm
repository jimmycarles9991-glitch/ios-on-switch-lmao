// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#import "RawMetalRotated.h"
#import "RawMetalXrPlanes.h"
#import "RawMetalFresh.h"
#include "raw_surface_note.h"
#include <mach/vm_page_size.h>
#include "NativeBridgeMetalFrontend.h"
#import "RawMetalMultisample.h"

// Why the raw path does not take a stored depth/stencil texture's m2v texture in place, from the answer of
// native_bridge_metal_texture_memory (-3: m2v made no texture)
@implementation RMDevice

- (void)rmCommit:(RMCommandBuffer *)cb
{
    commitBuffer(cb);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's attribution: this process's command buffers on each path since the device opened, never reset (the rates
// line's windows stay its own). Keys: raw, m2v, split, split_m2v, aborted, reopens (channels the kernel killed and this
// process opened again), reopen_us (the last one's time), set_refusals and set_rebuilds (resource set registrations
// the kernel refused, and those it took once made again without what it refused), gpu_order (1: the drivers order on
// the GPU now), cpu_ordered (the Orders the kernel refused, each a CPU-ordered line), pipelined_encoders and
// sealed_encoders (encoders the worker encoded ahead of their commit, of all it sealed), pipeline_handoffs and
// pipeline_handoff_wakes (hand-offs to the worker as encoders ended, and those that found it asleep: a wake each),
// pipeline_abandons.<reason>,
// m2v.<reason>, pred.<predicate> and failed.<predicate> (the command buffers a predicate of predFailure failed;
// nonzero only).
- (NSDictionary<NSString *, NSNumber *> *)switchRawCounters
{
    const Stats &s = gRM->stats;
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    d[@"raw"] = @(s.raw.load());
    d[@"m2v"] = @(s.m2v.load());
    d[@"split"] = @(s.splits.load());
    d[@"split_m2v"] = @(s.splitM2v.load());
    d[@"aborted"] = @(s.aborted.load());
    d[@"reopens"] = @(gRM->raw->stats.reopens.load());
    const raw::ResourceSet::Counts sets = gRM->raw->setCounts();
    d[@"set_refusals"] = @(sets.refusals);
    d[@"set_rebuilds"] = @(sets.rebuilds);
    {
        mach_timebase_info_data_t base;
        mach_timebase_info(&base);
        d[@"reopen_us"] = @(gRM->raw->stats.reopenTicks.load() * base.numer / base.denom / 1000);
        std::lock_guard<raw::Mutex> m(gRM->m2vLock);
        d[@"gpu_order"] = @(gRM->gpuOrder ? 1 : 0);
    }
    d[@"cpu_ordered"] = @(s.cpuOrdered.load());
    d[@"pipelined_encoders"] = @(s.pipelinedEncoders.load());
    d[@"sealed_encoders"] = @(s.sealedEncoders.load());
    d[@"pipeline_handoffs"] = @(s.handoffs.load());
    d[@"pipeline_handoff_wakes"] = @(s.handoffWakes.load());
    static const char *const vetoes[VetoKinds] = {"whole",    "counters", "shadows",    "storage", "probes",
                                                  "parallel", "ops",      "refused",    "planar",  "contents",
                                                  "native",   "failed",   "other_work", "device",  "zcull"};
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
    d[@"raster_ordered_draws"] = @(gRM->raw->stats.rasterOrderedDraws.load());
    d[@"banded_draws"] = @(gRM->raw->stats.bandedDraws.load());
    d[@"bands"] = @(gRM->raw->stats.bands.load());
    d[@"learned_draws"] = @(gRM->raw->stats.learnedDraws.load());
    d[@"banded_dispatches"] = @(gRM->raw->stats.bandedDispatches.load());
    d[@"kernel_windows"] = @(gRM->raw->stats.kernelWindows.load());
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
        const raw::EncodeTotals &encode = gRM->raw->stats.encode;
        const double perUs = raw::encodeTicksPerUs();
        d[@"encodes"] = @(encode.encodes.load());
        d[@"encode_us"] = @(uint64_t(double(encode.wallTicks.load()) / perUs));
        d[@"exec_us"] = @(uint64_t(double(encode.ticks[size_t(raw::EncodePart::Exec)].load()) / perUs));
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
    d[@"private_moves"] = @(gRM->raw->privateTransitions());
#if SWITCH_RAW_DIAGNOSTICS
    d[@"loop_continuations"] = @(gRM->stats.loopContinuations.load());
#endif
    d[@"xr_plane_passes"] = @(gRM->raw->stats.xrPlanePasses.load());
    d[@"xr_plane_shadows"] = @(gRM->raw->stats.xrPlaneShadows.load());
    d[@"xr_pair_binds"] = @(gRM->raw->stats.xrPairBinds.load());
    const raw::FetchCounts &fetch = gRM->raw->stats.fetch;
    d[@"fetch_sequential"] = @(fetch.sequential.load());
    d[@"fetch_triangles"] = @(fetch.sequentialTriangles.load());
    d[@"fetch_interlocked"] = @(fetch.interlocked.load());
    d[@"fetch_rung2"] = @(fetch.fetchFree[0].load());
    d[@"fetch_rung3"] = @(fetch.fetchFree[1].load());
    d[@"fetch_blend_twin"] = @(fetch.blendTwin.load());
    d[@"fetch_twin_rule"] = @(fetch.twinRule.load());
    for (size_t r = 1; r < raw::kFetchRefusals; r++) {
        if (const uint64_t n = fetch.refusedAtRecord[r].load())
            d[[NSString stringWithFormat:@"fetch_record.%s", raw::kFetchRefusalNames[r]]] = @(n);
        if (const uint64_t n = fetch.refusedAtEncode[r].load())
            d[[NSString stringWithFormat:@"fetch_encode.%s", raw::kFetchRefusalNames[r]]] = @(n);
    }
    for (size_t r = 1; r < raw::kStorageFallbacks; r++)
        if (const uint64_t n = fetch.storageFallback[r].load())
            d[[NSString stringWithFormat:@"fetch_fallback.%s", raw::kStorageFallbackNames[r]]] = @(n);
    for (int r = 1; r < WhyCount; r++)
        if (const uint64_t n = s.reasons[r].load())
            d[[NSString stringWithFormat:@"m2v.%s", kReasonNames[r]]] = @(n);
    for (int p = 1; p < PredCount; p++) {
        if (const uint64_t n = gPredCbs[p].load(std::memory_order_relaxed))
            d[[NSString stringWithFormat:@"pred.%s", kPredNames[p]]] = @(n);
        if (const uint64_t n = gPredFailed[p].load(std::memory_order_relaxed))
            d[[NSString stringWithFormat:@"failed.%s", kPredNames[p]]] = @(n);
    }
#if SWITCH_RAW_DIAGNOSTICS
    // states created that the pack lacked: found in this build's record cache or compiled (states_compiled)
    if (gRM->completion) {
        const raw::Compiler::Stats made = gRM->completion->stats();
        d[@"pack_misses"] = @(made.hits + made.misses);
        d[@"states_compiled"] = @(made.misses);
    }
#endif
    return d;
}

// hw_test_app's system.widgets_render and gpu.raw.memoryless_burst: the bytes the raw device holds now. pool: its pool,
// block-linear and depth chunks; wc: its write-combined chunks; heap: the shader heap's host memory (programs copied);
// heap_in_place: the pack's and the shader compiler's store's pages larger programs run from (file pages, outside the
// footprint); backings: memoryless attachments' uncompressed backings (trimBackings' last count); grows: allocations that
// wired a chunk themselves.
// hw_test_app's gpu.path.set_survives_trim: the latest resource set registration the kernel refused, its member and why
- (NSString *)switchRawSetRefusal
{
    return [NSString stringWithUTF8String:gRM->raw->setLastRefusal().c_str()];
}

- (NSDictionary<NSString *, NSNumber *> *)switchRawMemory
{
    raw::Device &d = *gRM->raw;
    return @{
        @"pool" : @(d.stats.poolBytes.load()),
        @"wc" : @(d.wcChunkBytesHeld()),
        @"heap" : @(d.heapBytes()),
        @"heap_in_place" : @(d.heapInPlaceBytes()),
        @"backings" : @(d.stats.backingBytes.load()),
        @"grows" : @(d.stats.poolStalls.load())
    };
}

// hw_test_app's gpu.compression_churn and gpu.raw.compressed_targets: the raw device's compressed memory (raw_comp.h).
// held, held_bytes and views are now; made and refused count since the device opened, and so do the clears of compressed
// targets the ZBC table held or not and the entries this process added (raw_zbc.h); ce_copies: a shadow's fills and stores
// through compressed memory; storage_fallback_compressed: draws whose storage form a compressed attachment turned away.
- (NSDictionary<NSString *, NSNumber *> *)switchRawCompression
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
        @"storage_fallback_compressed" : @(s.fetch.storageFallback[size_t(raw::StorageFallback::Compressed)].load()),
        @"misses" : @(s.compMisses.load())
    };
}

// The m2v device this one wraps (the one TEGRA_RAW_METAL=0 gives the process): hw_test_app draws each scene on both
- (id<MTLDevice>)switchM2VDevice
{
    return RM_M2V(NeedHwTestDevice);
}
#endif

// Alignments Core Animation lays its memory out by (QuartzCore 21H461), answered for what the raw path takes as it is; m2v takes
// any multiple of 16 (its own answers), so the m2v twins of the same memory are unchanged in what they hold:
//  - its buffer pool's suballocations (CA::OGL::MetalBufferPool::get_buffer: max(this, the argument's)): the A10X's 4, a
//    uniform buffer the raw path binds at the multiple of 256 below it with m2v's residual in its push word (raw::PackEntry);
//  (not iosurfaceReadOnlyTextureAlignmentBytes: QuartzCore makes it the render server's needed IOSurface row alignment,
//  CADeviceSetMinimumAlignment, and hands it to every client, CA::Context::connect_remote; MetalContext::update_surface then
//  refuses every surface whose rows miss it, "Row bytes (N) not 128 aligned", and the layer draws nothing (r7p5: 3347 refusals
//  in backboardd, Safari images and widgets blank). The m2v device's answer, 16, stands; rows the raw path cannot take stay
//  m2v's: its texture header takes rows of 32, a pitch render target rows of 128.)
//  - the image memory it samples in place (CA::OGL::MetalContext::copy_image_to_texture: offset and rows a multiple of
//    deviceLinearReadOnlyTextureAlignmentBytes, else it copies them into a texture of its own): the texture header's 32,
//    raw_device_answers.h's row;
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

// (a selector only m2v's device class has a method for opens the m2v device at its first use: RawMetalM2V.mm)
- (id<MTLCommandQueue>)newCommandQueue
{
    RMQueue *q = [RMQueue new];
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
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    // a shared buffer: write-combined (raw::Buffer::wc), as every one Core Animation makes is written by the CPU and read by the
    // GPU (its buffer pool, its gamma and weight tables: QuartzCore MetalBufferPool::allocate_buffer, options 0): nothing to clean
    // before a draw reads it, nothing to scan for the vertices an indexed draw reads (r16: 600-1300 KB cleaned a command buffer)
    const bool wc = (o & MTLResourceStorageModeMask) == MTLResourceStorageModeShared;
    if (!gRM->raw->createBuffer(n ? n : 16, b->_raw, wc)) {
        os_log_error(rawLog(), "raw: %{public}s: no memory for a %lu-byte buffer: none made", getprogname(),
                     (unsigned long)n);
        return nil;
    }
    b->_raw.hostVisible = (o & MTLResourceStorageModeMask) != MTLResourceStorageModePrivate;
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    id<MTLBuffer> b = [self newBufferWithLength:n options:o];
    if (!b || !p || !n)
        return b;
    RMBuffer *buffer = (RMBuffer *)b;
    raw::HostAccess access;
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
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = o;
    // The kernel wires pages the task maps read-only for GPU reads (TegraGM20BUserClientBo.cpp boImport): what it refuses
    // no device of this GPU imports
    if (!gRM->raw->createBufferNoCopy(p, n, b->_raw)) {
        os_log_error(rawLog(), "raw: %{public}s: the kernel imported no %lu bytes at %p: no no-copy buffer made",
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
    RMBuffer *b = [RMBuffer new];
    b->_device = self;
    b->_options = MTLResourceStorageModeShared;
    bool tiled = false;
    if (!gRM->raw->createSurfaceBuffer(IOSurfaceGetID(s), base, (allocation + page - 1) & ~(page - 1), s, b->_raw,
                                       tiled)) {
        if (!tiled)
            os_log_error(rawLog(), "raw: %{public}s: the kernel imported no IOSurface %u: no buffer over it made",
                         getprogname(), IOSurfaceGetID(s));
        return nil;
    }
    if (!gRM->raw->surfaceGpuWrites(IOSurfaceGetID(s)))
        os_log_error(rawLog(),
                     "raw: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
                     IOSurfaceGetID(s));
    return (id<MTLBuffer>)b;
}

- (id<MTLTexture>)wrapTwin:(id<MTLTexture>)twin descriptor:(MTLTextureDescriptor *)d
{
    if (!twin)
        return nil;
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_twin = twin;
    t->_kind = RMTextureTwinOnly;
    t->_descriptor = [d copy];
    return (id<MTLTexture>)t;
}

// A private or shared texture of its own memory is block-linear (TegraGM20BLayout.h), as NVK lays out an optimal image and
// m2v's view of it is (texture_tiled_in_place). The CPU reaches a shared one only through replaceRegion and getBytes,
// which go through the copy engine (transferTexture) or swizzle (cpuTiled); textures over buffers, IOSurfaces and
// caller pages stay pitch, as another binding names those bytes.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
#if SWITCH_FRAME_TIMELINE
    noteCompositorTarget(d, __builtin_frame_address(0));
#endif
    return swizzled([self rmNewTexture:d
                                 tiled:d.storageMode == MTLStorageModePrivate || d.storageMode == MTLStorageModeShared],
                    d.swizzle);
}

// Metal's, the identity's and GM20B's refusal of a descriptor's texture: one error line per rule, and nil
static bool refusedTexture(const raw::shapes::TextureShape &s)
{
    char shape[160];
    snprintf(shape, sizeof shape,
             "%llux%llux%llu of format %u, type %u, %llu levels, %llu samples, array %llu, storage %u",
             (unsigned long long)s.width, (unsigned long long)s.height, (unsigned long long)s.depth, s.format, s.type,
             (unsigned long long)s.levels, (unsigned long long)s.samples, (unsigned long long)s.array, s.storage);
    if (const char *why = raw::shapes::metalRefusal(s)) {
        SWITCH_REFUSE_DETAIL("raw", "tex.metal", uintptr_t(why),
                             "no %s texture: nil, as Metal makes none (%s; MTLDevice.h newTextureWithDescriptor:, "
                             "nullable)",
                             shape, why);
        return true;
    }
    if (const char *why = raw::shapes::identityRefusal(s, raw::answers::kThisKind)) {
        SWITCH_REFUSE_DETAIL("raw", "tex.identity", uintptr_t(why),
                             "no %s texture: nil, as the identity's Metal makes none (%s; MTLDevice.h "
                             "newTextureWithDescriptor:, nullable)",
                             shape, why);
        return true;
    }
    if (const char *why = raw::shapes::hardwareRefusal(s)) {
        SWITCH_REFUSE_DETAIL("raw", "tex.gm20b", uintptr_t(why),
                             "no %s texture: nil (%s; MTLDevice.h newTextureWithDescriptor:, nullable)", shape, why);
        return true;
    }
    return false;
}

// The first texture of a kind the raw path holds in memory of its own, once per process
static void announceTexture(raw::shapes::Route route, const raw::shapes::TextureShape &s, const raw::Texture &t)
{
    using Route = raw::shapes::Route;
    if (route == Route::Chain) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            os_log(rawLog(), "raw: mip chain on (%llu levels of %llux%llu, %llu bytes)", (unsigned long long)s.levels,
                   (unsigned long long)s.width, (unsigned long long)s.height,
                   (unsigned long long)t.chain->layout.bytes);
    } else if (route == Route::StoredSamples) {
        const bool zeta = raw::shapes::storedSamples(s) == raw::shapes::StoredSamples::Zeta;
        static std::atomic<bool> said[2];
        if (!said[zeta].exchange(true))
            os_log(rawLog(),
                   "raw: stored %llux multisampled %{public}s on (first %llux%llu format %u: %llu bytes of %{public}s)",
                   (unsigned long long)s.samples, zeta ? "depth/stencil" : "colour", (unsigned long long)s.width,
                   (unsigned long long)s.height, s.format, (unsigned long long)(zeta ? t.storage.bytes : t.span.bytes),
                   zeta ? "zeta memory, no m2v twin" : "block-linear memory, m2v's twin NVK's image over it");
    } else if (route == Route::StoredZeta) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            os_log(rawLog(),
                   "raw: stored one-sample depth/stencil on (first %llux%llu format %u, %u layers: %llu bytes of "
                   "%{public}s zeta memory, no m2v twin)",
                   (unsigned long long)s.width, (unsigned long long)s.height, s.format, unsigned(t.layers),
                   (unsigned long long)t.storage.bytes, t.comptags ? "compressed" : "plain");
    }
}

- (id<MTLTexture>)rmNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled
{
    if (refuseTurnedOffSurface(d))
        return nil;
    using Route = raw::shapes::Route;
    raw::shapes::TextureShape s = textureShape(d);
    s.ownMemory = true;
    if (refusedTexture(s))
        return nil;
    const Route route = raw::shapes::route(s);
    if (route == Route::None) {
        id<MTLDevice> m2v = RM_M2V(NeedTextureShape);
        RMTexture *w = (RMTexture *)[self wrapTwin:[m2v newTextureWithDescriptor:d] descriptor:d];
        if (w)
            refuseTexture(w, PredTexShape, raw::shapes::rawShapeTerm(s), d.pixelFormat, @"twin-only", nil);
        return (id<MTLTexture>)w;
    }
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = route == Route::Memoryless ? RMTextureMemoryless : RMTexturePool;
    const uint32_t kind = tiled || route != Route::Plain ? privateCompressedKind(d) : 0;
    t->_rawOk = raw::shapes::create(*gRM->raw, s, route, tiled, kind, t->_raw);
    if (t->_rawOk)
        announceTexture(route, s, t->_raw);
    else if (route == Route::StoredZeta)
        SWITCH_REFUSE_DETAIL("raw", "zeta.owned", d.pixelFormat,
                             "a stored %lux%lu depth/stencil texture of format %lu: no zeta memory for it",
                             (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
    if (t->_rawOk && route != Route::Memoryless)
        keepExtendedRange(d, t->_raw, false);
    if (!t->_rawOk) {
        os_log_error(rawLog(), "raw: %{public}s: no memory for a %lux%lux%lu format %lu texture: none made",
                     getprogname(), (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth,
                     (unsigned long)d.pixelFormat);
        return nil;
    }
    if (route != Route::Block)
        establishFresh(t, d);
    return (id<MTLTexture>)t;
}

// RGB8_444_2P (542) over plane 0 of a two-plane 4:4:4 surface of one byte a luma texel and two a chroma pair (CoreVideo's
// '_RB4', the widget and poster snapshots Core Animation samples): Metal reads its codes, red Cr, green Y, blue Cb, alpha 1
// (m2v ycbcr.rs), which the raw path holds in an RGBA8 image it writes from the planes with the copy engine before the work that
// samples it (raw::BlitEncoder::convertPlanar). A 2D texture no taller than the surface and as wide as its rows hold (the
// planes' top-left corner, as Metal reads and stores it), one level, no rotation, sampled or rendered (WidgetRenderer's
// snapshots: through its stand-in, raw::Texture::planarStandIn), of any usage. Its stand-in is pool memory (8 bytes a texel),
// made for a render target usage alone: an Unknown usage's would hold one under every snapshot Core Animation samples.
static bool planar444(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.pixelFormat == kRGB8_444_2P && plane == 0 && IOSurfaceGetPlaneCount(s) == 2 &&
           d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned &&
           IOSurfaceGetBytesPerRowOfPlane(s, 0) >= d.width && IOSurfaceGetHeightOfPlane(s, 0) >= d.height &&
           IOSurfaceGetBytesPerRowOfPlane(s, 1) >= 2 * d.width && IOSurfaceGetHeightOfPlane(s, 1) >= d.height &&
           IOSurfaceGetBytesPerElementOfPlane(s, 0) == 1 && IOSurfaceGetBytesPerElementOfPlane(s, 1) == 2;
}

// A YCbCr texture's plan (raw_ycbcr.h): Metal reads it converted by the surface's range and matrix, that through the sRGB curve,
// or its codes, which the raw path draws into an image of its own with the pack's conversion state (pack/ycbcr.metal, exact
// against Metal's reads: test/ycbcroracle.mm) before the work that samples it. Null when taken, else the rule that refused it.
const char *ycbcrPlan(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::YcbcrPlan &out)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    raw::YcbcrFacts f;
    f.pixelFormat = uint32_t(d.pixelFormat);
    f.fourcc = IOSurfaceGetPixelFormat(s);
    f.plane = uint32_t(plane);
    f.planes = uint32_t(IOSurfaceGetPlaneCount(s));
    f.width = uint32_t(d.width);
    f.height = uint32_t(d.height);
    f.surfaceWidth = uint32_t(IOSurfaceGetWidth(s));
    f.surfaceHeight = uint32_t(IOSurfaceGetHeight(s));
    const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
    for (size_t k = 0; k < 2 && k < f.planes; k++) {
        f.offset[k] = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, k)) - base;
        f.pitch[k] = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, k));
    }
    if (!f.planes)  // a one-plane format's surface of no planes ('2vuy', 'yuvs')
        f.pitch[0] = uint32_t(IOSurfaceGetBytesPerRow(s));
    f.allocBytes = IOSurfaceGetAllocSize(s);
    f.mappedBytes = (f.allocBytes + vm_page_size - 1) & ~uint64_t(vm_page_size - 1);
    f.simple = d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned;
    f.target = raw::ycbcrUsageRenders(d.usage);
    char matrix[32] = "";
    if (CFTypeRef value = IOSurfaceCopyValue(s, CFSTR("IOSurfaceYCbCrMatrix"))) {
        if (CFGetTypeID(value) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)value, matrix, sizeof matrix, kCFStringEncodingUTF8);
        CFRelease(value);
        f.matrix = matrix;
    }
    const char *why = raw::planYcbcr(f, out);
    if (why && d.usage == MTLTextureUsageUnknown) {  // sampled, and refused only once a pass renders into it
        f.target = false;
        why = raw::planYcbcr(f, out);
    }
    return why;
}

// Whether the GPU may write a texture of this descriptor (Unknown: any use)
static bool writesTexture(MTLTextureDescriptor *d)
{
    return d.usage == MTLTextureUsageUnknown || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
}

// A plane's rows as Metal's GPU reads them: its bytesPerRow rounded down to 16 (a single row's whatever covers it)
static size_t gpuRows(size_t pitch, MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    const size_t packed = d.width * (f ? f->bpp : 0);
    return d.height == 1 ? std::max(pitch & ~size_t(15), (packed + 15) & ~size_t(15)) : pitch & ~size_t(15);
}

// A plane whose offset or rows are off 16, made at Metal's GPU layout (both rounded down to 16): its CPU's rows are the
// plane's own
static void keepCpuRows(RMTexture *t, uint8_t *rows, size_t pitch)
{
    if (t->_rawOk && !t->_raw.blockLinear && ((uintptr_t(rows) | pitch) & 15)) {
        t->_cpuRows = rows;
        t->_cpuPitch = pitch;
    }
}

// Whether a texture of this descriptor is made to be rendered into or stored to, as the kernel's layout decision takes it
// (TegraGM20BLayout.h): a surface whose first texture says Unknown is left pitch, as the CPU may produce it
static bool producesTexture(MTLTextureDescriptor *d)
{
    return d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite);
}

#if SWITCH_FRAME_TIMELINE
static void noteCompositorSurface(MTLTextureDescriptor *d, IOSurfaceRef s, const void *frame)
{
    constexpr size_t kHalfScreen = 1280 * 720 / 2;
    if (!producesTexture(d) || IOSurfaceGetWidth(s) * IOSurfaceGetHeight(s) < kHalfScreen || !gRM || !gRM->raw ||
        gRM->raw->role() != raw::Device::Role::RenderServer || !raw::noteAllowed())
        return;
    raw::postSurfaceNote(s, frame);
}

static void noteCompositorTarget(MTLTextureDescriptor *d, const void *frame)
{
    constexpr NSUInteger kPanel = 1280 * 720;
    if (!(d.usage & MTLTextureUsageRenderTarget) || d.width * d.height <= kPanel || !gRM || !gRM->raw ||
        gRM->raw->role() != raw::Device::Role::RenderServer || !raw::noteAllowed())
        return;
    raw::postTargetNote({uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), uint32_t(d.usage),
                         uint32_t(d.storageMode), uint32_t(d.sampleCount), uint32_t(d.textureType),
                         switch_frame::callers(frame)});
}
#endif

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
    return swizzled([self rmSurfaceTexture:d iosurface:s plane:plane], d.swizzle);
}

- (id<MTLTexture>)rmSurfaceTexture:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
    SWITCH_API(ApiObject);
    if (!s)
        return nil;
    if (raw::ycbcrMetalRefuses(uint32_t(d.pixelFormat), uint32_t(d.width), uint32_t(d.height), d.usage)) {
        SWITCH_REFUSE_DETAIL(
            "raw", "ycbcr.odd", d.pixelFormat,
            "no %lux%lu texture of format %lu and usage 0x%lx: nil, as Metal answers for a subsampled "
            "YCbCr texture of an odd size it may write (MTLDevice.h newTextureWithDescriptor:iosurface:"
            "plane:, nullable)",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage);
        return nil;
    }
#if SWITCH_FRAME_TIMELINE
    gSurfaceTextures.fetch_add(1, std::memory_order_relaxed);
    noteCompositorSurface(d, s, __builtin_frame_address(0));
#endif
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = RMTextureSurface;
    t->_surface = (IOSurfaceRef)CFRetain(s);
    t->_surfaceWrites = holdSurfaceWrites(s);
    t->_plane = plane;
    raw::YcbcrPlan ycbcr;
    raw::Rotation turned;
    if (rotatedShape(d, s, plane, turned)) {
        t->_rawOk = createRotated(t, d, s, turned);
    } else if (planar444(d, s, plane)) {  // RGB8_444_2P over a 4:4:4 two-plane surface: an RGBA8 image of its codes
        void *base = IOSurfaceGetBaseAddress(s);
        const uintptr_t at = uintptr_t(base);
        t->_rawOk = gRM->raw->createPlanarTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)), uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at,
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1)), uint32_t(d.width), uint32_t(d.height), t->_raw);
        // rendered into: without its stand-in a pass into it goes to m2v (enc.planarrt), its samples stay raw
        if (t->_rawOk && (d.usage & MTLTextureUsageRenderTarget) &&
            !gRM->raw->createPlanarTarget(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, t->_raw))
            SWITCH_REFUSE_DETAIL("raw", "planar.target", 0, "no stand-in for a %lux%lu RGB8_444_2P render target",
                                 (unsigned long)d.width, (unsigned long)d.height);
    } else if (xrPlanesShape(d, s, plane)) {
        t->_rawOk = makeXrPlanes(t, s);
    } else if ((raw::isYcbcrFormat(uint32_t(d.pixelFormat)) || raw::packedPlaneChannels(uint32_t(d.pixelFormat))) &&
               !ycbcrPlan(d, s, plane, ycbcr)) {
        // a YCbCr texture or a packed 10-bit plane: an image the pack's conversion state draws
        t->_rawOk =
            gRM->raw->createYcbcrTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s,
                                         ycbcr, uint32_t(d.width), uint32_t(d.height), t->_raw);
        if (t->_rawOk && ycbcr.target)
            makeYcbcrTarget(t->_raw, uint32_t(d.pixelFormat), ycbcr.store);
    } else if (raw::SurfaceImage image; (!rawShape(d) || !IOSurfaceGetWidth(s)) && laidOutSurface(d, s, plane, image)) {
        // a shared texture in the device's layout of its descriptor (RawMetalSharedTextures.mm): a surface of no rows
        t->_rawOk = gRM->raw->createSurfaceImage(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s),
                                                 IOSurfaceGetAllocSize(s), s, image, uint32_t(d.width),
                                                 uint32_t(d.height), uint32_t(d.pixelFormat), t->_raw);
    } else if (rawShape(d) && !plane && IOSurfaceGetPlaneCount(s) <= 1) {
        // a surface without planes, or one described by a single plane (CoreVideo's chunky surfaces): its plane's bytes
        void *base = IOSurfaceGetBaseAddress(s);  // valid while the surface lives (the import retains it)
        const bool one = IOSurfaceGetPlaneCount(s) == 1;
        const uint64_t offset = one ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - uintptr_t(base) : 0;
        const size_t pitch = one ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
        const size_t gpuPitch = one ? gpuRows(pitch, d) : pitch;
        t->_rawOk = gRM->raw->createSurfaceTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset & ~15ull, uint32_t(d.width),
            uint32_t(d.height), uint32_t(gpuPitch), uint32_t(d.pixelFormat), t->_raw, producesTexture(d));
        keepCpuRows(t, static_cast<uint8_t *>(base) + offset, pitch);
        if (t->_rawOk)
            keepExtendedRange(d, t->_raw, false);
    } else if (rawShape(d) && plane < IOSurfaceGetPlaneCount(s)) {
        // a plane as a texture of its own (a video player's R8 luma and RG8 chroma): the plane's bytes, pitch, as CoreVideo
        // lays its planes out for every other reader (never a block-linear layout the kernel would decide for the surface)
        void *base = IOSurfaceGetBaseAddress(s);
        const uint64_t offset = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, plane)) - uintptr_t(base);
        const size_t pitch = IOSurfaceGetBytesPerRowOfPlane(s, plane);
        t->_rawOk = gRM->raw->createSurfaceTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset & ~15ull, uint32_t(d.width),
            uint32_t(d.height), uint32_t(gpuRows(pitch, d)), uint32_t(d.pixelFormat), t->_raw, false);
        keepCpuRows(t, static_cast<uint8_t *>(base) + offset, pitch);
        const OSType fourcc = IOSurfaceGetPixelFormat(s);
        if (t->_rawOk && (fourcc == 0x34323076 || fourcc == 0x34323066))
            t->_raw.nativePlane = uint32_t(plane);
    }
    if (t->_rawOk && t->_raw.converted())
        surfaceConverted(t->_surfaceWrites, true);
    if (t->_rawOk && writesTexture(d))
        declareGpuWrites(t);
    if (!t->_rawOk) {  // a surface the raw path does not take: m2v's texture over it, made now (it may refuse: nil, as today)
        // which term (createSurfaceTexture's, recomputed here: a creation, not the hot path)
        const size_t planes = IOSurfaceGetPlaneCount(s),
                     pitch = planes ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : IOSurfaceGetBytesPerRow(s);
        const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
        const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0);
        const OSType fourcc = IOSurfaceGetPixelFormat(s);
        NSString *extra = [NSString
            stringWithFormat:@"surface %u %ux%u '%c%c%c%c' bytesPerRow %lu planes %lu plane %lu alloc %lu "
                             @"base&0xfff 0x%lx",
                             IOSurfaceGetID(s), unsigned(IOSurfaceGetWidth(s)), unsigned(IOSurfaceGetHeight(s)),
                             char(fourcc >> 24), char(fourcc >> 16), char(fourcc >> 8), char(fourcc),
                             (unsigned long)pitch, (unsigned long)planes, (unsigned long)plane,
                             (unsigned long)IOSurfaceGetAllocSize(s), (unsigned long)(base & 0xfff)];
        if (planar444(d, s, plane))
            refuseTexture(t, PredTexPlanar, d.width, d.height, @"surface", extra);
        else if (uint32_t term = rawShapeTerm(d))
            refuseTexture(t, PredTexShape, term, d.pixelFormat, @"surface", extra);
        else if (plane >= std::max<size_t>(planes, 1)) {
            os_log_error(rawLog(), "raw: %{public}s: no texture over IOSurface %u: it has no plane %lu: %{public}@",
                         getprogname(), IOSurfaceGetID(s), (unsigned long)plane, extra);
            return nil;
        } else if (planes ? (pitch & ~size_t(15)) < packed && d.height > 1 : (pitch & 15) || pitch < packed) {
            // Metal asserts on a surface without planes whose rows are not 16 bytes apart; a plane's rows rounded down to 16
            // overlap on its GPU. m2v refused both (linear_plan.rs).
            os_log_error(
                rawLog(),
                "raw: %{public}s: no texture over IOSurface %u: rows off 16 that Metal does not take: %{public}@",
                getprogname(), IOSurfaceGetID(s), extra);
            return nil;
        } else if (uint64_t(pitch) * d.height > IOSurfaceGetAllocSize(s)) {
            os_log_error(
                rawLog(),
                "raw: %{public}s: no texture over IOSurface %u: %lu rows of %zu bytes past its %zu: %{public}@",
                getprogname(), IOSurfaceGetID(s), (unsigned long)d.height, pitch, IOSurfaceGetAllocSize(s), extra);
            return nil;
        } else
            refuseTexture(t, PredTexSurfImport, 1, 0, @"surface", extra);
        if (![t rmTwin])
            return nil;
    }
#if SWITCH_FRAME_TIMELINE
    if (t->_raw.planar || t->_raw.ycbcr)
        traceSurfaceImage(t, "imported", IOSurfaceGetSeed(s));
#endif
    return (id<MTLTexture>)t;
}

@end
