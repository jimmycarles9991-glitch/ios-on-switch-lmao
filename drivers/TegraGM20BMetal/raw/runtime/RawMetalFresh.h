// SPDX-License-Identifier: MIT
// A new Private texture of the raw path's own memory starts as Apple's GPUs start one (raw_fresh.h), written by the copy
// engine on the raw channel ahead of the first submission that may reach it (raw_fresh_queue.h). Stale pool memory never
// shows through.
#pragma once
#import "RawMetalInternal.h"
#include <vector>

void establishFresh(RMTexture *t, MTLTextureDescriptor *d);

// The fresh textures a raw submission writes ahead of its commands: made after the submission began and before its first
// command, kept by the command buffer until it completes once submitted() says it went through, else given back.
class FreshWrites {
public:
    FreshWrites(RMCommandBuffer *cb, size_t from, size_t to, raw::CommandBuffer &rc);
    FreshWrites() = default;
    FreshWrites(const FreshWrites &) = delete;
    FreshWrites &operator=(const FreshWrites &) = delete;
    ~FreshWrites();
    // An encode ahead's encoder [from, to): the fresh textures it names first taken; whether one of them needs its contents
    // written (written(): in a blit of its own, after the encoder before it ended and ahead of this one)
    bool claim(RMCommandBuffer *cb, size_t from, size_t to);
    void written(raw::CommandBuffer &rc);
    void submitted(RMCommandBuffer *cb);

private:
    std::vector<id> handled_, pending_;
    bool submitted_ = false;
};

// Ahead of a submission of the other driver: every fresh texture written by a raw submission of its own, which that
// submission is then ordered after (RawMetalSubmit.mm); false when one is pending and the raw path could not submit it
bool writeFreshAhead(RMCommandBuffer *cb, size_t from, size_t to);

// Before a CPU transfer into or out of `t` (replaceRegion, getBytes): a fresh texture's first contents written now, in a
// raw submission the transfer follows (its job names the resource set), not by a later one over what the transfer wrote;
// false when that submission failed (the texture stays fresh)
bool freshBeforeTransfer(RMTexture *t);

#if SWITCH_RAW_DIAGNOSTICS
// The encoder at `begin`: a fresh texture it reads before any command wrote it is counted and logged with the encoder that
// wrote it first (a producer encoded after its consumer)
void traceFreshUses(RMCommandBuffer *cb, size_t begin);
#endif
