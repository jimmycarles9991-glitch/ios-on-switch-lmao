// SPDX-License-Identifier: MIT
// Copies between RGB10A8_2P_XR10 (550) or RGB10A8_2P_XR10_sRGB (551) textures at least one of which a descriptor made,
// moved as an M4 Max moves them (xr_copy.h) on the copy engine. A descriptor's texel is its colour word and its alpha
// byte side by side (xr.h), the alpha the texel's fifth byte as Depth32Float_Stencil8's stencil is, moved to and from
// a surface's alpha plane through the same two-byte temp (gm20b::Device::stencilTemp)
#pragma once
#import "TegraGM20BMetalInternal.h"

// Whether a copy between `a` and `b` is one (OpCopyTexture's kCopyXrPlanes): both 550 or 551, one made from a descriptor,
// the other too or held in a surface's two planes
bool xrDescriptorCopy(SwitchMetalTexture *a, SwitchMetalTexture *b);
// Each descriptor side of a copy with a surface holds its temp; false: no memory for one
bool xrCopyTemps(SwitchMetalTexture *a, SwitchMetalTexture *b);
// `w` x `h` texels from (sx, sy) of slice `ss` of `s` to (dx, dy) of slice `ds` of `t`
bool copyXrPlanes(gm20b::CommandBuffer *rc, gm20b::BlitEncoder *be, const void *srcKey, const void *dstKey,
                  const void *stageKey, SwitchMetalTexture *s, uint32_t ss, uint32_t sx, uint32_t sy,
                  SwitchMetalTexture *t, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                  const char **why);
