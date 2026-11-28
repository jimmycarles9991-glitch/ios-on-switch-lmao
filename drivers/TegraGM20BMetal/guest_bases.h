// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "pack.h"

namespace gm20b {

// Whether each buffer's Metal address `pp` names is one a draw writes (RenderEncoder::guestBaseWords): two root push words
// that no other push of the record writes and no other address shares, a buffer slot, listed from the first
inline bool usableGuestBases(const PackPipeline &pp)
{
    bool ended = false;
    uint32_t written = 0;
    for (uint32_t k = 0; k < kPackGuestBases; k++) {
        const uint8_t word = pp.guestBaseWord[k];
        if (!word) {
            ended = true;
            continue;
        }
        const uint32_t words = word <= 31 ? 3u << (word - 1) : 0;
        if (ended || !words || (pp.guestBaseSlot[k] & ~kGuestBaseFragment) >= 31 || (written & words) ||
            (pp.pushMask & words))
            return false;
        written |= words;
    }
    return true;
}

}  // namespace gm20b
