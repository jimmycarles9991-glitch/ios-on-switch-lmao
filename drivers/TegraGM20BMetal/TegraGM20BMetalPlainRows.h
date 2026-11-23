// SPDX-License-Identifier: MIT
// Plain-format textures over a surface plane whose rows overlap, lie under 16 bytes or run past the allocation
// (plain_rows.h)
#pragma once
#import "TegraGM20BMetalInternal.h"

// A 2D texture of a plain format over plane `plane` of a surface of planes whose rows, as an M4 Max's GPU reads them,
// overlap or lie under 16 bytes, or end past the allocation (`d` a turned one's at its plane's extent)
bool plainRowsShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane);
// At `width` x `height` (a turned one's plane's extent); false with gm20b::surfaceRefusal naming the step
bool makePlainRows(SwitchMetalTexture *t, IOSurfaceRef s, NSUInteger plane, uint32_t width, uint32_t height);
