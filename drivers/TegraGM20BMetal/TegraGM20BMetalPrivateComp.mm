// SPDX-License-Identifier: MIT
// The facade's half of private textures on compressed memory (private_comp.h): which descriptors qualify, and the
// submit worker's moves onto compressed memory.
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPipeline.h"
#include <algorithm>
#include <vector>
#include "view.h"
#include "xr.h"

static thread_local bool tSubmitting = false;

bool submitRangeExclusive(SwitchMetalCommandBuffer *cb, bool followed)
{
    tSubmitting = true;
    const bool submitted = submitRange(cb, followed);
    tSubmitting = false;
    return submitted;
}

void gm20bMarkSubmission(bool submitting)
{
    tSubmitting = submitting;
}

bool gm20bInSubmission()
{
    return tSubmitting;
}

uint32_t privateCompressedKind(MTLTextureDescriptor *d)
{
    const gm20b::FormatInfo *f = gm20b::formatInfo(uint32_t(d.pixelFormat));
    gm20b::PrivateTextureFacts facts;
    facts.privateStorage = d.storageMode == MTLStorageModePrivate;
    facts.levels = uint32_t(d.mipmapLevelCount);
    facts.samples = uint32_t(std::max<NSUInteger>(d.sampleCount, 1));
    facts.shape = d.textureType == MTLTextureType2D              ? gm20b::PrivateShape2D::Plain
                  : d.textureType == MTLTextureType2DArray       ? gm20b::PrivateShape2D::Array
                  : d.textureType == MTLTextureType2DMultisample ? gm20b::PrivateShape2D::Multisample
                                                                 : gm20b::PrivateShape2D::Other;
    facts.compressibleUsage =
        d.usage != MTLTextureUsageUnknown &&
        !(d.usage & ~NSUInteger(MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite));
    facts.plainColour =
        f && f->rt && !gm20b::blockCompressed(f) && gm20b::xrForm(uint32_t(d.pixelFormat)) == gm20b::XrForm::None;
    facts.zeta = gm20b::zetaFormatIndex(uint32_t(d.pixelFormat)) < gm20b::kZetaFormats ? uint32_t(d.pixelFormat) : 0;
    const bool cube = d.textureType == MTLTextureTypeCube || d.textureType == MTLTextureTypeCubeArray;
    facts.layers = uint32_t(std::max<NSUInteger>(d.arrayLength, 1)) * (cube ? 6 : 1);
    facts.bpp = f ? f->bpp : 0;
    facts.compression = (gRM->raw->kernelInfo().flags & TegraGM20BInfoCompression) != 0;
    return gm20b::privateCompressedKind(facts);
}

void gm20bWantCompressed(id<MTLTexture> texture)
{
    SwitchMetalTexture *t =
        [(id)texture isKindOfClass:[SwitchMetalTexture class]] ? (SwitchMetalTexture *)texture : nil;
    if (!t || !t->_movableBytes)
        return;
    t->_wanting = t;
    if (!gRM->raw->wantCompressed(t->_raw, privateCompressedKind(t->_descriptor), (__bridge void *)t))
        t->_movableBytes = 0;
}

static std::vector<SwitchMetalTexture *> &gMoving =
    *new std::vector<SwitchMetalTexture *>;  // the submit worker's alone

// Called under the device lock (Device::privateMoves): a texture whose want is still there has not reached the
// destroyTexture of its dealloc, which erases it under that lock, so its memory holds; one being deallocated loads nil
static void takeWanting(void *owner)
{
    __unsafe_unretained SwitchMetalTexture *t = (__bridge SwitchMetalTexture *)owner;
    if (SwitchMetalTexture *alive = t->_wanting)
        gMoving.push_back(alive);
}

void gm20bMoveWanting()
{
    gm20b::Device &d = *gRM->raw;
    if (!d.privateMoveDue())
        return;
    d.privateMoves(takeWanting);
    if (gMoving.empty())
        return;
    // a move's job is a command buffer of its own: an encode ahead holding the device's goes back first
    gm20bPipelineYield(nil, SIZE_MAX, false);
    for (SwitchMetalTexture *t : gMoving)
        d.compressedFromPlain(t->_raw);
    gMoving.clear();
}
