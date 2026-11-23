// SPDX-License-Identifier: MIT
// Metal's GPU copies through a BGRA10_XR or BGRA10_XR_sRGB texture it keeps compressed (fresh.h metalCompresses), into or
// out of it, from or to a buffer or a texture: an M4 Max leaves the destination's fields their 10-bit codes alone, each
// field's low 6 bits 0, and its alpha codes clamped to 384..894; the source keeps its bytes and CPU copies keep every bit
// (m2v-textures5 probes/upload.mm, where.mm; m2v-textures6 probes/srckeep.mm; m2v-textures7 probes/lowbits.mm). After the copy
// engine's copy the destination's region goes into a scratch image of the command buffer's, which one pass of the pack's
// clamp state (pack/xr.metal xr_copy_clamp_frag) draws back into it as Apple leaves it. Any other copy pays a format test.
#pragma once
#import "TegraGM20BMetalInternal.h"

// The backing tag of the clamp's scratch: past every fixed tag and the open-ended run TegraGM20BMetalXrPlanes.h kXrPlanesOverflowTag
// counts up from, so no other backing of the command buffer is it. A tag past gpu.h kBackingTags is a backing's own
// memory all the same, only not in the device's last-backing cache, which turnScratch never asks.
constexpr uint32_t kCopyClampTag = 0xffffffffu;

// Whether level `level` of `t` is one Metal keeps compressed in a BGRA10_XR or BGRA10_XR_sRGB texture
bool gm20bCompressedXr(SwitchMetalTexture *t, uint32_t level);

// A copy whose destination Apple leaves clamped: `from` (a texture, level `fromLevel`; null: a buffer) into `to` (a texture,
// level `toLevel`; null: a buffer). Both textures 2D ones of a descriptor (block-linear, one sample), as measured: a copy
// into a texture over a surface or a buffer, an array, a cube or a 3D texture is left as the copy engine leaves it
bool gm20bCopyClamps(SwitchMetalTexture *from, uint32_t fromLevel, SwitchMetalTexture *to, uint32_t toLevel);

// After the copy into `t`'s level `level`, `depth` slices (arrays) or planes (3D) from `slice` or `plane`, (x, y, w, h)
// texels: each drawn back clamped. False: no scratch, header or pass for it (*why); `be` the blit encoder encoding goes on
// with
bool gm20bClampCopiedTexture(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *&be, SwitchMetalTexture *t, uint32_t level,
                             uint32_t slice, uint32_t plane, uint32_t depth, uint32_t x, uint32_t y, uint32_t w,
                             uint32_t h, const char **why);

// After the copy into `b`, `planes` images of `h` rows of `w` texels, `row` bytes apart from `offset`, `image` bytes apart:
// each drawn back clamped
bool gm20bClampCopiedBuffer(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *&be, SwitchMetalBuffer *b, uint64_t offset,
                            uint64_t row, uint64_t image, uint32_t w, uint32_t h, uint32_t planes, const char **why);
