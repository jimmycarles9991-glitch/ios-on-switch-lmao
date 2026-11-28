// SPDX-License-Identifier: MIT
// See render_encoder.h (clears: what a pass's clears reach; a fused pass's continuation; what the diagnostics read
// of what a pass reached).
#include "lock.h"
#include "clear.h"
#include "clear_box.h"
#include "attachment_lifetime.h"
#include "gpu.h"
#include "rotated_pass.h"

#include <os/log.h>
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "gm20b_gpu_internal.h"

namespace gm20b {

void RenderEncoder::continuePass(uint8_t stores, uint8_t discards)
{
    // Clear reach is a Metal encoder property, including its empty-draw behavior.
    // Later encoders must not enlarge or elide the first encoder's clear.
    if (!clearsSealed_) {
        clears(initialClears_, true);
        clearsSealed_ = true;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        pass_.color[i].store = stores & (1u << i);
        pass_.color[i].discard = discards & (1u << i);
    }
    pipeline_ = nullptr;
    clearTouchedBindings();
    invalidateDescriptors();
    setDepthStencil(nullptr);
    setStencilReference(0, 0);
    setViewport(0, 0, areaW_, areaH_, 0, 1);
    setScissor(0, 0, areaW_, areaH_);
    raster_ = {};
    depthClamp_ = false;
    clipDirty_ = true;
    setDepthBias(0, 0, 0);
    const float blend[4] = {};
    setBlendColor(blend);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.fusedPasses++);
}

bool RenderEncoder::takeStorage(uint8_t storage, uint8_t arrayed)
{
    if ((storage & ~pass_.storageMask) && !dev_->storageCompressedOk())
        return false;
    for (uint32_t j = 0; j < 8; j++)
        if ((arrayed >> j & 1) && pass_.color[j].texture && !pass_.color[j].texture->blockLinear)
            return false;
    pass_.storageMask |= storage;
    pass_.arrayedStorageMask |= arrayed;
    return true;
}

// The pass's clears, before its draws: each Clear load, and each DontCare load m2v answers with a clear (metal2vulkan
// clear_reach.rs, from Metal's measured behaviour). The pass reaches no tile without a draw call, every tile with one and a
// stored Clear-loaded attachment, else the tiles (32 x 32 pixels) its drawing draws' scissors within their viewports touch. A
// DontCare-loaded memoryless color some draw reads (or that is stored) clears to zero where the pass reaches anything; a stored
// DontCare-loaded color some draw reads (blended into, written through a partial mask, fetched) clears to zero over what the
// pass reaches: the whole render area, or the reached tiles' runs along each row of tiles. Any other DontCare load loads.
void RenderEncoder::clears(std::vector<Clear> &out, bool fused)
{
    out.clear();

    enum {
        Nothing,
        Every,
        Draws
    } reach = Nothing;

    bool clearStored = false;
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const ColorAttachment &a = pass_.color[j];
        clearStored |= a.texture && a.load == ColorAttachment::Clear && a.store && !memoryless_[j];
    }
    if (called_)
        reach = clearStored ? Every : drew_ ? Draws : Nothing;
    // the reached tiles where that is not every tile (none: every tile)
    constexpr uint32_t kTile = 32;
    const uint32_t perRow = (areaW_ + kTile - 1) / kTile, rows = (areaH_ + kTile - 1) / kTile;
    std::vector<bool> &tiles = tiles_;
    tiles.clear();
    if (reach == Draws && !whole_ && perRow && rows) {
        if (!trackRects_) {
            if (reached_.empty())
                reach = Nothing;
        } else {
            bool every = false;
            for (const auto &r : rects_)
                every |= r[0] < kTile && r[1] < kTile && (r[2] + kTile - 1) / kTile >= perRow &&
                         (r[3] + kTile - 1) / kTile >= rows;
            if (!every) {
                tiles.assign(size_t(perRow) * rows, false);
                for (const auto &r : rects_)
                    for (uint32_t ty = r[1] / kTile; ty < std::min(rows, (r[3] + kTile - 1) / kTile); ty++)
                        for (uint32_t tx = r[0] / kTile; tx < std::min(perRow, (r[2] + kTile - 1) / kTile); tx++)
                            tiles[size_t(ty) * perRow + tx] = true;
                if (std::find(tiles.begin(), tiles.end(), false) == tiles.end())
                    tiles.clear();  // every tile after all
                else if (std::find(tiles.begin(), tiles.end(), true) == tiles.end())
                    reach = Nothing;
            }
        }
    } else if (reach == Draws && !whole_ && reached_.empty()) {
        reach = Nothing;
    }
    static const float zero[4] = {};
    for (uint32_t j = 0; j < pass_.count && j < 8; j++) {
        const ColorAttachment &a = pass_.color[j];
        if (!a.texture)
            continue;
        const uint32_t w = areaW_, h = areaH_;  // NVK's clears cover the render area (nvk_CmdBeginRendering)
        if (a.load == ColorAttachment::Clear) {
            // Metal's Clear load of a stored attachment reaches past the render area: over it rounded up to whole 32-pixel
            // tiles, clipped to the attachment, in a pass with a draw call; the whole attachment in one without (m2v
            // clear_reach.rs `cleared`). Past the render area, m2v clears it by a pass of its own before this one
            // (ReachClears), which the pass then loads. A multisampled one's resolve reads those tiles too (an M4 Max
            // resolves a 64x64 Clear of a 40x24 area over [0, 64) x [0, 32), the clear's past the area: hwtests-r4
            // gpu.gm20b.resolve_codes_parts)
            uint32_t rw = w, rh = h;
            if ((a.store && !memoryless_[j]) || samples_ > 1) {
                const Rotation frame = rot_.turned() ? rot_ : frameOf(*a.texture);
                const bool called = called_ || pass_.drawn;
                rw = called ? std::min((w + kTile - 1) / kTile * kTile, frame.width) : frame.width;
                rh = called ? std::min((h + kTile - 1) / kTile * kTile, frame.height) : frame.height;
            }
            Clear c{j, 0, 0, rw, rh, a.clear};
            c.own = rw != w || rh != h;
            SWITCH_METAL_DIAGNOSTIC(dev_->stats.clears++);
            if (!c.own && (covered_ >> j & 1)) {
                SWITCH_METAL_DIAGNOSTIC(dev_->stats.clearsElided++);
                continue;
            }
            if (clearSeenOnlyByDraws(a.store, samples_, fused, pass_.drawn) && drawnClear(j, a.clear, out))
                continue;
            out.push_back(c);
            continue;
        }
        if (a.load != ColorAttachment::DontCare || reach == Nothing || (covered_ >> j & 1))
            continue;
        const bool read = readBits_ >> j & 1;
        if (memoryless_[j]) {
            const bool narrowed =
                clearSeenOnlyByDraws(a.store, samples_, fused, pass_.drawn) && drawnClear(j, zero, out);
            if (!narrowed && (read || a.store))
                out.push_back({j, 0, 0, w, h, zero});
            continue;
        }
        if (!a.store || !read)
            continue;
        if (tiles.empty()) {
            out.push_back({j, 0, 0, w, h, zero});
            continue;
        }
        for (uint32_t ty = 0; ty < rows; ty++)
            for (uint32_t tx = 0; tx < perRow;) {
                if (!tiles[size_t(ty) * perRow + tx]) {
                    tx++;
                    continue;
                }
                const uint32_t first = tx;
                while (tx < perRow && tiles[size_t(ty) * perRow + tx])
                    tx++;
                const uint32_t x0 = first * kTile, y0 = ty * kTile, x1 = std::min(tx * kTile, w),
                               y1 = std::min((ty + 1) * kTile, h);
                if (x0 < x1 && y0 < y1)
                    out.push_back({j, x0, y0, x1, y1, zero});
            }
    }
}

// A clear of colour `j` that only the pass's draws see (clear_box.h) into `out`, over their tiles' box: false where
// that is the whole render area, which the caller clears as before
bool RenderEncoder::drawnClear(uint32_t j, const float *value, std::vector<Clear> &out) const
{
    const PixelBox b = drawnClearBox(reached_, drew_, whole_, readBits_ >> j & 1, areaW_, areaH_);
    if (!b.x0 && !b.y0 && b.x1 == areaW_ && b.y1 == areaH_)
        return false;
    if (!b.empty())
        out.push_back({j, b.x0, b.y0, b.x1, b.y1, value});
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.clearsNarrowed++);
    SWITCH_METAL_DIAGNOSTIC(dev_->stats.clearPixelsLeft +=
                            uint64_t(areaW_) * areaH_ - (b.empty() ? 0 : uint64_t(b.x1 - b.x0) * (b.y1 - b.y0)));
    return true;
}

}  // namespace gm20b
