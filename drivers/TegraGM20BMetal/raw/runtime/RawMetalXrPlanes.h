// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 textures over a two-plane surface on the raw path, drawn and sampled in their planes (raw_xr_pass.h,
// raw_xr_texture.cpp)
#pragma once
#import "RawMetalInternal.h"

// 550 over plane 0 of a two-plane surface of a 4-byte and a 1-byte plane, both the descriptor's size: a 2D texture, one level,
// one sample, no rotation, rendered or sampled
bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
bool makeXrPlanes(RMTexture *t, IOSurfaceRef s);
