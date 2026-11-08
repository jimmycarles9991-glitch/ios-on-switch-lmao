// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

@implementation RMDevice

- (void)rmCommit:(RMCommandBuffer *)cb
{
    commitBuffer(cb);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's attribution: this process's command buffers on each path since the device opened, never reset (the rates
// line's windows stay its own). Keys: raw, m2v, split, split_m2v, aborted, reopens (channels the kernel killed and this
// process opened again), m2v.<reason>, pred.<predicate> (nonzero only).
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
    for (int r = 1; r < WhyCount; r++)
        if (const uint64_t n = s.reasons[r].load())
            d[[NSString stringWithFormat:@"m2v.%s", kReasonNames[r]]] = @(n);
    for (int p = 1; p < PredCount; p++)
        if (const uint64_t n = gPredCbs[p].load(std::memory_order_relaxed))
            d[[NSString stringWithFormat:@"pred.%s", kPredNames[p]]] = @(n);
    return d;
}

// The m2v device this one wraps (the one TEGRA_RAW_METAL=0 gives the process): hw_test_app draws each scene on both
- (id<MTLDevice>)switchM2VDevice
{
    return gRM->fallback;
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
//  - the image memory it samples in place (CA::OGL::MetalContext::copy_image_to_texture: offset and rows a multiple of this,
//    else it copies them into a texture of its own): the texture header's 32;
//  - the rows of its CPU readbacks (CAMetalTextureGetBytesPerRow).
- (NSUInteger)minConstantBufferAlignmentBytes
{
    return 4;
}

- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes
{
    return 32;
}

- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format
{
    const NSUInteger a = [gRM->fallback minimumLinearTextureAlignmentForPixelFormat:format];
    return a ? std::max<NSUInteger>(a, 32) : a;
}

// (every other selector: the m2v device's own method, which the device class inherits)
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
    if (!gRM->raw->createBuffer(n ? n : 16, b->_raw,
                                wc)) {  // the pool could not grow: m2v's buffer (its users go to m2v)
        SWITCH_REFUSE("raw", "buffer.pool", "no raw memory for a %lu-byte buffer: m2v's", (unsigned long)n);
        if (!predLogged(PredCallBufForeign))
            logPred(PredCallBufForeign, 0, n, @"(made: the pool had no room)");
        RMTwinObject *t = [RMTwinObject new];
        t->_device = self;
        t->_twin = [gRM->fallback newBufferWithLength:n options:o];
        return t->_twin ? (id<MTLBuffer>)t : nil;
    }
    return (id<MTLBuffer>)b;
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)p length:(NSUInteger)n options:(MTLResourceOptions)o
{
    SWITCH_API(ApiObject);
    id<MTLBuffer> b = [self newBufferWithLength:n options:o];
    if (!b || !p || !n)
        return b;
    if (![(id)b isKindOfClass:[RMBuffer class]]) {  // the pool had no room: m2v's buffer, made with the bytes
        RMTwinObject *t = (RMTwinObject *)b;
        t->_twin = [gRM->fallback newBufferWithBytes:p length:n options:o];
        return t->_twin ? b : nil;
    }
    memcpy(((RMBuffer *)b)->_raw.cpu, p, n);  // the draws that read it clean the lines they read
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
    if (!gRM->raw->createBufferNoCopy(p, n, b->_raw)) {  // (not page-aligned, or refused): m2v's, as today
        if (!predLogged(PredCallBufForeign))
            logPred(PredCallBufForeign, uintptr_t(p) & 0xfff, n, @"(made: a no-copy buffer the raw path refused)");
        RMTwinObject *t = [RMTwinObject new];
        t->_device = self;
        t->_twin = [gRM->fallback newBufferWithBytesNoCopy:p length:n options:o deallocator:deallocator];
        return t->_twin ? (id<MTLBuffer>)t : nil;
    }
    b->_deallocator = [deallocator copy];
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

// A private texture the CPU never touches is block-linear (TegraGM20BLayout.h), as NVK lays out an optimal image and m2v's
// view of it is (texture_tiled_in_place).
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    return [self rmNewTexture:d tiled:d.storageMode == MTLStorageModePrivate];
}

- (id<MTLTexture>)rmNewTexture:(MTLTextureDescriptor *)d tiled:(bool)tiled
{
    if (chainShape(d)) {  // mipmapped: level 0 of a block-linear chain of its levels (raw/fx)
        RMTexture *t = [RMTexture new];
        t->_device = self;
        t->_descriptor = [d copy];
        t->_kind = RMTexturePool;
        t->_rawOk = gRM->raw->createMipTexture(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat),
                                               uint32_t(d.mipmapLevelCount), t->_raw);
        if (t->_rawOk) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                os_log(rawLog(), "raw: mip chain on (%u levels of %ux%u, %llu bytes)", unsigned(d.mipmapLevelCount),
                       unsigned(d.width), unsigned(d.height), (unsigned long long)t->_raw.chain->layout.bytes);
            return (id<MTLTexture>)t;
        }
    }
    if (!rawShape(d) && !zetaDescriptor(d)) {
        RMTexture *w = (RMTexture *)[self wrapTwin:[gRM->fallback newTextureWithDescriptor:d] descriptor:d];
        if (w)
            refuseTexture(w, PredTexShape, rawShapeTerm(d), d.pixelFormat, @"twin-only", nil);
        return (id<MTLTexture>)w;
    }
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    if (d.storageMode == MTLStorageModeMemoryless) {
        t->_kind = RMTextureMemoryless;
        t->_rawOk = gRM->raw->createMemoryless(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), t->_raw);
    } else {
        t->_kind = RMTexturePool;
        // a shared texture (the CPU writes it: replaceRegion) in write-combined memory: its rows need no clean after
        t->_rawOk = gRM->raw->createTexture(uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat), t->_raw,
                                            d.storageMode == MTLStorageModeShared, tiled);
        if (t->_rawOk)
            keepExtendedRange(d, t->_raw, false);
    }
    if (!t->_rawOk) {
        RMTexture *w = (RMTexture *)[self wrapTwin:[gRM->fallback newTextureWithDescriptor:d] descriptor:d];
        if (w)
            refuseTexture(w, PredTexPoolAlloc, d.width, d.height, @"twin-only (pool)", nil);
        return (id<MTLTexture>)w;
    }
    return (id<MTLTexture>)t;
}

// RGB8_444_2P (542) over plane 0 of a two-plane 4:4:4 surface of one byte a luma texel and two a chroma pair (CoreVideo's
// '_RB4', the widget and poster snapshots Core Animation samples): Metal reads its codes, red Cr, green Y, blue Cb, alpha 1
// (m2v ycbcr.rs), which the raw path holds in an RGBA8 image it writes from the planes with the copy engine before the work that
// samples it (raw::BlitEncoder::convertPlanar). A 2D texture of the surface's size, one level, no view, no rotation, sampled
// or rendered (WidgetRenderer's snapshots: through its stand-in, raw::Texture::planarStandIn).
static bool planar444(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    constexpr MTLPixelFormat kRGB8_444_2P = MTLPixelFormat(542);
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.pixelFormat == kRGB8_444_2P && plane == 0 && IOSurfaceGetPlaneCount(s) == 2 &&
           d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned &&
           !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | MTLTextureUsageRenderTarget)) &&
           IOSurfaceGetWidthOfPlane(s, 0) == d.width && IOSurfaceGetHeightOfPlane(s, 0) == d.height &&
           IOSurfaceGetWidthOfPlane(s, 1) == d.width && IOSurfaceGetHeightOfPlane(s, 1) == d.height &&
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
    f.allocBytes = IOSurfaceGetAllocSize(s);
    f.simple = d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && !turned &&
               !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView));
    char matrix[32] = "";
    if (CFTypeRef value = IOSurfaceCopyValue(s, CFSTR("IOSurfaceYCbCrMatrix"))) {
        if (CFGetTypeID(value) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)value, matrix, sizeof matrix, kCFStringEncodingUTF8);
        CFRelease(value);
        f.matrix = matrix;
    }
    return raw::planYcbcr(f, out);
}

// Whether the GPU may write a texture of this descriptor (Unknown: any use)
static bool writesTexture(MTLTextureDescriptor *d)
{
    return d.usage == MTLTextureUsageUnknown || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
}

// Whether a texture of this descriptor is made to be rendered into or stored to, as the kernel's layout decision takes it
// (TegraGM20BLayout.h): a surface whose first texture says Unknown is left pitch, as the CPU may produce it
static bool producesTexture(MTLTextureDescriptor *d)
{
    return d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite);
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
    SWITCH_API(ApiObject);
    if (!s)
        return nil;
#if SWITCH_FRAME_TIMELINE
    gSurfaceTextures.fetch_add(1, std::memory_order_relaxed);
#endif
    RMTexture *t = [RMTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = RMTextureSurface;
    t->_surface = (IOSurfaceRef)CFRetain(s);
    t->_plane = plane;
    raw::YcbcrPlan ycbcr;
    if (planar444(d, s, plane)) {  // RGB8_444_2P over a 4:4:4 two-plane surface: an RGBA8 image of its codes
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
    } else if (raw::isYcbcrFormat(uint32_t(d.pixelFormat)) && !ycbcrPlan(d, s, plane, ycbcr)) {
        // a YCbCr texture: an image the pack's conversion state draws
        t->_rawOk =
            gRM->raw->createYcbcrTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s,
                                         ycbcr, uint32_t(d.width), uint32_t(d.height), t->_raw);
    } else if (rawShape(d) && !plane && IOSurfaceGetPlaneCount(s) <= 1) {
        // a surface without planes, or one described by a single plane (CoreVideo's chunky surfaces): its plane's bytes
        void *base = IOSurfaceGetBaseAddress(s);  // valid while the surface lives (the import retains it)
        const bool one = IOSurfaceGetPlaneCount(s) == 1;
        const uint64_t offset = one ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - uintptr_t(base) : 0;
        const size_t pitch = one ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
        t->_rawOk = gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset,
                                                   uint32_t(d.width), uint32_t(d.height), uint32_t(pitch),
                                                   uint32_t(d.pixelFormat), t->_raw, producesTexture(d));
        if (t->_rawOk)
            keepExtendedRange(d, t->_raw, false);
    } else if (rawShape(d) && plane < IOSurfaceGetPlaneCount(s)) {
        // a plane as a texture of its own (a video player's R8 luma and RG8 chroma): the plane's bytes, pitch, as CoreVideo
        // lays its planes out for every other reader (never a block-linear layout the kernel would decide for the surface)
        void *base = IOSurfaceGetBaseAddress(s);
        t->_rawOk = gRM->raw->createSurfaceTexture(
            IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s,
            uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, plane)) - uintptr_t(base), uint32_t(d.width),
            uint32_t(d.height), uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, plane)), uint32_t(d.pixelFormat), t->_raw,
            false);
    }
    if (t->_rawOk && t->_raw.converted())
        surfaceConverted(s, true);
    if (t->_rawOk && writesTexture(d))
        declareGpuWrites(t);
    if (!t->_rawOk) {  // a surface the raw path does not take: m2v's texture over it, made now (it may refuse: nil, as today)
        // which term (createSurfaceTexture's, recomputed here: a creation, not the hot path)
        const size_t planes = IOSurfaceGetPlaneCount(s),
                     pitch = planes == 1 ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
        const uintptr_t base = uintptr_t(IOSurfaceGetBaseAddress(s));
        const uintptr_t offset = planes == 1 ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - base : 0;
        const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
        const uint64_t packed = uint64_t(d.width) * (f ? f->bpp : 0);
        const OSType fourcc = IOSurfaceGetPixelFormat(s);
        NSString *extra =
            [NSString stringWithFormat:@"surface %ux%u '%c%c%c%c' bytesPerRow %lu planes %lu plane %lu alloc %lu "
                                       @"base&0xfff 0x%lx",
                                       unsigned(IOSurfaceGetWidth(s)), unsigned(IOSurfaceGetHeight(s)),
                                       char(fourcc >> 24), char(fourcc >> 16), char(fourcc >> 8), char(fourcc),
                                       (unsigned long)pitch, (unsigned long)planes, (unsigned long)plane,
                                       (unsigned long)IOSurfaceGetAllocSize(s), (unsigned long)(base & 0xfff)];
        if (planar444(d, s, plane))
            refuseTexture(t, PredTexPlanar, d.width, d.height, @"surface", extra);
        else if (uint32_t term = rawShapeTerm(d))
            refuseTexture(t, PredTexShape, term, d.pixelFormat, @"surface", extra);
        else if (plane || planes > 1)
            refuseTexture(t, PredTexSurfPlane, plane, planes, @"surface", extra);
        else if ((pitch & 15) || (offset & 15) || pitch < packed)  // (its plane's offset: as its pitch)
            refuseTexture(t, PredTexSurfPitch, pitch, offset & 15 ? offset : packed, @"surface", extra);
        else if (uint64_t(pitch) * d.height > IOSurfaceGetAllocSize(s))
            refuseTexture(t, PredTexSurfBounds, uint64_t(pitch) * d.height, IOSurfaceGetAllocSize(s), @"surface",
                          extra);
        else if (base & (size_t(getpagesize()) - 1))
            refuseTexture(t, PredTexSurfBase, base & 0xfff, 0, @"surface", extra);
        else
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
