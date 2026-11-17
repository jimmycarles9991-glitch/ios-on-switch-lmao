// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"

inline bool blitImage(RMTexture *texture, uint32_t level, raw::Texture &image)
{
    RMTexture *owner = texture->_levelsOwner ?: texture;
    const uint32_t absolute = texture->_levelsBase + level;
    if (absolute) {
        if (!raw::chainLevelImage(owner->_raw, absolute, image))
            return false;
    } else {
        image = owner->_raw;
    }
    image.va += uint64_t(texture->_slicesBase) * image.layerBytes;
    image.layers = texture->_raw.layers;
    image.type = texture->_raw.type;
    image.format = texture->_raw.format;
    image.stencilTemp = owner->_raw.stencilTemp;
    return true;
}
