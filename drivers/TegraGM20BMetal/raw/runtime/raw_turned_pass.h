// SPDX-License-Identifier: MIT
// A turned pass whose attachments passRotation takes once their texels are moved (raw_rotated_pass.h turnablePass, raw_turn_copy.h).
// The pass draws in the turn and frame of its first turned colour attachment (rotation 4 as rotation 1, which stores alike).
// Before it: each rotation-4 plane of that frame the pass loads is mirrored across its width and drawn as rotation 1;
// every other attachment whose texels matter (loaded or stored, or turned another way) moves into a plane-shaped stand-in the
// pass renders turned, its texels read in through its own load map where it is loaded. After it: each stored stand-in goes
// back through the attachment's store map, over the render area alone (the intersection of every attachment's frame).
// Metal loads (transposed) and stores only the tiles a draw reaches (raw_reach.h), so only those are mirrored, by a copy through
// the pass's reach mask; a rotation-4 attachment of another frame comes into its stand-in transposed in them alone.
#pragma once
#include <deque>
#include <vector>
#include "raw_gpu.h"
#include "raw_rotated_pass.h"

namespace raw {

class TurnedPass {
public:
    // `p` as the pass draws it, the copies before it encoded; false: *why (rc's to abandon)
    bool begin(CommandBuffer &rc, PassDesc &p, DrawReplay &draws, const char **why);
    // The stored stand-ins back, after the pass and its resolves
    bool end(CommandBuffer &rc, const char **why);

private:
    struct Back {
        const Texture *into;
        Texture *standIn;
        TurnMap map;
    };

    Texture *standIn(CommandBuffer &rc, const Texture &t, uint32_t tag, const char **why);
    // `t` (loaded: through `load`) into a stand-in held for the pass, put back through `store` where it is stored
    // `reached`: then through that map in the tiles the pass's draws reach
    Texture *moved(CommandBuffer &rc, Texture &t, uint32_t tag, const TurnMap *load, const TurnMap *store,
                   const char **why, const TurnMap *reached = nullptr);
    bool mirrored(CommandBuffer &rc, Texture &t, uint32_t tag, const char **why);
    bool clearAhead(CommandBuffer &rc, const ColorAttachment &a, const Rect &area, bool draws, const char **why);

    Rotation rot_;
    Rect planeArea_;
    PassReach reach_;
    std::vector<Back> backs_;
    std::deque<Texture> held_;  // what the pass's attachments point at until it ends
};

}  // namespace raw
