// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"

// The plane a texture over `s` reads: its first byte past the surface's base and its rows
void planeAt(IOSurfaceRef s, NSUInteger plane, uint64_t &offset, size_t &pitch);
// A texture buffer over a surface of no planes longer than its rows, which Metal asserts on: why, else null
const char *texelBufferRowsRefusal(MTLTextureDescriptor *d, IOSurfaceRef s);
// No texture over `s`: one line naming whose answer the nil is, or the step that failed
id<MTLTexture> unmadeSurfaceTexture(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
