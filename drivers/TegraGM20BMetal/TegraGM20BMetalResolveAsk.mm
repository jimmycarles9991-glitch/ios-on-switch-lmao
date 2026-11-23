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

#if SWITCH_METAL_DIAGNOSTICS
// Once a reason: what made a screen-format resolve take the pass, so the device log names the fact
static void sayReadable(SwitchMetalTexture *into, const gm20b::ResolveTarget &t, uint32_t samples)
{
    static std::atomic<uint32_t> said{0};
    const uint32_t why = t.sharedStorage | t.imported << 1 | t.overBuffer << 2 | t.storage << 3 | t.readAsData << 4;
    if (said.fetch_or(why) & why)
        return;
    os_log(gm20bLog(),
           "SwitchMetal: a %ux %ux%u format %u resolve is read as data, so the pass: shared=%d imported=%d buffer=%d "
           "storage=%d read=%d usage=0x%lx kind=%u",
           samples, into->_raw.width, into->_raw.height, into->_raw.format, t.sharedStorage, t.imported, t.overBuffer,
           t.storage, t.readAsData, (unsigned long)into->_descriptor.usage, unsigned(into->_kind));
}
#endif

gm20b::ResolveAsk resolveAsk(SwitchMetalTexture *into, const gm20b::Texture &target, const gm20b::Texture &rendered,
                             uint32_t width, uint32_t height)
{
    gm20b::ResolveAsk a;
    a.format = uint16_t(target.format);
    a.samples = uint8_t(rendered.samples);
    const gm20b::ResolveTarget kind = resolveTarget(into);
    a.readable = gm20b::resolveTargetReadable(kind);
#if SWITCH_METAL_DIAGNOSTICS
    if (a.readable && (a.format == 70 || a.format == 80) && (a.samples == 2 || a.samples == 4))
        sayReadable(into, kind, a.samples);
#endif
    a.wholeTarget = width == target.width && height == target.height;
    a.shadowTarget = target.shadowTarget;
    return a;
}
