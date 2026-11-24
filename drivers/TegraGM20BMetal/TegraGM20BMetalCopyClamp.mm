// SPDX-License-Identifier: MIT
// The clamp Apple's GPU copies through a compressed BGRA10_XR or BGRA10_XR_sRGB texture leave in their destination
// (TegraGM20BMetalCopyClamp.h).
#import "TegraGM20BMetalCopyClamp.h"
#import "TegraGM20BMetalBlitImages.h"
#include "attachment_target.h"
#include "fresh.h"

namespace {

constexpr uint32_t kRgba16Uint = 113;  // BGRA10_XR's fields as the format's own unsigned-integer target holds them

gm20b::Pipeline *clampState(const char **why)
{
    gm20b::Pipeline *state = gRM->raw->ownPipeline("raw.xr.xr_copy_clamp_frag");
    if (!state)
        *why = "the pack has no copy clamp state (raw.xr.xr_copy_clamp_frag)";
    return state;
}

// The scratch the copy engine moves the copied texels into, `memory` (the blit encoder's key of the destination) read first
gm20b::Texture *scratchOf(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const void *memory, uint32_t w, uint32_t h,
                          const char **why)
{
    gm20b::Texture *scratch = rc->turnScratch(kRgba16Uint, w, h, kCopyClampTag);
    if (!scratch) {
        *why = "no memory for the scratch of a copy's alpha clamp";
        return nullptr;
    }
    be->access(memory, scratch);
    return scratch;
}

// One pass of the clamp state over (x, y, w, h) of `target`, drawn from `scratch`; `be` reopened after it
bool drawClamped(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *&be, gm20b::Pipeline *state, gm20b::Texture &target,
                 const gm20b::Texture &scratch, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char **why)
{
    be->end();
    gm20b::PassDesc p;
    p.color[0].texture = &target;
    p.color[0].load = gm20b::ColorAttachment::Load;
    p.color[0].store = true;
    p.count = 1;
    p.width = target.width;
    p.height = target.height;
    gm20b::RenderEncoder *e = rc->render(p, why);
    bool drawn = false;
    if (e) {
        e->setPipeline(state);
        e->setTexture(0, &scratch);
        const uint32_t origin[4] = {x, y, 0, 0};
        e->setBytes(2, 0, origin, sizeof origin);
        e->setViewport(x, y, w, h, 0, 1);
        e->setScissor(x, y, w, h);
        drawn = e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why);
        if (drawn)
            e->end();
    }
    be = rc->blit();
    return drawn;
}

}  // namespace

// A 2D BGRA10_XR or BGRA10_XR_sRGB texture of a descriptor, block-linear, one sample: the shape the copies were measured
// with and the clamp pass renders in place
static bool measuredXr(SwitchMetalTexture *t)
{
    if (!t || (t->_raw.format != 552 && t->_raw.format != 553) || t->_descriptor.textureType != MTLTextureType2D)
        return false;
    __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    return !owner->_buffer && !owner->_surface && owner->_raw.blockLinear && owner->_raw.samples <= 1;
}

bool gm20bCompressedXr(SwitchMetalTexture *t, uint32_t level)
{
    if (!measuredXr(t))
        return false;
    __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    MTLTextureDescriptor *d = owner->_descriptor;
    const uint32_t absolute = t->_levelsBase + level;
    const uint32_t w = std::max(owner->_raw.width >> absolute, 1u), h = std::max(owner->_raw.height >> absolute, 1u);
    return gm20b::metalCompresses(uint64_t(d.usage), d.allowGPUOptimizedContents, w, h);
}

bool gm20bCopyClamps(SwitchMetalTexture *from, uint32_t fromLevel, SwitchMetalTexture *to, uint32_t toLevel)
{
    if (to && !measuredXr(to))
        return false;
    return gm20bCompressedXr(from, fromLevel) || gm20bCompressedXr(to, toLevel);
}

bool gm20bClampCopiedTexture(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *&be, SwitchMetalTexture *t, uint32_t level,
                             uint32_t slice, uint32_t plane, uint32_t depth, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t h, const char **why)
{
    gm20b::Pipeline *state = clampState(why);
    __unsafe_unretained SwitchMetalTexture *owner = t->_levelsOwner ?: t;
    gm20b::Texture image;
    if (!state || !blitImage(t, level, image))
        return false;
    const bool layered = t->_raw.layers > 1;
    for (uint32_t k = 0; k < std::max(depth, 1u); k++) {
        const uint32_t z = (layered ? slice : plane) + k;
        gm20b::Texture target;
        if (!gm20b::attachmentTarget(owner->_raw, t->_levelsBase + level, layered ? t->_slicesBase + z : z, target)) {
            *why = "a copy's destination the clamp pass cannot render";
            return false;
        }
        target.format = kRgba16Uint;
        target.renderable = true;
        gm20b::Texture *scratch = scratchOf(rc, be, (__bridge const void *)owner, w, h, why);
        if (!scratch)
            return false;
        be->copyTexels(image, x, y, z, *scratch, 0, 0, 0, w, h);
        if (!drawClamped(rc, be, state, target, *scratch, x, y, w, h, why))
            return false;
    }
    return true;
}

bool gm20bClampCopiedBuffer(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *&be, SwitchMetalBuffer *b, uint64_t offset,
                            uint64_t row, uint64_t image, uint32_t w, uint32_t h, uint32_t planes, const char **why)
{
    gm20b::Pipeline *state = clampState(why);
    if (!state)
        return false;
    const gm20b::Buffer &buffer = b->_raw;
    for (uint32_t k = 0; k < std::max(planes, 1u); k++) {
        const uint64_t va = buffer.va + offset + k * image;
        gm20b::Texture target;
        gm20b::shape(w, h, kRgba16Uint, target);
        target.va = va;
        target.pitch = uint32_t(row);
        target.storage = {va, row * (h - 1) + uint64_t(w) * target.bpp, buffer.hostVisible, buffer.span.storage,
                          buffer.gpuVolatile};
        // rows the render hardware takes as a pitch target are drawn in place, any other through a block-linear backing
        // the copy engine fills from them and drains back (Texture::shadowTarget)
        const bool linear = !((va | row) & 127);
        target.renderable = linear;
        target.shadowTarget = !linear;
        gm20b::Texture *scratch = scratchOf(rc, be, (__bridge const void *)b, w, h, why);
        if (!scratch)
            return false;
        be->copyBufferTexels(va, uint32_t(row), *scratch, 0, 0, 0, w, h, true);
        if (!drawClamped(rc, be, state, target, *scratch, 0, 0, w, h, why))
            return false;
    }
    return true;
}
