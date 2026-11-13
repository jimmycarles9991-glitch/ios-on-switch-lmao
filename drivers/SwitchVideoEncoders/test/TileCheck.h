// SPDX-License-Identifier: BSD-2-Clause-Patent
// The tile check: AVE's tile slots driven through this Mac's VideoToolbox the way CMPhoto drives them for a HEIC
// (VTTileCompressionSession, 'hvc1', 512x512 tiles, CMPhoto's session properties in one batch, its two property
// reads, PrepareToEncodeTiles, a tile at a time). A 1000x700 picture in a 2x2 grid: the edge tiles are partly outside
// it, and one tile comes as its own 512x512 copy at (0, 0). The fake engine answers H.265 jobs with a placeholder
// slice and keeps the input picture each one read, which must be the tile's region of the source, edge-replicated.
#ifndef TILE_CHECK_H
#define TILE_CHECK_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../../TegraNVENC/TegraNVENCABI.h"

// One result line; true when every tile came back as a sync sample of the tile size with the session's hvcC, from
// exactly the input it should have read, and the session's SwitchEncoderRoutes counted the tiles. `encoderID` names
// the 'hvc1' registration to use.
bool tile_check(CFStringRef encoderID, char *line, size_t size);
// The source pictures' row alignment (bytes; 256 by default, VIC's grid).
extern uint32_t tile_check_row_alignment;

// The fake engine's H.265 job: `buffers` by handle, the input read in the layout its setup names; `chroma_mode` is the
// one TegraNVENCInfo stated. 0, or -1.
int tile_check_engine(const TegraNVENCSubmit *submit, uint8_t *const *buffers, uint32_t chroma_mode);

#endif
