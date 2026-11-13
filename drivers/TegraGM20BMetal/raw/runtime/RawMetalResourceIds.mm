// SPDX-License-Identifier: MIT
// gpuResourceID of the raw path's textures and samplers (raw_resource_id.h), and what a draw finds behind one it loads from an
// argument buffer: the first ask of an object's ID names the object in the device's handle tables and heap
// (raw_heap_tables.h), and its release names none there again. An ID gets into an argument buffer only through its ask
// (an encoder's setTexture: asks too), so no other object's creation writes the tables.
#import "RawMetalInternal.h"
#include "bridge/NativeMetalDirect.h"
#include "raw_resource_id.h"

static_assert(sizeof(raw::HeapTextureFacts) == sizeof(NmbHeapTextureFacts) &&
                  offsetof(raw::HeapTextureFacts, swizzle) == offsetof(NmbHeapTextureFacts, swizzle),
              "the bridge's texture facts");

bool rmHeapHolds(uint32_t heapClass, const raw::HeapTextureFacts *facts)
{
    return switch_nmb_heap_holds(heapClass, reinterpret_cast<const NmbHeapTextureFacts *>(facts));
}

static MTLResourceID resourceId(uint64_t word)
{
    MTLResourceID resource;
    static_assert(sizeof resource == sizeof word);
    memcpy(&resource, &word, sizeof word);
    return resource;
}

static void heapRefused(const char *what)
{
    once([NSString stringWithFormat:@"argument buffers: no memory for the device's handle tables (%s): an argument "
                                    @"buffer's %s reads the neutral",
                                    what, what]);
}

static raw::HeapTextureFacts heapFacts(RMTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    if (!d)
        once(
            @"argument buffers: a texture with no descriptor named its ID: an argument buffer reads the neutral there");
    const MTLTextureSwizzleChannels s = d.swizzle;
    return raw::heapFacts({uint32_t(d.pixelFormat),
                           uint32_t(d.textureType),
                           uint32_t(d.sampleCount),
                           uint32_t(d.usage),
                           uint32_t(d.arrayLength),
                           uint32_t(d.mipmapLevelCount),
                           {uint8_t(s.red), uint8_t(s.green), uint8_t(s.blue), uint8_t(s.alpha)}});
}

void rmHeapForgetTexture(RMTexture *t)
{
    if (gRM && t->_idNamed.load(std::memory_order_acquire))
        gRM->raw->forgetTexture(t->_raw);
}

void rmHeapForgetSampler(RMSampler *s)
{
    if (gRM && s->_idNamed.load(std::memory_order_acquire))
        gRM->raw->forgetSampler(s->_raw, s->_normalized);
}

@implementation RMTexture (ResourceID)

- (MTLResourceID)gpuResourceID
{
    rmTexturePlain(self);
    const uint64_t word = raw::textureResourceId(_raw.tic);
    if (word && gRM && !_idNamed.exchange(true, std::memory_order_acq_rel)) {
        const raw::HeapTextureFacts facts = heapFacts(self);
        const uint32_t flags = switch_nmb_heap_texture_flags(reinterpret_cast<const NmbHeapTextureFacts *>(&facts));
        if (!gRM->raw->nameTexture(_raw, facts, flags))
            heapRefused("texture");
    }
    return resourceId(word);
}

@end

@implementation RMSampler (ResourceID)

- (MTLResourceID)gpuResourceID
{
    const uint64_t word = raw::samplerResourceId(_raw.tsc);
    if (word && gRM && !_idNamed.exchange(true, std::memory_order_acq_rel)) {
        MTLSamplerDescriptor *d = _descriptor;
        const NmbSamplerDescriptor desc = {d.minFilter,     d.magFilter,       d.mipFilter,   d.sAddressMode,
                                           d.tAddressMode,  d.rAddressMode,    d.borderColor, d.normalizedCoordinates,
                                           d.maxAnisotropy, d.compareFunction, d.lodAverage,  d.lodMinClamp,
                                           d.lodMaxClamp};
        if (!gRM->raw->nameSampler(_raw, switch_nmb_sampler_word(&desc), _normalized))
            heapRefused("sampler");
    }
    return resourceId(word);
}

@end
