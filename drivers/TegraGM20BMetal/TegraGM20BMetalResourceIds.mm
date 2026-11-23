// SPDX-License-Identifier: MIT
// gpuResourceID of the driver's textures and samplers (resource_id.h), and what a draw finds behind one it loads from an
// argument buffer: the first ask of an object's ID names the object in the device's handle tables and heap
// (heap_tables.h), and its release names none there again. An ID gets into an argument buffer only through its ask
// (an encoder's setTexture: asks too), so no other object's creation writes the tables.
#import "TegraGM20BMetalInternal.h"
#include "heap_fit.h"
#include "resource_id.h"
#include "sampler_word.h"

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

static gm20b::HeapTextureFacts heapFacts(SwitchMetalTexture *t)
{
    MTLTextureDescriptor *d = t->_descriptor;
    if (!d)
        once(
            @"argument buffers: a texture with no descriptor named its ID: an argument buffer reads the neutral there");
    const MTLTextureSwizzleChannels s = d.swizzle;
    return gm20b::heapFacts({uint32_t(d.pixelFormat),
                             uint32_t(d.textureType),
                             uint32_t(d.sampleCount),
                             uint32_t(d.usage),
                             uint32_t(d.arrayLength),
                             uint32_t(d.mipmapLevelCount),
                             {uint8_t(s.red), uint8_t(s.green), uint8_t(s.blue), uint8_t(s.alpha)}});
}

void gm20bHeapForgetTexture(SwitchMetalTexture *t)
{
    if (gRM && t->_idNamed.load(std::memory_order_acquire))
        gRM->raw->forgetTexture(t->_raw);
}

void gm20bHeapForgetSampler(SwitchMetalSampler *s)
{
    if (gRM && s->_idNamed.load(std::memory_order_acquire))
        gRM->raw->forgetSampler(s->_raw, s->_normalized);
}

@implementation SwitchMetalTexture (ResourceID)

- (MTLResourceID)gpuResourceID
{
    const uint64_t word = gm20b::textureResourceId(_raw.tic);
    if (word && gRM && !_idNamed.exchange(true, std::memory_order_acq_rel)) {
        const gm20b::HeapTextureFacts facts = heapFacts(self);
        if (!gRM->raw->nameTexture(_raw, facts, gm20b::heapTextureFlags(facts)))
            heapRefused("texture");
    }
    return resourceId(word);
}

@end

@implementation SwitchMetalSampler (ResourceID)

- (MTLResourceID)gpuResourceID
{
    const uint64_t word = gm20b::samplerResourceId(_raw.tsc);
    if (word && gRM && !_idNamed.exchange(true, std::memory_order_acq_rel)) {
        MTLSamplerDescriptor *d = _descriptor;
        const gm20b::SamplerFacts facts = {d.minFilter,     d.magFilter,       d.mipFilter,   d.sAddressMode,
                                           d.tAddressMode,  d.rAddressMode,    d.borderColor, d.normalizedCoordinates,
                                           d.maxAnisotropy, d.compareFunction, d.lodAverage,  d.lodMinClamp,
                                           d.lodMaxClamp};
        if (!gRM->raw->nameSampler(_raw, gm20b::samplerWord(facts), _normalized))
            heapRefused("sampler");
    }
    return resourceId(word);
}

@end
