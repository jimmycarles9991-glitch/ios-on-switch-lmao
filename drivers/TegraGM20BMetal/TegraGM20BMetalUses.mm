// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"

// The host imports a texture's memory lies in, named to the submission (gm20b::CommandBuffer::use): a surface's or a planar
// texture's planes, a buffer texture's buffer's, a view's parent's; and a YCbCr texture's planes, which only its conversion
// reads (its image is pool memory).
// The command buffer holds every object an encode reaches (SwitchMetalCommandBuffer retain:), so the encode path retains none
__attribute__((objc_externally_retained)) void useTexture(gm20b::CommandBuffer *rc, SwitchMetalTexture *texture,
                                                          bool write)
{
    __unsafe_unretained SwitchMetalTexture *t = texture;
    if (t && t->_levelsOwner && !t->_ownForm)
        t = t->_levelsOwner;
    if (!t)
        return;
#if SWITCH_METAL_DIAGNOSTICS
    t->_raw.probeWritten |= write;
#endif
    rc->use(t->_raw, write);
    if (t->_buffer)
        rc->use(t->_buffer->_raw, write);
    for (const gm20b::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        if (plane)
            rc->use(*plane, write && t->_raw.xrPlanes);
}

__attribute__((objc_externally_retained)) void useBuffer(gm20b::CommandBuffer *rc, SwitchMetalBuffer *b, bool write)
{
    if (b)
        rc->use(b->_raw, write);
}
