// SPDX-License-Identifier: MIT
// How far a pass's clear of a colour attachment reaches when nothing after the pass can read it: one not stored (a
// DontCare store, memoryless or not; a memoryless backing kept for a later pass is stored) with one sample (a
// multisampled one may resolve, which reads it whole), in a pass that is neither fused with the next (whose draws are
// not known yet) nor another pass's clears alone. Only the pass's own draws see such an attachment, each at the pixels
// it covers, which lie in its scissor within its viewport (RenderEncoder::reached_, the box of rects_): the clear covers
// the 32-pixel tiles of that box, the render area where a draw's reach was not counted, and nothing where no draw drew
// or none reads the attachment (blends into it, writes it through a partial mask, fetches it). A stored attachment's
// Clear keeps Metal's reach: later work may read any texel it defined.
#pragma once
#include <algorithm>
#include <cstdint>
#include "bounds.h"

namespace gm20b {

constexpr uint32_t kClearTile = 32;

inline bool clearSeenOnlyByDraws(bool stored, uint32_t samples, bool fused, bool clearsAlone)
{
    return !stored && samples <= 1 && !fused && !clearsAlone;
}

// The draws' box `reached` grown by the draw rectangle `r`
inline void growReached(PixelBox &reached, const PixelBox &r)
{
    reached = reached.empty() ? r
                              : PixelBox{std::min(reached.x0, r.x0), std::min(reached.y0, r.y0),
                                         std::max(reached.x1, r.x1), std::max(reached.y1, r.y1)};
}

// The box of a w x h render area such a clear covers; empty: none
inline PixelBox drawnClearBox(const PixelBox &reached, bool drew, bool whole, bool read, uint32_t w, uint32_t h)
{
    if (!drew || !read)
        return {};
    if (whole)
        return {0, 0, w, h};
    if (reached.empty())
        return {};
    return {reached.x0 / kClearTile * kClearTile, reached.y0 / kClearTile * kClearTile,
            std::min((reached.x1 + kClearTile - 1) / kClearTile * kClearTile, w),
            std::min((reached.y1 + kClearTile - 1) / kClearTile * kClearTile, h)};
}

}  // namespace gm20b
