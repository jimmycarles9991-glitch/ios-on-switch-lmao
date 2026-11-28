// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalResolveAsk.h"

void noteReadAsData(id<MTLTexture> t)
{
    for (SwitchMetalTexture *x = [(id)t isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)t : nil; x;
         x = x->_parent)
        x->_readAsData.store(true, std::memory_order_relaxed);
}

gm20b::ResolveTarget resolveTarget(SwitchMetalTexture *into)
{
    gm20b::ResolveTarget t;
    for (SwitchMetalTexture *x = into; x; x = x->_parent) {
        MTLTextureDescriptor *d = x->_descriptor;
        t.sharedStorage |= d.storageMode == MTLStorageModeShared ||
                           (x->_heap && x->_heap->_descriptor.storageMode == MTLStorageModeShared);
        const bool surface = x->_kind == SwitchMetalTextureSurface || x->_surface;
        const bool renderTargetOnly = (d.usage & ~MTLTextureUsageRenderTarget) == 0;
        t.imported |= surface ? !renderTargetOnly : x->_raw.import != nullptr;
        t.overBuffer |= x->_kind == SwitchMetalTextureBuffer || x->_buffer;
        t.storage |= (d.usage & MTLTextureUsageShaderWrite) != 0;
        t.readAsData |= x->_readAsData.load(std::memory_order_relaxed);
    }
    return t;
}

gm20b::ResolveAsk resolveAsk(SwitchMetalTexture *into, const gm20b::Texture &target, const gm20b::Texture &rendered,
                             uint32_t width, uint32_t height)
{
    gm20b::ResolveAsk a;
    a.format = uint16_t(target.format);
    a.samples = uint8_t(rendered.samples);
    const gm20b::ResolveTarget kind = resolveTarget(into);
    a.readable = gm20b::resolveTargetReadable(kind);
    a.wholeTarget = width == target.width && height == target.height;
    a.shadowTarget = target.shadowTarget;
    return a;
}
