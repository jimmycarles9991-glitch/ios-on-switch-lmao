// SPDX-License-Identifier: MIT
// raw/fx: a 3D texture's memory as Mesa NIL lays out a 3D image on Tegra (nil/image.rs Image::new_tiled for ImageDim::_3D,
// nil/tiling.rs Tiling::choose and clamp, nil/copy.rs's block order). The raw path's writes, m2v's twin over the same memory
// (NVK's optimal image of the descriptor) and the texture unit (its header's GOBS_PER_BLOCK_DEPTH) all address it so.
//
// A 3D image's blocks are one GOB wide and one GOB tall (Tiling::choose sets no block height for _3D) and 1 << zLog2 planes
// deep, zLog2 chosen by depth and clamped to each level. An image made 2D-view compatible (VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE:
// m2v makes a 3D render target so, image_plan.rs render_3d_flags) keeps one plane a block. Blocks run along a row, then
// down the plane's rows of blocks, then through the depth; inside a block the GOBs run down, then through its planes.
#pragma once
#include <cstddef>
#include <cstdint>
#include "fx_mip.h"

namespace raw {
namespace fx {

// NIL's layout of a 3D colour image of `levels` levels; false: not one (a size of 0 or past 16384 on a side, levels 0 or more
// than kMaxLevels or than the largest side's full chain, bpp not 1/2/4/8/16)
bool volumeLayout(uint32_t width, uint32_t height, uint32_t depth, uint32_t levels, uint32_t bpp, bool view2d,
                  MipChain &out);

// NIL's layout of a 2D array of `layers` layers (a cube's six faces, a cube array's six a cube), each a 2D image of `levels`
// levels (mipChainLayout's), `arrayStride` apart
bool arrayLayout(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t bpp, MipChain &out);

// NIL's layout of a multisampled 2D colour image of one level: its samples as a 2D image of the sample grid NIL's sample layout
// gives (2: 2X1_D3D, 4: 2X2, 8: 4X2_D3D), its block height chosen by the height in pixels; false: not 1, 2, 4 or 8 samples
bool msLayout(uint32_t width, uint32_t height, uint32_t samples, uint32_t bpp, MipChain &out);

// Byte offset of texel byte (xB, y) of plane z in level `l`, from the image's base, in the CPU's view (TegraColor GOBs)
uint64_t volumeOffset(const MipChain &c, uint32_t l, uint32_t xB, uint32_t y, uint32_t z);

// The CPU's copy of a w x h x d box at (x, y, z) of level `l` between the image's memory and rows `rowBytes`, planes
// `planeBytes` apart (replaceRegion/getBytes of a shared 3D texture); the caller cleans or invalidates the lines
void writeVolume(const MipChain &c, uint8_t *cpu, uint32_t l, const uint32_t origin[3], const uint32_t size[3],
                 const uint8_t *src, size_t rowBytes, size_t planeBytes);
void readVolume(const MipChain &c, const uint8_t *cpu, uint32_t l, const uint32_t origin[3], const uint32_t size[3],
                uint8_t *dst, size_t rowBytes, size_t planeBytes);

}  // namespace fx
}  // namespace raw
