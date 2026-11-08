// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// Each predicate's refusal key: domain "raw", the predicate's name as the reason (kits/SwitchRefusal)
static constexpr std::array<uint64_t, PredCount> predKeys()
{
    std::array<uint64_t, PredCount> keys{};
    for (int p = 0; p < PredCount; p++)
        keys[p] = switch_refusal_key("raw", kPredNames[p]);
    return keys;
}

static constexpr std::array<uint64_t, PredCount> kPredKeys = predKeys();
std::atomic<uint64_t> gPredCbs[PredCount], gPredOnly[PredCount], gPredLogged{0};

// Each predicate's first command buffer this process sends to m2v, through kits/SwitchRefusal (domain raw-metal, the
// predicate's name as the reason)
void logPred(RMPred p, uint64_t a, uint64_t b, NSString *detail)
{
    const uint64_t bit = 1ull << p;
    if (gPredLogged.fetch_or(bit, std::memory_order_relaxed) & bit)
        return;
    switch_refusal_k(kPredKeys[p], "raw", kPredNames[p], "a=%llu (0x%llx) b=%llu (0x%llx)%s%s", (unsigned long long)a,
                     (unsigned long long)a, (unsigned long long)b, (unsigned long long)b, detail ? " " : "",
                     detail ? detail.UTF8String : "");
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: first %s: a=%llu b=%llu %s\n", kPredNames[p], (unsigned long long)a, (unsigned long long)b,
            detail ? detail.UTF8String : "");
#endif
}

// The type encoding of a selector some MTL* protocol declares (the plugin's answerUnimplemented uses the same lookup).
const char *metalTypes(SEL selector)
{
    unsigned count = 0;
    Protocol *__unsafe_unretained *protocols = objc_copyProtocolList(&count);
    const char *types = nullptr;
    for (unsigned i = 0; i < count && !types; ++i) {
        if (strncmp(protocol_getName(protocols[i]), "MTL", 3) != 0)
            continue;
        for (int required = 1; required >= 0 && !types; --required) {
            const objc_method_description d = protocol_getMethodDescription(protocols[i], selector, required, YES);
            if (d.name)
                types = d.types;
        }
    }
    free(protocols);
    return types;
}

// A format the raw path has a header for. RGB10A8_2P_XR10 only private or memoryless: its texels are m2v's own layout, not the two
// planes of Metal's bytes a surface, a buffer or the CPU holds (raw_xr.h)
static bool keptFormat(MTLTextureDescriptor *d)
{
    if (raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::Planes && d.storageMode != MTLStorageModePrivate &&
        d.storageMode != MTLStorageModeMemoryless)
        return false;
    return raw::formatInfo(uint32_t(d.pixelFormat)) != nullptr;
}

// A texture the raw path keeps: 2D, one level, one sample, a format it has a header for, not rotated (the descriptor's rotation SPI:
// Core Animation's window-server surfaces on a rotated display, CA::OGL::MetalContext::get_surface; m2v renders those rotated)
bool rawShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount <= 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && !turned && keptFormat(d);
}

// A mipmapped texture the raw path keeps as a block-linear chain of its levels (raw::Device::createMipTexture, raw/fx): 2D, one
// sample, one layer, no rotation, of a format the raw path renders and holds as it is (not extended range). Private, memoryless (r7r: Core Animation makes 776x24 mipmapped surfaces memoryless too, kept here as
// private ones, a superset) or shared: CA::OGL::MetalContext::update_surface copies IOSurface content it draws with a trilinear
// minification filter into a texture of the surface texture's descriptor (shared) with every level, then generates them (on
// the 18:13 boot of 11-06, 49 command buffers of a 1280x720 one went to m2v). A shared one's CPU access reads and writes the
// chain (cpuLevel).
bool chainShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount > 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && !turned &&
           (d.storageMode == MTLStorageModePrivate || d.storageMode == MTLStorageModeMemoryless ||
            d.storageMode == MTLStorageModeShared) &&
           f && f->rt && raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None;
}

// A memoryless Depth32Float_Stencil8 texture, 2D, one level, one sample, no rotation: the raw path's zeta (raw_zeta.h). A stored
// one stays m2v's: its twin could not hold the raw path's texels (NVK's image is its own memory), and a command buffer m2v
// replays would read another depth than the one the raw path left.
bool zetaDescriptor(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType == MTLTextureType2D && d.mipmapLevelCount <= 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && !turned && d.pixelFormat == MTLPixelFormat(raw::kZetaFormat) &&
           d.storageMode == MTLStorageModeMemoryless;
}

// Diagnostics: which rawShape term fails (1 type, 2 levels, 3 samples, 4 array/depth, 5 rotation, 6 format; 0 none)
uint32_t rawShapeTerm(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType != MTLTextureType2D    ? 1
           : d.mipmapLevelCount > 1             ? 2
           : d.sampleCount > 1                  ? 3
           : (d.arrayLength > 1 || d.depth > 1) ? 4
           : turned                             ? 5
           : !keptFormat(d)                     ? 6
                                                : 0;
}

static NSString *describe(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return [NSString
        stringWithFormat:@"%lux%lu format %lu type %lu levels %lu samples %lu array %lu rotation %lu usage 0x%lx "
                         @"storage %lu",
                         (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
                         (unsigned long)d.arrayLength, (unsigned long)turned, (unsigned long)d.usage,
                         (unsigned long)d.storageMode];
}

// A kept extended-range texture's facts (raw_xr.h). Metal keeps it compressed with allowGPUOptimizedContents, 16 texels or more a
// side and no ShaderWrite, PixelFormatView or ShaderAtomic usage, never over a buffer (m2v heap_tiled.rs metal_compresses); one
// draws may sample gets its stand-in.
void keepExtendedRange(MTLTextureDescriptor *d, raw::Texture &t, bool overBuffer)
{
    if (raw::xrForm(t.format) == raw::XrForm::None)
        return;
    const MTLTextureUsage plain =
        MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView | MTLTextureUsageShaderAtomic;
    const bool every = d.usage == MTLTextureUsageUnknown;
    t.compressed =
        !overBuffer && d.allowGPUOptimizedContents && std::min(d.width, d.height) >= 16 && !every && !(d.usage & plain);
    if (every || (d.usage & MTLTextureUsageShaderRead))
        gRM->raw->createStandIn(t);
}

static NSString *surfaceShape(RMTexture *t)
{
    IOSurfaceRef s = t->_surface;
    if (!s)
        return @"";
    const OSType fourcc = IOSurfaceGetPixelFormat(s);
    const size_t planes = IOSurfaceGetPlaneCount(s);
    NSMutableString *out =
        [NSMutableString stringWithFormat:@" surface '%c%c%c%c' %zux%zu planes %zu plane %lu", char(fourcc >> 24),
                                          char(fourcc >> 16), char(fourcc >> 8), char(fourcc), IOSurfaceGetWidth(s),
                                          IOSurfaceGetHeight(s), planes, (unsigned long)t->_plane];
    for (size_t k = 0; k < planes && k < 3; k++)
        [out appendFormat:@" p%zu %zux%zu/%zuB row %zu", k, IOSurfaceGetWidthOfPlane(s, k),
                          IOSurfaceGetHeightOfPlane(s, k), IOSurfaceGetBytesPerElementOfPlane(s, k),
                          IOSurfaceGetBytesPerRowOfPlane(s, k)];
    if (raw::isYcbcrFormat(uint32_t(t->_descriptor.pixelFormat))) {
        raw::YcbcrPlan plan;
        const char *why = ycbcrPlan(t->_descriptor, s, t->_plane, plan);
        [out appendFormat:@" YCbCr not taken: %s", why ?: "taken (refused later: the import or the pool)"];
    }
    return out;
}

void refuseTexture(RMTexture *t, RMPred p, uint64_t a, uint64_t b, NSString *kind, NSString *extra)
{
    t->_pred = p;
    if (!predLogged(p))
        logPred(p, a, b,
                [NSString
                    stringWithFormat:@"(%@ %@%@%@)", kind, describe(t->_descriptor), extra ? @" " : @"", extra ?: @""]);
    // and one line per new shape of texture the raw path leaves to m2v (the predicate's first line names only the first:
    // r7r's Tips replays were tex.shape of textures no line described), up to 32 per process: the predicate, its term, the
    // descriptor, and for a surface its pixel format, planes and what the 4:2:0 path did not take
    static std::mutex lock;
    static NSMutableSet<NSString *> *seen;
    MTLTextureDescriptor *d = t->_descriptor;
    NSString *shape = [NSString
        stringWithFormat:@"%s %llu %@ %lux%lu f%lu t%lu l%lu s%lu u%lx m%lu%@", kPredNames[p], (unsigned long long)a,
                         kind, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
                         (unsigned long)d.usage, (unsigned long)d.storageMode, surfaceShape(t)];
    {
        std::lock_guard<std::mutex> l(lock);
        if (!seen)
            seen = [NSMutableSet set];
        if (seen.count >= 32 || [seen containsObject:shape])
            return;
        [seen addObject:shape];
    }
    os_log(rawLog(), "raw: texture on m2v: %{public}@", shape);
}
