// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

// The host imports a texture's memory lies in, named to the submission (raw::CommandBuffer::use): a surface's or a planar
// texture's planes, a buffer texture's buffer's, a view's parent's; and a YCbCr texture's planes, which only its conversion
// reads (its image is pool memory).
// The command buffer holds every object an encode reaches (RMCommandBuffer retain:), so the encode path retains none
__attribute__((objc_externally_retained)) void useTexture(raw::CommandBuffer *rc, RMTexture *texture, bool write)
{
    __unsafe_unretained RMTexture *t = texture;
    while (t && t->_kind == RMTextureView && t->_parent)
        t = t->_parent;
    if (!t)
        return;
#if SWITCH_RAW_DIAGNOSTICS
    t->_raw.probeWritten |= write;
#endif
    rc->use(t->_raw, write);
    if (t->_buffer)
        rc->use(t->_buffer->_raw, write);
    for (const raw::Texture *plane : {t->_raw.luma, t->_raw.chroma})
        if (plane)
            rc->use(*plane, write && t->_raw.xrPlanes);
}

__attribute__((objc_externally_retained)) void useBuffer(raw::CommandBuffer *rc, RMBuffer *b, bool write)
{
    if (b)
        rc->use(b->_raw, write);
}
