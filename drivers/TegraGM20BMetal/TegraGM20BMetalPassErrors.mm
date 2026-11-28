// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPassErrors.h"
#include "attachment_target.h"
#include "metal_formats.h"
#include "mixed_samples.h"
#include "zeta_colour.h"

namespace {

bool blockFormat(uint32_t format)
{
    const gm20b::metal_formats::Described *d = gm20b::metal_formats::described(format);
    return d && (d->blockW > 1 || d->blockH > 1);
}

bool hasDepth(MTLPixelFormat format)
{
    return format == MTLPixelFormatDepth16Unorm || format == MTLPixelFormatDepth32Float ||
           format == MTLPixelFormatDepth32Float_Stencil8;
}

bool hasStencil(MTLPixelFormat format)
{
    return format == MTLPixelFormatStencil8 || format == MTLPixelFormatDepth32Float_Stencil8 ||
           format == MTLPixelFormatX32_Stencil8;
}

bool resolves(MTLStoreAction a)
{
    return a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve;
}

NSUInteger slicesOf(id<MTLTexture> t)
{
    switch (t.textureType) {
    case MTLTextureTypeCube:
        return 6;
    case MTLTextureTypeCubeArray:
        return 6 * t.arrayLength;
    case MTLTextureType1DArray:
    case MTLTextureType2DArray:
    case MTLTextureType2DMultisampleArray:
        return t.arrayLength;
    default:
        return 1;
    }
}

// A slice of an array or cube and a depth plane of a 3D texture, each of the level
const char *partInvalid(id<MTLTexture> t, NSUInteger level, NSUInteger slice, NSUInteger plane)
{
    if (level >= t.mipmapLevelCount)
        return "a level past its texture's";
    if (t.textureType == MTLTextureType3D) {
        if (slice)
            return "a slice of a 3D texture";
        return plane >= std::max<NSUInteger>(1, t.depth >> level) ? "a depth plane past its level's" : nullptr;
    }
    if (plane)
        return "a depth plane of a texture that is not 3D";
    return slice >= slicesOf(t) ? "a slice past its texture's" : nullptr;
}

bool ours(id<MTLTexture> t)
{
    return [(id)t isKindOfClass:[SwitchMetalTexture class]];
}

// Apple's GPU keeps Depth32Float bits in a one-sample colour texture of 4-byte texels as the depth attachment
// (zeta_colour.h)
bool colourAsDepthTexture(id<MTLTexture> t)
{
    SwitchMetalTexture *x = (SwitchMetalTexture *)t;
    return ours(t) && x->_gm20bOk && gm20b::colourAsDepth(x->_raw);
}

// The slice of a type that has none, and the depth plane of one that is not 3D, are ignored
void ownIndices(MTLRenderPassAttachmentDescriptor *a)
{
    const MTLTextureType type = a.texture.textureType;
    if (type == MTLTextureType3D || type == MTLTextureType2D || type == MTLTextureType2DMultisample ||
        type == MTLTextureType1D)
        a.slice = 0;
    if (type != MTLTextureType3D)
        a.depthPlane = 0;
}

// A memoryless attachment reads as zeros where it is loaded, and nothing of it is stored
void memorylessAsApple(MTLRenderPassAttachmentDescriptor *a)
{
    if (a.texture.storageMode != MTLStorageModeMemoryless)
        return;
    if (a.loadAction == MTLLoadActionLoad) {
        a.loadAction = MTLLoadActionClear;
        if ([a isKindOfClass:[MTLRenderPassColorAttachmentDescriptor class]])
            ((MTLRenderPassColorAttachmentDescriptor *)a).clearColor = MTLClearColorMake(0, 0, 0, 0);
        else if ([a isKindOfClass:[MTLRenderPassDepthAttachmentDescriptor class]])
            ((MTLRenderPassDepthAttachmentDescriptor *)a).clearDepth = 0;
        else
            ((MTLRenderPassStencilAttachmentDescriptor *)a).clearStencil = 0;
    }
    if (a.storeAction == MTLStoreActionStore)
        a.storeAction = MTLStoreActionDontCare;
    else if (a.storeAction == MTLStoreActionStoreAndMultisampleResolve)
        a.storeAction = MTLStoreActionMultisampleResolve;
}

// An X32_Stencil8 view is the stencil of the Depth32Float_Stencil8 memory it views: an M4 Max writes that texture's stencil
// through one, beside the texture as depth or alone (harness/passformsoracle.mm stencil_view)
void viewedStencil(MTLRenderPassAttachmentDescriptor *a)
{
    SwitchMetalTexture *v = (SwitchMetalTexture *)a.texture;
    if (!ours(a.texture) || a.texture.pixelFormat != MTLPixelFormatX32_Stencil8 || !v->_levelsOwner ||
        v->_levelsOwner->_descriptor.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)
        return;
    a.level += v->_levelsBase;
    a.slice += v->_slicesBase;
    a.texture = (id<MTLTexture>)v->_levelsOwner;
}

// The other aspect of a Depth32Float_Stencil8 texture attached as one alone: kept as it was
void keepOtherAspect(MTLRenderPassAttachmentDescriptor *attached, MTLRenderPassAttachmentDescriptor *other)
{
    id<MTLTexture> t = attached.texture;
    if (!t || other.texture || t.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)
        return;
    other.texture = t;
    other.level = attached.level;
    other.slice = attached.slice;
    other.loadAction = MTLLoadActionLoad;
    other.storeAction = MTLStoreActionStore;
}

// A texture standing in for an attachment none of whose writes Metal keeps: memoryless, of a format the attachment's
// draws can target
id<MTLTexture> droppedAttachment(SwitchMetalCommandBuffer *cb, id<MTLTexture> t, NSUInteger layers)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = blockFormat(uint32_t(t.pixelFormat)) ? MTLPixelFormatRGBA8Unorm : t.pixelFormat;
    d.width = t.width;
    d.height = t.height;
    d.sampleCount = t.sampleCount;
    const bool multisampled = t.sampleCount > 1;
    if (layers > 1)
        d.textureType = multisampled ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
    else
        d.textureType = multisampled ? MTLTextureType2DMultisample : MTLTextureType2D;
    d.arrayLength = layers;
    d.storageMode = MTLStorageModeMemoryless;
    d.usage = MTLTextureUsageRenderTarget;
    return [(id<MTLDevice>)cb->_device newTextureWithDescriptor:d];
}

// The layers attachment `a` has from its slice (a 3D level's planes from its plane); none past the texture's
NSUInteger attachmentLayers(MTLRenderPassAttachmentDescriptor *a)
{
    id<MTLTexture> t = a.texture;
    const bool volume = t.textureType == MTLTextureType3D;
    const NSUInteger first = volume ? a.depthPlane : a.slice,
                     has = volume ? std::max<NSUInteger>(1, t.depth >> a.level) : slicesOf(t);
    return first < has ? has - first : 0;
}

// renderTargetArrayLength past an attachment's layers: an M4 Max draws every layer each attachment has and drops the rest
// (m2v-pass-forms/probe/bitsprobe.mm layers: colours of 4 and 2 layers and a depth of 2 under 4, the first takes all four),
// so a plain pass keeps the client's length, each shorter attachment rendered through a backing of the pass's
// (CommandBuffer::render), a memoryless one backed with the pass's. Clamping the length to the attachments' layers is
// wrong on the GM20B: it draws a render_target_array_index past THIRD_DIMENSION into layer 0 rather than dropping it
// (HW Test gpu.x1.layer_past_count, a target of two layers: past_count_c1=3/1/k/k; gpu.gm20b.rtal_past_layers read
// slice 1 holding layer 1's draw under the clamp). A pass with an attachment
// the encode moves through work of its own of the pass's layers (a format stand-in, a planar, YCbCr or two-plane target,
// a 3D plane image) stays of the layers every stored attachment has, the rest dropped: where that leaves layers of an
// attachment undrawn that the M4 Max draws, a skip (true)
bool layersAsApple(SwitchMetalCommandBuffer *cb, MTLRenderPassDescriptor *pass)
{
    const NSUInteger layers = pass.renderTargetArrayLength;
    if (layers <= 1)
        return false;
    NSUInteger most = 0, least = layers;
    bool plain = true;
    const auto count = [&](MTLRenderPassAttachmentDescriptor *a) {
        if (!a.texture || a.texture.storageMode == MTLStorageModeMemoryless)
            return;
        const NSUInteger has = std::max<NSUInteger>(attachmentLayers(a), 1);
        most = std::max(most, has);
        least = std::min(least, has);
        SwitchMetalTexture *x = (SwitchMetalTexture *)a.texture;
        plain = plain && ours(a.texture) && x->_gm20bOk && !gm20b::formatStandIn(x->_raw.format) &&
                !x->_raw.converted() && !x->_raw.xrPlanes && !x->_raw.xrPlane && x->_raw.type != gm20b::fx::kTexThreeD;
    };
    count(pass.depthAttachment);
    count(pass.stencilAttachment);
    for (NSUInteger c = 0; c < 8; c++)
        count(pass.colorAttachments[c]);
    if (!most)
        return false;
    if (!plain)
        pass.renderTargetArrayLength = least;
    if (plain || least >= std::min(layers, most))
        return false;
    skip(cb, PredPassAttLevel, std::min(layers, most), least,
         "renderTargetArrayLength past an attachment's layers, beside one the driver draws through work of its own: "
         "the layers every attachment has drawn, where an M4 Max draws each attachment's own");
    return true;
}

}  // namespace

bool gm20bPassAsApple(SwitchMetalCommandBuffer *cb, MTLRenderPassDescriptor *pass)
{
    MTLRenderPassDepthAttachmentDescriptor *depth = pass.depthAttachment;
    MTLRenderPassStencilAttachmentDescriptor *stencil = pass.stencilAttachment;
    if (depth.storeAction == MTLStoreActionCustomSampleDepthStore)
        depth.storeAction = MTLStoreActionStore;
    viewedStencil(stencil);
    keepOtherAspect(depth, stencil);
    keepOtherAspect(stencil, depth);
    for (MTLRenderPassAttachmentDescriptor *z :
         {(MTLRenderPassAttachmentDescriptor *)depth, (MTLRenderPassAttachmentDescriptor *)stencil})
        if (z.texture) {
            ownIndices(z);
            memorylessAsApple(z);
        }
    const NSUInteger layers = std::max<NSUInteger>(pass.renderTargetArrayLength, 1);
    for (NSUInteger c = 0; c < 8; c++) {
        MTLRenderPassColorAttachmentDescriptor *a = pass.colorAttachments[c];
        id<MTLTexture> t = a.texture;
        if (!ours(t))
            continue;
        ownIndices(a);
        if (blockFormat(uint32_t(t.pixelFormat)) || partInvalid(t, a.level, a.slice, a.depthPlane)) {
            if (id<MTLTexture> none = droppedAttachment(cb, t, layers)) {
                a.texture = none;
                a.level = a.slice = a.depthPlane = 0;
                a.loadAction = MTLLoadActionDontCare;
                a.storeAction = MTLStoreActionDontCare;
                a.resolveTexture = nil;
            }
        }
        memorylessAsApple(a);
    }
    return layersAsApple(cb, pass);
}

// A one-sample colour beside one four-sample depth/stencil texture renders as an M4 Max renders it (mixed_samples.h)
const char *gm20bPassShapeInvalid(MTLRenderPassDescriptor *pass)
{
    constexpr const char *kCounts = "attachments of different sample counts";
    const char *rule = nullptr;
    const auto check = [&](MTLRenderPassAttachmentDescriptor *a, NSUInteger &samples) {
        id<MTLTexture> t = a.texture;
        if (!t || rule)
            return;
        if (!samples)
            samples = t.sampleCount;
        if (t.sampleCount != samples)
            rule = kCounts;
    };
    NSUInteger zeta = 0, colour = 0;
    check(pass.depthAttachment, zeta);
    check(pass.stencilAttachment, zeta);
    for (NSUInteger c = 0; c < 8; c++)
        check(pass.colorAttachments[c], colour);
    id<MTLTexture> d = pass.depthAttachment.texture, s = pass.stencilAttachment.texture;
    if (!rule && zeta && colour && zeta != colour &&
        (!gm20b::mixedSamples(uint32_t(colour), uint32_t(zeta)) || (d && s && d != s)))
        rule = kCounts;
    return rule;
}

const char *gm20bColorTargetInvalid(MTLRenderPassColorAttachmentDescriptor *a)
{
    id<MTLTexture> t = a.texture;
    if (!ours(t))
        return "a colour attachment of another device's texture";
    if (blockFormat(uint32_t(t.pixelFormat)))
        return "a colour attachment of a block format with no memory for the texture standing in for it";
    // an M4 Max makes no texture of it, by descriptor, view or buffer (harness/passformsoracle.mm unspecialized)
    if (t.pixelFormat == MTLPixelFormat(263))
        return "a colour attachment of MTLPixelFormatUnspecialized, which Metal makes no texture of";
    // a one-sample Depth32Float or Depth16Unorm takes its colour twin's bits (zeta_colour.h)
    if (t.sampleCount <= 1 && gm20b::depthAsColourFormat(uint32_t(t.pixelFormat)))
        return nullptr;
    return hasDepth(t.pixelFormat) || hasStencil(t.pixelFormat) ? "a colour attachment of a depth/stencil format"
                                                                : nullptr;
}

const char *gm20bColorPartInvalid(MTLRenderPassColorAttachmentDescriptor *a)
{
    id<MTLTexture> t = a.texture;
    if (const char *rule = partInvalid(t, a.level, a.slice, a.depthPlane))
        return rule;
    // a store the encoder decides may resolve (setColorStoreAction:atIndex:); any other leaves the resolve alone
    id<MTLTexture> r = a.resolveTexture;
    if (!r || !(resolves(a.storeAction) || a.storeAction == MTLStoreActionUnknown))
        return nullptr;
    if (!ours(r))
        return "a resolve into another device's texture";
    if (!gm20b::resolvesInto(uint32_t(t.pixelFormat), uint32_t(r.pixelFormat)))
        return "a resolve into a texture of another pixel format";
    return partInvalid(r, a.resolveLevel, a.resolveSlice, a.resolveDepthPlane);
}

const char *gm20bZetaAttachmentInvalid(MTLRenderPassDescriptor *pass)
{
    const auto aspect = [](MTLRenderPassAttachmentDescriptor *a, bool depth) -> const char * {
        id<MTLTexture> t = a.texture;
        if (!t)
            return nullptr;
        if (!ours(t))
            return "a depth or stencil attachment of another device's texture";
        if (depth && !hasDepth(t.pixelFormat) && !colourAsDepthTexture(t))
            return "a depth attachment of a format with no depth";
        if (!depth && !hasStencil(t.pixelFormat))
            return "a stencil attachment of a format with no stencil";
        if (const char *rule = partInvalid(t, a.level, a.slice, a.depthPlane))
            return rule;
        if (!resolves(a.storeAction) || !a.resolveTexture)
            return nullptr;
        if (t.sampleCount <= 1)
            return "a resolve of a one-sample depth or stencil";
        id<MTLTexture> r = a.resolveTexture;
        if (!ours(r))
            return "a depth or stencil resolve into another device's texture";
        if (depth ? !hasDepth(r.pixelFormat) : !hasStencil(r.pixelFormat))
            return "a depth resolve into a texture with no depth, or a stencil one into one with no stencil";
        return partInvalid(r, a.resolveLevel, a.resolveSlice, a.resolveDepthPlane);
    };
    if (const char *rule = aspect(pass.depthAttachment, true))
        return rule;
    return aspect(pass.stencilAttachment, false);
}

bool gm20bZetaResolvesIntoMemoryless(MTLRenderPassDescriptor *pass)
{
    for (MTLRenderPassAttachmentDescriptor *a : {(MTLRenderPassAttachmentDescriptor *)pass.depthAttachment,
                                                 (MTLRenderPassAttachmentDescriptor *)pass.stencilAttachment})
        if (a.texture && resolves(a.storeAction) && a.resolveTexture.storageMode == MTLStorageModeMemoryless)
            return true;
    return false;
}

// The encoder is marked failed, and the failure vetoes the encode ahead of the commit (failCommandBuffer: VetoRefused);
// the command buffer fails before its first range is submitted (TegraGM20BMetalSubmit.mm submitRange)
void gm20bPassFailed(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, NSString *why,
                     MTLCommandBufferError code)
{
    cb->_encFailed = true;
    hit(cb, p, a, b);
    failCommandBuffer(cb, why, code, p);
}

void gm20bCallInvalid(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, const char *rule)
{
    if (!predLogged(p))
        logPred(p, a, b, @(rule));
    gm20bPassFailed(cb, p, a, b, [NSString stringWithFormat:@"%s: Metal's validation error", rule],
                    MTLCommandBufferErrorInvalidResource);
}

void gm20bPassInvalid(SwitchMetalCommandBuffer *cb, SwitchMetalPred p, uint64_t a, uint64_t b, const char *rule)
{
    SWITCH_REFUSE_DETAIL("gm20b", "pass.invalid", uintptr_t(rule), "%s: Metal's validation error (MTLRenderPass.h)",
                         rule);
    gm20bPassFailed(cb, p, a, b, [NSString stringWithFormat:@"A render pass Metal's validation stops: %s", rule]);
}

void gm20bPassWorkFails(SwitchMetalCommandBuffer *cb, bool ahead, const char *why, MTLCommandBufferError code)
{
    if (!ahead)
        failCommandBuffer(cb, @(why), code);
}

void gm20bOwnDrawFails(SwitchMetalCommandBuffer *cb, bool ahead, const char *why, const gm20b::Refusal &f)
{
    const bool memory = f.id == gm20b::RefHeapMemory || f.id == gm20b::RefNoTic || f.id == gm20b::RefTestsMemory ||
                        f.id == gm20b::RefViewHeader || f.id == gm20b::RefRotated;
    gm20bPassWorkFails(cb, ahead, why, memory ? MTLCommandBufferErrorOutOfMemory : MTLCommandBufferErrorInternal);
}
