// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_attachment_target.h"

// The texture whose chain holds `t`'s levels: t, or for a format view of all of a mipmapped texture (Device::createView
// keeps no chain) the texture it views
inline RMTexture *rawLevelsOwner(RMTexture *t)
{
    RMTexture *o = t;
    while (!o->_raw.chain && o->_kind == RMTextureView && !o->_viewPart && o->_parent)
        o = o->_parent;
    return o->_raw.chain ? o : t;
}

// Level `level` of slice `slice` (a 3D texture's plane) of `t` as the target a pass binds (raw::attachmentTarget), in t's
// format; false: no such part of a texture the raw path renders
inline bool rawAttachmentPart(RMTexture *t, uint32_t level, uint32_t slice, raw::Texture &part)
{
    RMTexture *o = rawLevelsOwner(t);
    const raw::FormatInfo *f = raw::formatInfo(t->_raw.format);
    if (!f || !f->rt || f->bpp != o->_raw.bpp || !raw::attachmentTarget(o->_raw, level, slice, part))
        return false;
    part.format = t->_raw.format;
    return true;
}

// What a pass renders as level `level` of slice `slice` of `t`: t itself where that is all of it (raw::wholeAttachment), else
// its part made into `part`, which the encoder reads until its end(), with a header of it alone where a draw may fetch it
// (`fetched`); null: no such part
inline raw::Texture *rawAttachmentTarget(RMTexture *t, uint32_t level, uint32_t slice, bool fetched, raw::Texture &part)
{
    if (raw::wholeAttachment(t->_raw, level, slice))
        return &t->_raw;
    if (!rawAttachmentPart(t, level, slice, part))
        return nullptr;
    part.tic = fetched ? gRM->raw->attachmentTic(rawLevelsOwner(t)->_raw, part, level, slice) : 0;
    return &part;
}
