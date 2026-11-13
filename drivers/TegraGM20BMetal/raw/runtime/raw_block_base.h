// SPDX-License-Identifier: MIT
// Where a no-copy block-compressed texture can be its caller's memory (Device::createBlockTextureOver). NIL aligns a tiled
// image to its level-0 tile and to at least 4096 bytes (nil image.rs Image::new_tiled: fx::MipChain::align), and an image
// read from a base off that alignment shows its blocks moved (HW Test ui.compressed_image: 8-pixel groups permuted
// [1,2,0,3]/[3,0,2,1]). The base cannot be moved: the GPU maps 4 KiB pages, so a GPU VA keeps each byte's offset in its page.
// Memory off the alignment is copied by the copy engine into a pool image of the same layout instead.
#pragma once
#include <cstdint>
#include "../fx/fx_mip.h"

namespace raw {

inline bool blockImageInPlace(uintptr_t base, const fx::MipChain &layout)
{
    return layout.align && !(base & (layout.align - 1));
}

}  // namespace raw
