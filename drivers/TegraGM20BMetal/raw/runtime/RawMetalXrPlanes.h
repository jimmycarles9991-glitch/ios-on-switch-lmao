// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 textures over a two-plane surface on the raw path, drawn and sampled in their planes (raw_xr_pass.h,
// raw_xr_texture.cpp)
#pragma once
#import "RawMetalInternal.h"

// 550 over plane 0 of a two-plane surface of a 4-byte and a 1-byte plane, both the descriptor's size: a 2D texture, one level,
// one sample, no rotation, rendered, sampled, or read and written by kernels
bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
bool makeXrPlanes(RMTexture *t, IOSurfaceRef s);
// A pass that stores into `t` (held in its planes) through an RG32Uint image of its texels: the image, a block-linear scratch of
// the command buffer's (backing tag kXrPlanesImageTag + k), merged from the planes first; after the pass, the image split back
// into them. 0, or 1 a pass the raw path cannot render, 2 a draw the encoder refused (*why)
constexpr uint32_t kXrPlanesImageTag = 104;
int mergeXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, uint32_t k, raw::Texture *&image, const char **why);
int splitXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, const raw::Texture &image, const char **why);
