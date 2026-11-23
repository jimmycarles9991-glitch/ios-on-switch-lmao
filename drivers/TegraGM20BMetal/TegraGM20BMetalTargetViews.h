// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "attachment_target.h"

// The texture whose chain holds `t`'s levels: t, or for a format view of all of a mipmapped texture (Device::createView
// keeps no chain) the texture it views
inline SwitchMetalTexture *gm20bLevelsOwner(SwitchMetalTexture *t)
{
    SwitchMetalTexture *o = t;
    while (!o->_raw.chain && o->_kind == SwitchMetalTextureView && !o->_viewPart && o->_parent)
        o = o->_parent;
    return o->_raw.chain ? o : t;
}

// The texture whose chain holds level `level` of `t`, `level` and `slice` made its: t's levels owner, or for a level past
// the first of a view of several levels (whose own image is its first level's) the texture it views, from the view's base
// level and slice
inline SwitchMetalTexture *gm20bPartOwner(SwitchMetalTexture *t, uint32_t &level, uint32_t &slice)
{
    if (level && t->_viewPart && t->_levelsOwner && t->_levelsOwner->_raw.chain) {
        level += t->_levelsBase;
        slice += t->_slicesBase;
        return t->_levelsOwner;
    }
    return gm20bLevelsOwner(t);
}

// Level `level` of slice `slice` (a 3D texture's plane) of `t` as the target a pass binds (gm20b::attachmentTarget), in t's
// format; false: no such part of a texture the driver renders
inline bool gm20bAttachmentPart(SwitchMetalTexture *t, uint32_t level, uint32_t slice, gm20b::Texture &part)
{
    SwitchMetalTexture *o = gm20bPartOwner(t, level, slice);
    const gm20b::FormatInfo *f = gm20b::formatInfo(t->_raw.format);
    if (!f || !(f->rt || gm20b::formatStandIn(t->_raw.format)) || f->bpp != o->_raw.bpp ||
        !gm20b::attachmentTarget(o->_raw, level, slice, part))
        return false;
    part.format = t->_raw.format;
    return true;
}

// What a pass renders as level `level` of slice `slice` of `t`: t itself where that is all of it (gm20b::wholeAttachment), else
// its part made into `part`, which the encoder reads until its end(), with a header of it alone where a draw may fetch it
// (`fetched`); null: no such part
inline gm20b::Texture *gm20bAttachmentTarget(SwitchMetalTexture *t, uint32_t level, uint32_t slice, bool fetched,
                                             gm20b::Texture &part)
{
    if (gm20b::wholeAttachment(t->_raw, level, slice))
        return &t->_raw;
    if (!gm20bAttachmentPart(t, level, slice, part))
        return nullptr;
    SwitchMetalTexture *o = gm20bPartOwner(t, level, slice);
    part.tic = fetched ? gRM->raw->attachmentTic(o->_raw, part, level, slice) : 0;
    return &part;
}
