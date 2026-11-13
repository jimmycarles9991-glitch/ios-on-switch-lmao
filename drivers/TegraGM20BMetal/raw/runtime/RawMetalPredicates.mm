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
#if SWITCH_RAW_DIAGNOSTICS
std::atomic<uint64_t> gPredCbs[PredCount], gPredOnly[PredCount];
#endif
std::atomic<uint64_t> gPredLogged{0};

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

// The type encoding of a selector some MTL* protocol declares (the plugin's answerUnimplemented uses the same lookup), walked
// once per selector: objc_copyProtocolList copies the shared cache's protocols (the frontend's forwardSignature, r14-maps)
const char *metalTypes(SEL selector)
{
    static raw::Mutex lock;
    static auto *known = new std::unordered_map<SEL, const char *>;
    {
        std::lock_guard<raw::Mutex> l(lock);
        if (const auto it = known->find(selector); it != known->end())
            return it->second;
    }
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
    std::lock_guard<raw::Mutex> l(lock);
    known->emplace(selector, types);
    return types;
}

bool twinAnswers(Class twinClass, SEL selector)
{
    return twinClass && [twinClass instancesRespondToSelector:selector];
}

static void returnZero(NSInvocation *invocation)
{
    if (const NSUInteger length = invocation.methodSignature.methodReturnLength) {
        uint64_t small[8] = {};
        void *zero = length <= sizeof small ? small : calloc(1, length);
        [invocation setReturnValue:zero];
        if (zero != small)
            free(zero);
    }
}

void answerZero(id object, NSInvocation *invocation)
{
    Class cls = object_getClass(object);
    SWITCH_REFUSE_DETAIL("raw", "selector.zero", uintptr_t(invocation.selector) * 31 + uintptr_t(cls),
                         "-[%s %s] answered zero, as m2v's object answers it", class_getName(cls),
                         sel_getName(invocation.selector));
#if SWITCH_FRAME_TIMELINE
    if (gRM)
        RAW_DIAGNOSTIC(gRM->stats.zeroAnswers.fetch_add(1, std::memory_order_relaxed));
#endif
    returnZero(invocation);
}

void answerMissing(id object, NSInvocation *invocation)
{
    Class cls = object_getClass(object);
    const uint64_t key = switch_refusal_key_with(SWITCH_REFUSAL_KEY("raw", "selector.missing"),
                                                 uintptr_t(invocation.selector) * 31 + uintptr_t(cls));
    if (switch_refusal_first(key)) {
        char text[256];
        snprintf(text, sizeof text, "-[%s %s] has no raw method (m2v's object has one): answered zero",
                 class_getName(cls), sel_getName(invocation.selector));
        switch_refusal_log("raw", "selector.missing", text);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        os_log_fault(rawLog(), "raw: %{public}s", text);
#endif
    }
#if SWITCH_FRAME_TIMELINE
    if (gRM)
        RAW_DIAGNOSTIC(gRM->stats.twinForwards.fetch_add(1, std::memory_order_relaxed));
#endif
    returnZero(invocation);
}

// A format the raw path has a header for. RGB10A8_2P_XR10 only private or memoryless: its texels are m2v's own layout, not the two
// planes of Metal's bytes a surface, a buffer or the CPU holds (raw_xr.h); one over a two-plane surface's planes is taken before
// this (xrPlanesShape)
static bool keptFormat(MTLTextureDescriptor *d)
{
    if (raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::Planes && d.storageMode != MTLStorageModePrivate &&
        d.storageMode != MTLStorageModeMemoryless)
        return false;
    // a block-compressed texture is private: written and read by the copy engine in whole blocks, never by the CPU
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    return f && (!raw::blockCompressed(f) || d.storageMode == MTLStorageModePrivate);
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

// A 1D texture the raw path keeps (raw::Device::createLine): one level, of a format it has a header for, not extended range.
// Memory of its own only: a 1D header has no pitch layout, which a buffer's or a surface's rows are.
bool lineShape(MTLTextureDescriptor *d)
{
    return d.textureType == MTLTextureType1D && d.mipmapLevelCount <= 1 && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.height <= 1 && d.depth <= 1 && d.storageMode != MTLStorageModeMemoryless &&
           raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None && raw::formatInfo(uint32_t(d.pixelFormat)) &&
           !raw::blockCompressed(raw::formatInfo(uint32_t(d.pixelFormat)));
}

// m2v makes a 3D texture it may render into 2D-array compatible (image_plan.rs render_3d_flags), which NIL lays out one plane a
// block; Unknown usage is every usage (texture_descriptor.rs effective_usage)
bool volumeRendered(MTLTextureDescriptor *d)
{
    return d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageRenderTarget);
}

// A 3D texture the raw path keeps (raw::Device::createVolume): of a format it has a header for, not extended range, memory of
// its own, mipmapped of a format it renders (as a 2D chain is). One m2v would render into must be of a format the raw path
// renders too: m2v's twin is laid out for rendering exactly where NVK renders the format.
bool volumeShape(MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    return d.textureType == MTLTextureType3D && d.sampleCount <= 1 && d.arrayLength <= 1 &&
           d.storageMode != MTLStorageModeMemoryless && raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None &&
           f && !raw::blockCompressed(f) && (f->rt || (!volumeRendered(d) && d.mipmapLevelCount <= 1));
}

// A 2D array, cube or cube array the raw path keeps (raw::Device::createArray): one sample, of a format it has a header for,
// not extended range, memory of its own, mipmapped of a format it renders (as a 2D chain is); its header's type
// (fx::kTexTwoDArray, kTexCube, kTexCubeArray), 0: none
uint32_t arrayType(MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    const bool kept = (d.mipmapLevelCount <= 1 || (f && f->rt)) && d.sampleCount <= 1 && d.depth <= 1 &&
                      d.storageMode != MTLStorageModeMemoryless &&
                      raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::None && f && !raw::blockCompressed(f);
    if (!kept)
        return 0;
    switch (d.textureType) {
    case MTLTextureType1DArray:
        return d.height <= 1 ? raw::fx::kTexOneDArray : 0;
    case MTLTextureType2DArray:
        return raw::fx::kTexTwoDArray;
    case MTLTextureTypeCube:
        return d.width == d.height && d.arrayLength <= 1 ? raw::fx::kTexCube : 0;
    case MTLTextureTypeCubeArray:
        return d.width == d.height ? raw::fx::kTexCubeArray : 0;
    default:
        return 0;
    }
}

// A depth/stencil texture of a format the raw path renders (raw_zeta.h kZetaFormatInfo), one level, no rotation: memoryless, 2D
// of one sample or 2D multisampled of 2 or 4; or stored (private), 2D of one sample, a render target that may also be sampled:
// in the raw path's own zeta memory (makeStoredZeta), a 2D array or cube rendered there a slice at a time or layered, moved
// into NVK's image only if a replay needs m2v's twin
bool zetaDescriptor(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    const bool samples = d.textureType == MTLTextureType2D ? d.sampleCount <= 1
                                                           : d.textureType == MTLTextureType2DMultisample &&
                                                                 (d.sampleCount == 2 || d.sampleCount == 4);
    const bool layers = d.textureType == MTLTextureType2DArray || d.textureType == MTLTextureTypeCube;
    const bool stored = d.storageMode == MTLStorageModePrivate && (d.textureType == MTLTextureType2D || layers) &&
                        (d.usage & ~MTLTextureUsageShaderRead) == MTLTextureUsageRenderTarget;
    return (samples || (layers && stored)) && d.mipmapLevelCount <= 1 && (d.arrayLength <= 1 || layers) &&
           d.depth <= 1 && !turned && raw::zetaFormatIndex(uint32_t(d.pixelFormat)) < raw::kZetaFormats &&
           (d.storageMode == MTLStorageModeMemoryless || stored);
}

// A memoryless multisampled color texture, 2D, one level, 2 or 4 samples (the counts the device answers yes for), no rotation, of a
// format the raw path has a header for (raw::Device::createMemoryless with its samples)
bool multisampleShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return d.textureType == MTLTextureType2DMultisample && d.mipmapLevelCount <= 1 && d.arrayLength <= 1 &&
           d.depth <= 1 && (d.sampleCount == 2 || d.sampleCount == 4) && !turned &&
           d.storageMode == MTLStorageModeMemoryless && keptFormat(d);
}

// A memoryless 2D array or cube, one sample and one level, no rotation, of a depth/stencil format the raw path renders or a
// color format it renders as it is (raw::Device::createMemoryless with its layers): its layers, 0: none
uint32_t memorylessLayers(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    const uint32_t format = uint32_t(d.pixelFormat);
    const raw::FormatInfo *f = raw::formatInfo(format);
    const bool target = raw::zetaFormatIndex(format) < raw::kZetaFormats ||
                        (f && f->rt && !raw::blockCompressed(f) && raw::xrForm(format) == raw::XrForm::None);
    const bool layered = d.textureType == MTLTextureType2DArray || d.textureType == MTLTextureTypeCube;
    if (d.storageMode != MTLStorageModeMemoryless || !layered || d.sampleCount > 1 || d.mipmapLevelCount > 1 ||
        d.depth > 1 || turned || !target)
        return 0;
    return raw::textureLayers(uint32_t(d.textureType), uint32_t(std::min<NSUInteger>(d.arrayLength, 2048)));
}

// A block-compressed 2D texture or 2D array the raw path keeps in block-linear memory of its own (raw::Device::
// createBlockTexture): any levels and layers, private or shared (the CPU's copies move whole blocks of NIL's image), one
// sample, no rotation; a private one of one level and one layer is rawShape's
bool blockShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    return f && raw::blockCompressed(f) &&
           (d.textureType == MTLTextureType2D || d.textureType == MTLTextureType2DArray) && d.sampleCount <= 1 &&
           d.depth <= 1 && !turned &&
           (d.storageMode == MTLStorageModePrivate || d.storageMode == MTLStorageModeShared) && !rawShape(d);
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
    static raw::Mutex lock;
    static NSMutableSet<NSString *> *seen;
    MTLTextureDescriptor *d = t->_descriptor;
    NSString *shape = [NSString
        stringWithFormat:@"%s %llu %@ %lux%lu f%lu t%lu l%lu s%lu u%lx m%lu%@", kPredNames[p], (unsigned long long)a,
                         kind, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat,
                         (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
                         (unsigned long)d.usage, (unsigned long)d.storageMode, surfaceShape(t)];
    {
        std::lock_guard<raw::Mutex> l(lock);
        if (!seen)
            seen = [NSMutableSet set];
        if (seen.count >= 32 || [seen containsObject:shape])
            return;
        [seen addObject:shape];
    }
    os_log(rawLog(), "raw: texture on m2v: %{public}@", shape);
}
