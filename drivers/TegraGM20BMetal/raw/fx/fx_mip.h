// SPDX-License-Identifier: MIT
// raw/fx (r12-cafx): GPU mip chains for the raw GM20B path. Core Animation minifies icons, switcher cards and web tiles through
// mipmapped BGRA8 textures (render or copy into level 0, generateMipmapsForTexture:, sample with a mip filter); the raw path
// refuses every command buffer that touches one (tex.shape, pass.fmt, blit.tex, mipmaps, enc.colorfmt) and m2v replays it whole
// (15-88 ms each; 21.9 s of the 32.5 s replayed in the r7p6 UI run). This module is what the raw path needs to keep them:
//
//  - the chain's layout: a block-linear 2D colour image of N levels exactly as Mesa NIL lays it out on Tegra (nil/image.rs
//    Image::new_tiled, nil/tiling.rs Tiling::choose/clamp, GOBType::TegraColor), because the texture unit derives every level's
//    address from level 0's tiling by the same rule and the writer must put each level where it will read;
//  - its texture header (TEXHEAD_BL, nil/descriptor.rs nvb097_fill_image_view_desc);
//  - the FERMI_TWOD_A (0x902D, subchannel 3 of every channel the kext makes) methods that fill it: level 0 converted from the
//    raw path's pitch texture (1:1, point), then each level from the one above (bilinear, sample origin at pixel centres: a 2:1
//    step samples exactly between two texels, the 2x2 box filter m2v's vkCmdBlitImage(LINEAR) chain gives), with the waits and
//    texture-cache invalidates between dependent steps, all on the GPU (no CPU wait, nothing global);
//  - a self-test pattern the device runs once (Mesa nvc0 routes scaled blits to the 3D engine with "FIXME: find correct src
//    coordinates adjustments", so the 2D engine's scaled sample position is checked on the hardware before chains are used).
//
// Memory: a chain must be bound with PTE kind kBlockLinearKind (0xFE, GENERIC_16BX2: NIL nvc0_choose_pte_kind for colour; r3l
// showed block-linear through a kind-0 mapping scrambles each GOB's interior). The CPU sees that memory in TegraColor GOB order
// (nil/copy.rs CopyGOBTegra), which gobOffset() gives.
//
// Nothing here allocates, submits or keeps state: rawca's raw_gpu/RawMetal own memory, rings and lifetimes; these functions
// compute layouts and headers and write push words into a raw::Push the caller reserved (the *Words() bounds).
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>  // raw_push.h uses it without including it
#include "../runtime/raw_push.h"
#include "TegraGM20BLayout.h"

namespace raw {
namespace fx {

constexpr uint32_t kMaxLevels = 15;                     // TEXHEAD_BL MAX_MIP_LEVEL is 4 bits; 16384 needs 15
constexpr uint32_t kBlockLinearKind = TGM_LAYOUT_KIND;  // GMMU PTE kind GENERIC_16BX2 for the chain's memory
constexpr uint32_t kChainAlign = 4096;                  // NIL Image::new_tiled: align_B = max(level-0 tile bytes, 4096)

struct MipLevel {
    uint64_t offset = 0;             // from the chain's base
    uint32_t width = 0, height = 0;  // texels: max(1, w >> level), max(1, h >> level)
    uint32_t rowBytes = 0,
             rows = 0;  // the level's extent aligned to its tile (row stride; rows, a multiple of 8 << yLog2)
    uint8_t yLog2 = 0;  // the level's block height in GOBs, log2 (level 0's clamped to the level, as hardware does)
};

struct MipChain {
    uint32_t width = 0, height = 0, levels = 0, bpp = 0;
    uint8_t yLog2 =
        0;  // level 0's: the texture header's GOBS_PER_BLOCK_HEIGHT and the 2D engine's block size of level 0
    uint64_t bytes = 0, align = 0;
    MipLevel level[kMaxLevels];
};

// The number of levels of a full chain (Metal's mipmapLevelCount for a full chain): floor(log2(max(w, h))) + 1
uint32_t fullLevels(uint32_t width, uint32_t height);
// NIL's layout of a 2D, one-sample, one-layer colour image of `levels` levels and `bpp` bytes a texel; false: not one (levels 0
// or more than kMaxLevels or more than fullLevels, a size of 0 or over 16384, bpp not 1/2/4/8/16)
bool mipChainLayout(uint32_t width, uint32_t height, uint32_t levels, uint32_t bpp, MipChain &out);
// Byte offset of texel byte (xB, y) inside level `l` of the chain, from the chain's base, in the CPU's view (TegraColor GOBs;
// `fermi`: the FermiColor arrangement instead, the desktop parts'; the device's CPU view of kind 0xFE memory is TegraColor)
uint64_t texelOffset(const MipChain &c, uint32_t l, uint32_t xB, uint32_t y, bool fermi = false);

// The CPU's copy of `w` x `h` texels at (x, y) of level `l` between the chain's memory (`chainCpu`, in the view `fermi` names)
// and rows `rowBytes` apart: a Shared mipmapped texture's replaceRegion/getBytes of a level below the first. Moves each
// GOB line's 16-byte run at once; the caller cleans or invalidates the lines.
void writeLevel(const MipChain &c, uint8_t *chainCpu, uint32_t l, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                const uint8_t *src, size_t rowBytes, bool fermi = false);
void readLevel(const MipChain &c, const uint8_t *chainCpu, uint32_t l, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               uint8_t *dst, size_t rowBytes, bool fermi = false);

// Byte offset inside one GOB (64 B x 8 rows) of byte xB % 64 of row y % 8, TegraColor (nil/copy.rs CopyGOBTegra)
inline uint32_t gobOffset(uint32_t xB, uint32_t y)
{
    return tgm_layout_gob_offset(xB, y);
}

// The same for FermiColor GOBs (nil/copy.rs CopyGOBFermi: the desktop parts' sector order)
uint32_t gobOffsetFermi(uint32_t xB, uint32_t y);

// The texture header of the chain at GPU address `va` (512-byte aligned): word 0 from `tic0` (the format's components, data
// types and swizzle: raw::formatInfo()->tic0), a view of levels [baseLevel, baseLevel + levelCount). NIL nvb097 for a 2D view.
void mipChainTic(const MipChain &c, uint64_t va, uint32_t tic0, bool srgb, uint32_t baseLevel, uint32_t levelCount,
                 uint32_t out[8]);

// FERMI_TWOD_A surface format of an MTLPixelFormat the 2D engine filters faithfully (cl902d.h SET_SRC_FORMAT), 0: none (a
// chain of that format is kept, and its generateMipmaps is not the 2D engine's)
uint32_t twodFormat(uint32_t mtlPixelFormat);

// A pitch-linear surface of the raw path (a raw::Texture: va, pitch, width, height)
struct Pitch {
    uint64_t va = 0;
    uint32_t pitch = 0, width = 0, height = 0;
};

// Once per channel, before its first 2D work: Mesa nvc0_screen's 2D init (operation SRCCOPY, no clip, no colour key, corral
// 0x3f, safe overlap, render enable always, all processing clusters).
size_t twodInitWords();
void emitTwodInit(Push &p);

// generateMipmaps: levels (firstLevel, lastLevel] of the chain at `chainVa` from the level above each, after (when `level0`)
// converting the pitch texture into chain level 0 first (firstLevel must then be 0). Starts with a 3D wait for idle (the work
// before it wrote level 0) and ends with one plus a 3D texture data cache invalidate (the draws after it sample the chain).
// How a scaled (bilinear) step asks the 2D engine to sample: the origin mode and the source origin SRC_X0/Y0 = halfDu x du/2 +
// halfTexel x 1/2 (each axis with its own du/dv).
struct Sampling {
    uint8_t corner = 0;    // SAMPLE_MODE ORIGIN: 0 centre, 1 corner
    int8_t halfDu = 0;     // the origin's du/2 multiples
    int8_t halfTexel = 0;  // and its half-texel multiples
};

// GM20B's: corner origin, the source origin at half a step (texel x samples at (x + 1/2) du). The class's documented centre
// origin (sampling at SRC_X0 + (x + 1/2) du) is not what the engine does: the device's own sweep of eight candidates, in the
// render server at every boot that ran it (11-05 11:02 and 11:33, 11-06 21:40, 23:16, 23:23 and 23:43), found 287 bytes of 304
// below level 0 wrong under it, and none under this one (raw/fx/test/mipcheck.cpp holds the sweep's counts to the 2D engine
// model, twod_sim's quirkCentreHalfTexel)
constexpr Sampling kGm20bSampling = {1, 1, 0};

size_t generateWords(const MipChain &c, uint32_t firstLevel, uint32_t lastLevel);
void emitGenerate(Push &p, const MipChain &c, uint64_t chainVa, const Pitch *level0, uint32_t fmt2d,
                  uint32_t firstLevel, uint32_t lastLevel, Sampling sampling = kGm20bSampling);

// A 1:1 copy of [sx, sx + w) x [sy, sy + h) of a pitch surface into level `level` of the chain at (dx, dy): a copy into a level
// (copyFromTexture/copyFromBuffer ... destinationLevel:), with the same waits as emitGenerate.
size_t copyWords();
void emitCopyToLevel(Push &p, const MipChain &c, uint64_t chainVa, uint32_t level, const Pitch &src, uint32_t fmt2d,
                     uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);

// The CPU reference of one chain step: level n+1 from level n exactly as the 2D engine's bilinear blit samples under
// kGm20bSampling (u = (x + 0.5) * w_n / w_{n+1} - 0.5, clamp to edge, unorm8 per component, weights at 1/256): the host
// tests' oracle.
void referenceDownsample(const uint8_t *src, uint32_t srcPitch, uint32_t sw, uint32_t sh, uint8_t *dst,
                         uint32_t dstPitch, uint32_t dw, uint32_t dh, uint32_t bpp);

}  // namespace fx
}  // namespace raw
