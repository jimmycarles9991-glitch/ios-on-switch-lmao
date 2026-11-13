// SPDX-License-Identifier: MIT
// The header a draw binds for a view of a pass's color attachment other than the attachment's own: the storage form's
// storage image (word 0 the record's storageTic) or the input-attachment form's fetch, as m2v's runtime makes the view
// (render_target.rs attachment_view_in: the attachment's level and the pass's layers from its slice, a 2D array in a
// layered pass) and NVK heads it (nvk_image_view.c, NIL nvb097_fill_tic, MIT). A header has no base layer and no array
// pitch: its address is the first layer's, and the texture unit steps layers by the stride of the image the header
// describes. So a level of a mipmapped array is headed as the whole chain, the level its RES_VIEW range, and a 3D image's
// level as a 2D array of its planes (nil_image_3d_level_as_2d_array: that level alone, a plane a layer).
#pragma once
#include <cstdint>
#include "../fx/fx_mip.h"
#include "raw_view.h"

namespace raw {

// Level `level` of an attachment's image and `layers` of its layers from `slice`, of word 0 `word0`: a 2D array header
// where `arrayed`, else a 2D one (one layer). `part`: the header is made from the image the attachment is a level or slice
// of; else from the attachment's own header, of the attachment's one level at its address. `layers` 0 names
// Device::viewTic's view of a texture's own header in another format.
struct AttachmentView {
    uint32_t word0 = 0, slice = 0, layers = 0;
    uint8_t level = 0;
    bool srgb = false, arrayed = false, part = false;

    bool operator==(const AttachmentView &o) const
    {
        return word0 == o.word0 && slice == o.slice && layers == o.layers && level == o.level && srgb == o.srgb &&
               arrayed == o.arrayed && part == o.part;
    }
};

// The header of view `v` of image `c` at `base` (`volume`: a 3D image, its planes the layers); false: no such part of it
inline bool attachmentViewHeader(const fx::MipChain &c, uint64_t base, bool volume, const AttachmentView &v,
                                 uint32_t out[8])
{
    if (!v.layers || v.level >= c.levels)
        return false;
    fx::MipChain view = c;
    uint64_t va = base;
    uint32_t level = v.level;
    if (volume) {
        const fx::MipLevel &l = c.level[v.level];
        if (l.zLog2 || uint64_t(v.slice) + v.layers > l.depth)
            return false;
        view.levels = 1;
        view.level[0] = l;
        view.level[0].offset = 0;
        view.width = l.width;
        view.height = l.height;
        view.depth = 1;
        view.yLog2 = l.yLog2;
        view.zLog2 = 0;
        va += l.offset + uint64_t(v.slice) * l.rowBytes * l.rows;
        level = 0;
    } else if (uint64_t(v.slice) + v.layers > c.layers) {
        return false;
    } else {
        va += uint64_t(v.slice) * c.arrayStride;
    }
    view.layers = v.layers;
    fx::mipChainTic(view, va, v.word0, v.srgb, level, 1, out, v.arrayed ? fx::kTexTwoDArray : fx::kTexTwoD);
    return true;
}

// The attachment's own header `own` as view `v` of it: v's word 0 and sRGB conversion, a 2D array of v.layers layers where
// arrayed. Its own header heads one block-linear 2D level (a pass's backing, a 2D texture), whose layer stride under the
// texture unit is the attachment's (Device::backing: one level's tiles); false for any other header (a pitch one has no
// array type).
inline bool ownViewHeader(const uint32_t own[8], const AttachmentView &v, uint32_t out[8])
{
    constexpr uint32_t kBlockLinear = 3;
    const uint32_t headerVersion = own[2] >> 21 & 7, textureType = own[4] >> 23 & 15;
    if (!v.layers || headerVersion != kBlockLinear || textureType != fx::kTexTwoD)
        return false;
    viewHeader(own, v.word0, out);
    out[4] |= uint32_t(v.srgb) << 22;  // S_R_G_B_CONVERSION (bit 150)
    if (v.arrayed) {
        out[4] = (out[4] & ~(15u << 23)) | fx::kTexTwoDArray << 23;              // TEXTURE_TYPE (bits 151-154)
        out[5] = (out[5] & ~(0x3fffu << 16)) | ((v.layers - 1) & 0x3fff) << 16;  // DEPTH_MINUS_ONE (bits 176-189)
    }
    return true;
}

}  // namespace raw
