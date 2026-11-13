// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#import "RawMetalMultisample.h"
#include "NativeBridgeMetalFrontend.h"

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d)
{
    if (d.textureType != MTLTextureType2DMultisample)
        return RMStoredSamples::None;
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    if ((d.sampleCount != 2 && d.sampleCount != 4) || d.mipmapLevelCount > 1 || d.arrayLength > 1 || d.depth > 1 ||
        turned || d.storageMode != MTLStorageModePrivate)
        return RMStoredSamples::None;
    const uint32_t format = uint32_t(d.pixelFormat);
    if (raw::zetaFormatIndex(format) < raw::kZetaFormats)
        return (d.usage & ~MTLTextureUsageShaderRead) == MTLTextureUsageRenderTarget ? RMStoredSamples::Zeta
                                                                                     : RMStoredSamples::None;
    const raw::FormatInfo *f = raw::formatInfo(format);
    return f && f->rt && !raw::blockCompressed(f) && raw::xrForm(format) == raw::XrForm::None ? RMStoredSamples::Colour
                                                                                              : RMStoredSamples::None;
}

bool makeStoredMultisample(MTLTextureDescriptor *d, RMStoredSamples kind, raw::Texture &t)
{
    const uint32_t width = uint32_t(d.width), height = uint32_t(d.height), format = uint32_t(d.pixelFormat),
                   samples = uint32_t(d.sampleCount);
    const bool zeta = kind == RMStoredSamples::Zeta;
    const bool made = zeta ? gRM->raw->createOwnedZeta(width, height, format, samples, t)
                           : gRM->raw->createStoredMultisample(width, height, format, samples, t);
    static std::atomic<bool> said[2];
    if (made && !said[zeta].exchange(true))
        os_log(rawLog(), "raw: stored %ux multisampled %{public}s on (first %ux%u format %u: %llu bytes of %{public}s)",
               samples, zeta ? "depth/stencil" : "colour", width, height, format,
               (unsigned long long)(zeta ? t.storage.bytes : t.span.bytes),
               zeta ? "zeta memory, no m2v twin" : "block-linear memory, m2v's twin NVK's image over it");
    return made;
}

static const char *zetaMemoryRefusal(int asked)
{
    switch (asked) {
    case -3:
        return "m2v made no texture";
    case -1:
        return "the frontend or NVK has no texture memory accessor";
    case -2:
        return "its texels are no one NVK image's";
    case 2:
        return "NVK's image has more than one plane";
    case 3:
        return "NVK's image is not bound";
    case 4:
        return "NVK's image has more than one level";
    case 5:
        return "NVK's image has more than one layer";
    case 6:
        return "NVK's image is multisampled";
    default:
        return "NVK names no image";
    }
}

bool makeStoredZeta(MTLTextureDescriptor *d, raw::Texture &t)
{
    const uint32_t width = uint32_t(d.width), height = uint32_t(d.height), format = uint32_t(d.pixelFormat);
    const uint32_t layers =
        raw::textureLayers(uint32_t(d.textureType), uint32_t(std::min<NSUInteger>(d.arrayLength, 2048)));
    const bool made =
        gRM->raw->createOwnedZeta(width, height, format, 1, (d.usage & MTLTextureUsageShaderRead) != 0, t, layers);
    static std::atomic<bool> said{false};
    if (made && !said.exchange(true))
        os_log(
            rawLog(),
            "raw: stored one-sample depth/stencil on (first %ux%u format %u, %u layers: %llu bytes of %{public}s zeta "
            "memory, no m2v twin)",
            width, height, format, layers, (unsigned long long)t.storage.bytes, t.comptags ? "compressed" : "plain");
    if (!made)
        SWITCH_REFUSE_DETAIL("raw", "zeta.owned", d.pixelFormat,
                             "a stored %lux%lu depth/stencil texture of format %lu stays m2v's: no zeta memory",
                             (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat);
    return made;
}

void rmStoredZetaForM2v(RMTexture *t)
{
    if (t && t->_rawOk && t->_raw.zeta && t->_raw.samples == 1 && zetaDescriptor(t->_descriptor))
        (void)[t rmTwin];
}

static id storedZetaTwinExcluded(RMTexture *t, id<MTLDevice> m2v, const char **why)
{
    @synchronized(t) {
        if (t->_twin)
            return t->_twin;
    }
    if (tRawEncoding) {
        *why = "it was asked for inside a raw encode";
        return nil;
    }
    id twin = [m2v newTextureWithDescriptor:t->_descriptor];
    struct switch_nvk_image_memory m;
    const int asked = !twin                                 ? -3
                      : !native_bridge_metal_texture_memory ? -1
                                                            : native_bridge_metal_texture_memory(twin, &m);
    if (asked) {
        *why = zetaMemoryRefusal(asked);
        return nil;
    }
    raw::ZetaMemory z;
    z.va = m.addr;
    z.bytes = m.size_B;
    z.layerBytes = m.array_stride_B;
    z.pitch = m.row_stride_B;
    z.bpp = m.texel_B;
    z.blockHeight = m.y_log2;
    z.blockDepth = m.z_log2;
    z.tiled = m.tiled;
    z.compressed = m.compressed;
    @synchronized(t) {
        if (!gRM->raw->moveOwnedZeta(t->_raw, z, why))
            return nil;
    }
    gRM->rawSinceM2v = true;
    // published with submissions excluded, so a caller that waited on them finds this twin rather than moving again
    return twinPublish(t, t->_twin, twin);
}

id storedZetaTwin(RMTexture *t, id<MTLDevice> m2v, const char **why)
{
    return rmExcludingSubmissions([&]() -> id {
        return storedZetaTwinExcluded(t, m2v, why);
    });
}

bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, raw::SamplePositions &out)
{
    const auto read = [pass](float *xy, uint32_t n) {
        MTLSamplePosition at[8];
        [pass getSamplePositions:at count:n];
        for (uint32_t i = 0; i < n; i++) {
            xy[2 * i] = at[i].x;
            xy[2 * i + 1] = at[i].y;
        }
    };
    if (!raw::readSamplePositions(uint32_t(std::min<NSUInteger>(count, 9)), samples, read, out))
        return false;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log(rawLog(),
               "raw: a pass's own sample positions on (first: %u of %u samples, x | y << 4 %02x %02x %02x %02x)",
               unsigned(out.count), samples, out.xy[0], out.xy[1], out.xy[2], out.xy[3]);
    return true;
}

static bool ownedZeta(void *object)
{
    id o = (__bridge id)object;
    if (![o isKindOfClass:[RMTexture class]])
        return false;
    const RMTexture *t = (RMTexture *)o;
    return t->_rawOk && t->_raw.zeta && (t->_raw.samples > 1 || t->_raw.arena);
}

// Whether every pass of the range that attaches `t` neither loads nor stores it: m2v's own texture of its descriptor then
// stands in for it, as nothing in the raw path's memory is read or kept
static bool transientIn(const RMCommandPlan &plan, size_t from, size_t to, void *t)
{
    for (size_t at = from; at < to; at++) {
        const Cmd c = plan.legacy(at);
        if (c.op != OpRender)
            continue;
        MTLRenderPassDescriptor *pass = (__bridge MTLRenderPassDescriptor *)c.obj;
        for (MTLRenderPassAttachmentDescriptor *a : {(MTLRenderPassAttachmentDescriptor *)pass.depthAttachment,
                                                     (MTLRenderPassAttachmentDescriptor *)pass.stencilAttachment})
            if ((__bridge void *)a.texture == t &&
                (a.loadAction == MTLLoadActionLoad || a.storeAction != MTLStoreActionDontCare))
                return false;
    }
    return true;
}

RMTexture *ownedZetaIn(RMCommandBuffer *cb, size_t from, size_t to)
{
    void *found = nullptr;
    auto check = [&](void *object) {
        if (!found && object && ownedZeta(object))
            found = object;
    };
    const RMCommandPlan &plan = cb->_plan;
    plan.forEachUse(from, to, [&](void *object, uint8_t access) {
        if ((access & ~raw::ResourceUse::Write) || !transientIn(plan, from, to, object))
            check(object);
    });
    auto bound = [&](uint32_t stage, uint32_t index) {
        if (index < plan.textures[stage].size())
            for (void *object : plan.textures[stage][index].objects)
                check(object);
    };
    for (size_t at = from; at < to && !found; at++) {
        const Op op = plan.operation(at).op;
        if (isPlanDraw(op)) {
            const raw::RenderStateRef &state = plan.renderStates[plan.draw(at).state];
            bound(0, state.textures[0]);
            bound(1, state.textures[1]);
        } else if (op == OpDispatch) {
            bound(2, plan.kernelStates[plan.dispatch(at).state].textures);
        }
    }
    return (__bridge RMTexture *)found;
}

void failOwnedZeta(RMCommandBuffer *cb, RMTexture *t)
{
    SWITCH_REFUSE_DETAIL(
        "raw", "replay.zeta", t->_descriptor.pixelFormat,
        "a command buffer range failed before m2v encoded it: it uses a %ux%u depth/stencil texture of "
        "%u samples whose memory m2v has no texture of",
        t->_raw.width, t->_raw.height, unsigned(t->_raw.samples));
    NSString *text = [NSString
        stringWithFormat:@"The depth/stencil texture %s (%ux%u, pixel format %lu) has no compatibility texture",
                         [t label].UTF8String ?: "-", t->_raw.width, t->_raw.height,
                         (unsigned long)t->_descriptor.pixelFormat];
    NSError *error = [NSError errorWithDomain:MTLCommandBufferErrorDomain
                                         code:MTLCommandBufferErrorInternal
                                     userInfo:@{NSLocalizedDescriptionKey : text}];
    std::lock_guard<raw::Mutex> l(cb->_lock);
    if (!cb->_segError)
        cb->_segError = error;
}
