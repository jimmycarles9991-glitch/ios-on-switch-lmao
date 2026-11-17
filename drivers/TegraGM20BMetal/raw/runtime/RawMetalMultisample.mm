// SPDX-License-Identifier: MIT
#include "raw_lock.h"
#import "RawMetalMultisample.h"
#import "RawMetalPassCache.h"
#include "NativeBridgeMetalFrontend.h"

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d)
{
    return raw::shapes::storedSamples(textureShape(d));
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
    rmPassInvalidate();
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
    auto bound = [&](const auto &table, uint32_t index) {
        if (index >= table.size())
            return;
        const auto textures = table[index];
        raw::forEachSlot(textures.set, [&](uint32_t slot) {
            check(textures.objects[slot]);
        });
    };
    for (size_t at = from; at < to && !found; at++) {
        const Op op = plan.operation(at).op;
        if (isPlanDraw(op)) {
            const raw::RenderStateRef &state = plan.renderStates[plan.draw(at).state];
            bound(plan.textures[0], state.textures[0]);
            bound(plan.textures[1], state.textures[1]);
        } else if (op == OpDispatch) {
            bound(plan.kernelTextures, plan.kernelStates[plan.dispatch(at).state].textures);
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
