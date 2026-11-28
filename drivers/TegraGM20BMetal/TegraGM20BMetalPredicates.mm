// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#include "srgb_pair.h"

// Each predicate's refusal key: domain "gm20b", the predicate's name as the reason (kits/SwitchRefusal)
static constexpr std::array<uint64_t, PredCount> predKeys()
{
    std::array<uint64_t, PredCount> keys{};
    for (int p = 0; p < PredCount; p++)
        keys[p] = switch_refusal_key("gm20b", kPredNames[p]);
    return keys;
}

static constexpr std::array<uint64_t, PredCount> kPredKeys = predKeys();

static constexpr std::array<uint64_t, PredCount> failedKeys()
{
    std::array<uint64_t, PredCount> keys{};
    for (int p = 0; p < PredCount; p++)
        keys[p] = switch_refusal_key("gm20b.failed", failedName(SwitchMetalPred(p)));
    return keys;
}

static constexpr std::array<uint64_t, PredCount> kFailedKeys = failedKeys();
#if SWITCH_METAL_DIAGNOSTICS
std::atomic<uint64_t> gPredFailed[PredCount];
std::atomic<uint64_t> gPredSkipped[PredCount];
#endif
std::atomic<uint64_t> gPredLogged{0};

void gm20bSkipped(SwitchMetalPred p, uint64_t n)
{
    SWITCH_METAL_DIAGNOSTIC(gPredSkipped[p].fetch_add(n, std::memory_order_relaxed));
#if !SWITCH_METAL_DIAGNOSTICS
    (void)p, (void)n;
#endif
}

void predSkipped(SwitchMetalPred p, uint64_t a, uint64_t b, const char *rule, uint64_t n)
{
    gm20bSkipped(p, n);
    switch_refusal_k(switch_refusal_key_with(kPredKeys[p], uintptr_t(rule)), "gm20b", kPredNames[p],
                     "a=%llu (0x%llx) b=%llu (0x%llx) %s", (unsigned long long)a, (unsigned long long)a,
                     (unsigned long long)b, (unsigned long long)b, rule);
}

// Each predicate's first hit in this process, through kits/SwitchRefusal (domain raw-metal, the predicate's name as the
// reason)
void logPred(SwitchMetalPred p, uint64_t a, uint64_t b, NSString *detail)
{
    const uint64_t bit = 1ull << p;
    if (gPredLogged.fetch_or(bit, std::memory_order_relaxed) & bit)
        return;
    switch_refusal_k(kPredKeys[p], "gm20b", kPredNames[p], "a=%llu (0x%llx) b=%llu (0x%llx)%s%s", (unsigned long long)a,
                     (unsigned long long)a, (unsigned long long)b, (unsigned long long)b, detail ? " " : "",
                     detail ? detail.UTF8String : "");
#if !TARGET_OS_IPHONE
    fprintf(stderr, "SwitchMetal: first %s: a=%llu b=%llu %s\n", kPredNames[p], (unsigned long long)a,
            (unsigned long long)b, detail ? detail.UTF8String : "");
#endif
}

void predFails(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, const char *detail)
{
    if (!predLogged(p))
        logPred(p, a, b, detail ? @(detail) : nil);
    failCommandBuffer(
        cb, [NSString stringWithFormat:@"The driver refused %s%s%s", kPredNames[p], detail ? ": " : "", detail ?: ""],
        MTLCommandBufferError(predFailure(p)), p);
}

void commandBufferFailed(SwitchMetalPred p, NSError *error)
{
    SWITCH_METAL_DIAGNOSTIC(gPredFailed[p].fetch_add(1, std::memory_order_relaxed));
    logFailure(p, error);
}

void logFailure(SwitchMetalPred p, NSError *error)
{
    const char *description = error.localizedDescription.UTF8String ?: "";
    // none gathers causes that have nothing else in common: each description is a line of its own
    const uint64_t key =
        p ? kFailedKeys[p] : switch_refusal_key_with(kFailedKeys[PredNone], switch_refusal_key("", description));
    if (!switch_refusal_first(key))
        return;
    char text[512];
    snprintf(text, sizeof text, "a command buffer failed (MTLCommandBufferErrorDomain %ld): %s", long(error.code),
             description);
    switch_refusal_log("gm20b.failed", failedName(p), text);
}

// The type encoding of a selector some MTL* protocol declares, walked once per selector: objc_copyProtocolList copies the
// shared cache's protocols (the frontend's forwardSignature, r14-maps)
const char *metalTypes(SEL selector)
{
    static gm20b::UnfairMutex lock;
    static auto *known = new std::unordered_map<SEL, const char *>;
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock);
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
    std::lock_guard<gm20b::UnfairMutex> l(lock);
    known->emplace(selector, types);
    return types;
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
    SWITCH_REFUSE_DETAIL("gm20b", "selector.zero", uintptr_t(invocation.selector) * 31 + uintptr_t(cls),
                         "-[%s %s] answered zero", class_getName(cls), sel_getName(invocation.selector));
#if SWITCH_FRAME_TIMELINE
    if (gRM)
        SWITCH_METAL_DIAGNOSTIC(gRM->stats.zeroAnswers.fetch_add(1, std::memory_order_relaxed));
#endif
    returnZero(invocation);
}

gm20b::shapes::TextureShape textureShape(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    gm20b::shapes::TextureShape s;
    s.type = uint32_t(d.textureType);
    s.format = uint32_t(d.pixelFormat);
    s.storage = uint32_t(d.storageMode);
    s.usage = d.usage;
    s.width = d.width;
    s.height = d.height;
    s.depth = d.depth;
    s.levels = d.mipmapLevelCount;
    s.samples = d.sampleCount;
    s.array = d.arrayLength;
    s.rotation = [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return s;
}

bool gm20bShape(MTLTextureDescriptor *d)
{
    return gm20b::shapes::gm20bShape(textureShape(d));
}

bool chainShape(MTLTextureDescriptor *d)
{
    return gm20b::shapes::chainShape(textureShape(d));
}

bool lineShape(MTLTextureDescriptor *d)
{
    return gm20b::shapes::lineShape(textureShape(d));
}

bool volumeRendered(MTLTextureDescriptor *d)
{
    return gm20b::shapes::volumeRendered(textureShape(d));
}

bool volumeShape(MTLTextureDescriptor *d)
{
    return gm20b::shapes::volumeShape(textureShape(d));
}

uint32_t arrayType(MTLTextureDescriptor *d)
{
    return gm20b::shapes::arrayType(textureShape(d));
}

bool zetaDescriptor(MTLTextureDescriptor *d)
{
    return gm20b::shapes::zetaShape(textureShape(d));
}

bool multisampleShape(MTLTextureDescriptor *d)
{
    return gm20b::shapes::multisampleShape(textureShape(d));
}

uint32_t memorylessLayers(MTLTextureDescriptor *d)
{
    return gm20b::shapes::memorylessLayers(textureShape(d));
}

bool blockShape(MTLTextureDescriptor *d)
{
    return gm20b::shapes::blockShape(textureShape(d));
}

uint32_t gm20bShapeTerm(MTLTextureDescriptor *d)
{
    return gm20b::shapes::gm20bShapeTerm(textureShape(d));
}

// A kept extended-range texture's facts (xr.h), or an RG8Unorm_sRGB one's stand-in (srgb_pair.h). Metal keeps
// it compressed with allowGPUOptimizedContents, 16 texels or more a side and no ShaderWrite, PixelFormatView or
// ShaderAtomic usage, never over a buffer (m2v heap_tiled.rs metal_compresses); one draws may sample gets its stand-in.
// False: the memory or header of a stand-in it needs was refused, so no draw could sample it. A pack with no conversion
// state for the form (a host test's) makes it without one, which a draw sampling it refuses at its bind.
bool keepExtendedRange(MTLTextureDescriptor *d, gm20b::Texture &t, bool overBuffer)
{
    const bool sampled = d.usage == MTLTextureUsageUnknown || (d.usage & MTLTextureUsageShaderRead);
    if (gm20b::srgbPair(t.format))
        return !sampled || gRM->raw->createStandIn(t);
    if (!gm20b::xrSampled(t.format))
        return true;
    const MTLTextureUsage plain =
        MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView | MTLTextureUsageShaderAtomic;
    const bool every = d.usage == MTLTextureUsageUnknown;
    t.compressed =
        !overBuffer && d.allowGPUOptimizedContents && std::min(d.width, d.height) >= 16 && !every && !(d.usage & plain);
    return !sampled || gRM->raw->createStandIn(t) ||
           !gRM->raw->ownPipeline(gm20b::xrConversionOf(t.format, t.xrPlanes));
}
