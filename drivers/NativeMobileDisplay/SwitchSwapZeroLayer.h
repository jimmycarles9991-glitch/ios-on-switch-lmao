// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <string.h>

namespace switch_display {

// IOMFBSwap layout read by IOMobileFramebufferLegacy::swap_submit on both 21H461 and 24B119:
// surfaceID[4] at +0x9c, enabled layer mask at +0x14c, swap type at +0x17c (9 uses an internal surface).
// Core Animation unconditionally sets enabled bit (layer == 0 ? 2 : 0) with surfaceID 0 on every detached-plane
// frame, causing Apple's swap_submit to call IOSurfaceRoot::lookupSurface(root, 0) on every frame.
// Eliding bits for layers already known off preserves swapEffectiveSurfaces identically while avoiding lookupSurface(0).
struct SwapZeroLayerState {
    static constexpr unsigned kSwapSurfaceIdsOffset = 0x9c;
    static constexpr unsigned kSwapEnabledOffset = 0x14c;
    static constexpr unsigned kSwapTypeOffset = 0x17c;
    static constexpr uint32_t kSwapTypeInternal = 9;
    static constexpr unsigned kLayers = 4;
    static constexpr uint32_t kLayerMask = 0xfu;

    struct Call {
        uint32_t origEnabled = 0;
        uint32_t elided = 0;
        uint32_t activating = 0;
        uint32_t disabling = 0;
        uint32_t preGen[kLayers] = {};
        bool special = false;
    };

    // Packed lock-free state: [3:0] activeMask, [19:4] disablingCount[4] (4b each), [63:20] gen[4] (11b each).
    // Steady-state frames where active layers stay active and no disable is in flight take zero atomic writes.
    uint64_t state = kLayerMask;

    static constexpr uint32_t activeOf(uint64_t s)
    {
        return static_cast<uint32_t>(s & kLayerMask);
    }

    static constexpr uint32_t disablingOf(uint64_t s, unsigned i)
    {
        return static_cast<uint32_t>((s >> (4u + 4u * i)) & 0xfu);
    }

    static constexpr uint64_t withDisabling(uint64_t s, unsigned i, uint32_t count)
    {
        const unsigned shift = 4u + 4u * i;
        return (s & ~(uint64_t{0xfu} << shift)) | (uint64_t{count & 0xfu} << shift);
    }

    static constexpr uint32_t genOf(uint64_t s, unsigned i)
    {
        return static_cast<uint32_t>((s >> (20u + 11u * i)) & 0x7ffu);
    }

    static constexpr uint64_t bumpGen(uint64_t s, unsigned i)
    {
        const unsigned shift = 20u + 11u * i;
        const uint32_t next = (genOf(s, i) + 1u) & 0x7ffu;
        return (s & ~(uint64_t{0x7ffu} << shift)) | (uint64_t{next} << shift);
    }

    Call begin(unsigned char *swap)
    {
        Call c;
        if (!swap)
            return c;
        uint32_t swapType = 0;
        memcpy(&c.origEnabled, swap + kSwapEnabledOffset, sizeof(c.origEnabled));
        memcpy(&swapType, swap + kSwapTypeOffset, sizeof(swapType));
        if (swapType == kSwapTypeInternal) {
            c.special = true;
            reset();
            return c;
        }
        uint32_t surfaceIds[kLayers] = {};
        memcpy(surfaceIds, swap + kSwapSurfaceIdsOffset, sizeof(surfaceIds));
        uint32_t activating = 0;
        uint32_t zeroMask = 0;
        for (unsigned i = 0; i < kLayers; ++i) {
            const uint32_t bit = 1u << i;
            if ((c.origEnabled & bit) == 0)
                continue;
            if (surfaceIds[i] != 0)
                activating |= bit;
            else
                zeroMask |= bit;
        }
        c.activating = activating;
        uint64_t s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
        for (;;) {
            const uint32_t active = activeOf(s);
            uint32_t elided = zeroMask & ~active;
            uint32_t disabling = zeroMask & active;
            if (c.origEnabled != 0 && (c.origEnabled & ~elided) == 0) {
                disabling |= elided;
                elided = 0;
            }
            bool needMutate = disabling != 0 || (activating & ~active) != 0;
            for (unsigned i = 0; !needMutate && i < kLayers; ++i) {
                if ((activating & (1u << i)) && disablingOf(s, i) != 0)
                    needMutate = true;
            }
            if (!needMutate) {
                c.elided = elided;
                c.disabling = 0;
                break;
            }
            uint64_t next = s | activating;
            for (unsigned i = 0; i < kLayers; ++i) {
                const uint32_t bit = 1u << i;
                if ((activating & bit) && ((active & bit) == 0 || disablingOf(s, i) != 0))
                    next = bumpGen(next, i);
            }
            uint32_t trackedDisabling = 0;
            for (unsigned i = 0; i < kLayers; ++i) {
                const uint32_t bit = 1u << i;
                if ((disabling & bit) == 0)
                    continue;
                const uint32_t cnt = disablingOf(s, i);
                if (cnt < 0xfu) {
                    next = withDisabling(next, i, cnt + 1u);
                    c.preGen[i] = genOf(next, i);
                    trackedDisabling |= bit;
                }
            }
            if (__atomic_compare_exchange_n(&state, &s, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                c.elided = elided;
                c.disabling = trackedDisabling;
                break;
            }
        }
        if (c.elided != 0) {
            const uint32_t filtered = c.origEnabled & ~c.elided;
            memcpy(swap + kSwapEnabledOffset, &filtered, sizeof(filtered));
        }
        return c;
    }

    void finish(unsigned char *swap, const Call &c, bool succeeded)
    {
        if (swap && c.elided != 0)
            memcpy(swap + kSwapEnabledOffset, &c.origEnabled, sizeof(c.origEnabled));
        if (c.special) {
            reset();
            return;
        }
        uint64_t s = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
        for (;;) {
            bool needMutate = c.disabling != 0 || (c.activating & ~activeOf(s)) != 0;
            for (unsigned i = 0; !needMutate && i < kLayers; ++i) {
                if ((c.activating & (1u << i)) && disablingOf(s, i) != 0)
                    needMutate = true;
            }
            if (!needMutate)
                return;
            uint64_t next = s;
            for (unsigned i = 0; i < kLayers; ++i) {
                const uint32_t bit = 1u << i;
                if ((c.activating & bit) && ((activeOf(s) & bit) == 0 || disablingOf(s, i) != 0)) {
                    next |= bit;
                    next = bumpGen(next, i);
                }
            }
            for (unsigned i = 0; i < kLayers; ++i) {
                const uint32_t bit = 1u << i;
                if ((c.disabling & bit) == 0)
                    continue;
                const uint32_t cnt = disablingOf(next, i);
                if (cnt > 0)
                    next = withDisabling(next, i, cnt - 1u);
                if (succeeded && genOf(next, i) == c.preGen[i])
                    next &= ~bit;
            }
            if (__atomic_compare_exchange_n(&state, &s, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return;
        }
    }

    void reset()
    {
        uint64_t s = __atomic_load_n(&state, __ATOMIC_RELAXED);
        for (;;) {
            uint64_t next = s | kLayerMask;
            for (unsigned i = 0; i < kLayers; ++i)
                next = bumpGen(next, i);
            if (__atomic_compare_exchange_n(&state, &s, next, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                return;
        }
    }
};

}  // namespace switch_display
