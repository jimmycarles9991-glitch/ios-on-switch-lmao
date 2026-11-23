// SPDX-License-Identifier: MIT
// The 2D images an extended-range texture's stand-in is drawn through (xr.h): the pack's conversion states read one
// 2D image of codes at the fragment's pixel and write that texel's value, whatever level, layer, plane or sample it is,
// so the codes and the stand-in convert as pairs of 2D images over the same texels of each. - a one-level 2D texture,
// array, cube or cube array: its layers stacked, one image (NIL lays a one-level layer out in whole blocks, arrayStride
// its rows' bytes, so layer s's row y is row s * rows + y); - a mipmapped one: each level of each layer; - a 3D level:
// its planes, one 2D image of block height zLog2. A 3D block is one GOB tall and 1 << zLog2 planes deep, its GOBs
// running through the planes as a 2D block's run down its rows, so each texel of the level is a texel of that image, at
// the same place in both images of a pair; - a multisampled texture: its sample grid (fx::msLayout), one image. The two
// images of a pair have the texel extent and block height of each other, as NIL chooses both by texels alone; an image
// taller than a target takes is cut at whole blocks.
#pragma once
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>
#include "fx/fx_image.h"
#include "gpu.h"

namespace gm20b {

// attachmentTic's slice of a conversion part: past every slice a pass's part names, so a pass into a texture's level 0
// slice 0 and the conversion's image of every slice of level 0 keep a header each
constexpr uint32_t kXrPartSlices = 1u << 27;

struct XrPart {
    Texture codes, standIn;
    uint32_t level = 0,
             key = 0;  // its level, and kXrPartSlices + its place among the level's parts (attachmentTic's slice)
};

namespace xr_detail {

constexpr uint32_t kTallest = 16384;

struct Image {
    uint64_t va;
    uint32_t width, rows, rowBytes;
    uint8_t yLog2;
    uint32_t level;
};

inline void level(const fx::MipChain &c, uint64_t base, uint32_t l, uint32_t layer, std::vector<Image> &out)
{
    const fx::MipLevel &lv = c.level[l];
    out.push_back({base + layer * c.arrayStride + lv.offset, lv.width, lv.rows, lv.rowBytes, lv.yLog2, l});
}

// `t`'s 2D images, in order; false where it is none of the shapes above
inline bool images(const Texture &t, std::vector<Image> &out)
{
    fx::MipChain c;
    if (t.samples > 1) {
        if (!fx::msLayout(t.width, t.height, t.samples, t.bpp, c) || c.level[0].rowBytes != t.pitch)
            return false;
        out.push_back({t.va, t.width << c.sxLog2, c.level[0].rows, c.level[0].rowBytes, c.yLog2, 0});
        return true;
    }
    if (t.type == fx::kTexThreeD) {
        if (t.chain)
            c = t.chain->layout;
        else if (!fx::volumeLayout(t.width, t.height, t.depth, 1, t.bpp, t.blockDepth == 0, c))
            return false;
        const uint64_t base = t.chain ? t.chain->va : t.va;
        for (uint32_t l = 0; l < c.levels; l++) {
            const fx::MipLevel &lv = c.level[l];
            out.push_back({base + lv.offset, lv.width, lv.planes * lv.rows, lv.rowBytes, lv.zLog2, l});
        }
        return true;
    }
    if (t.chain) {
        c = t.chain->layout;
        for (uint32_t layer = 0; layer < c.layers; layer++)
            for (uint32_t l = 0; l < c.levels; l++)
                level(c, t.chain->va, l, layer, out);
        return true;
    }
    if (!t.blockLinear)
        return false;
    const uint32_t blockRows = 8u << t.blockHeight, layers = std::max(t.layers, 1u);
    const uint32_t rows = (t.height + blockRows - 1) / blockRows * blockRows;
    if (layers > 1 && t.layerBytes != uint64_t(t.pitch) * rows)
        return false;
    out.push_back({t.va, t.width, rows * layers, t.pitch, t.blockHeight, 0});
    return true;
}

// Image `i` of `t` from row `first`, `rows` rows of it, as a 2D target and header part
inline Texture part(const Texture &t, const Image &i, uint32_t first, uint32_t rows)
{
    Texture p;
    p.storage = t.storageRange();
    p.va = i.va + uint64_t(first) * i.rowBytes;
    p.width = p.texelWidth = i.width;
    p.height = p.texelHeight = rows;
    p.format = t.format;
    p.pitch = i.rowBytes;
    p.bpp = t.bpp;
    p.blockLinear = true;
    p.blockHeight = i.yLog2;
    p.layerBytes = uint64_t(i.rowBytes) * rows;
    p.type = fx::kTexTwoD;
    p.renderable = true;
    return p;
}

}  // namespace xr_detail

// The pairs `codes` converts into `standIn` through, in order; false where the two are not alike in texels
inline bool xrParts(const Texture &codes, const Texture &standIn, std::vector<XrPart> &out)
{
    using namespace xr_detail;
    std::vector<Image> from, to;
    if (!images(codes, from) || !images(standIn, to) || from.size() != to.size())
        return false;
    out.clear();
    uint32_t keys[fx::kMaxLevels];
    std::fill(std::begin(keys), std::end(keys), kXrPartSlices);
    for (size_t k = 0; k < from.size(); k++) {
        const Image &a = from[k], &b = to[k];
        if (a.width != b.width || a.rows != b.rows || a.yLog2 != b.yLog2 || a.level != b.level ||
            a.level >= fx::kMaxLevels)
            return false;
        const uint32_t blockRows = 8u << a.yLog2, step = kTallest / blockRows * blockRows;
        for (uint32_t first = 0; first < a.rows; first += step) {
            const uint32_t rows = std::min(step, a.rows - first);
            out.push_back({part(codes, a, first, rows), part(standIn, b, first, rows), a.level, keys[a.level]++});
        }
    }
    return true;
}

}  // namespace gm20b
