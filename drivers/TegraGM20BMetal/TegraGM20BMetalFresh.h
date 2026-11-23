// SPDX-License-Identifier: MIT
// A new Private texture of the driver's own memory starts as Apple's GPUs start one (fresh.h), written by the copy
// engine on the raw channel ahead of the first submission that may reach it (fresh_queue.h). Stale pool memory never
// shows through.
#pragma once
#import "TegraGM20BMetalInternal.h"
#include <vector>

void establishFresh(SwitchMetalTexture *t, MTLTextureDescriptor *d);
// Whether establishFresh takes `t`: a Private texture of the driver's own pool memory, written to its first contents
// ahead of its first use
bool freshFills(SwitchMetalTexture *t, MTLTextureDescriptor *d);

// The fresh textures a raw submission writes ahead of its commands: made after the submission began and before its first
// command, kept by the command buffer until it completes once submitted() says it went through, else given back.
class FreshWrites {
public:
    FreshWrites(SwitchMetalCommandBuffer *cb, size_t from, size_t to, gm20b::CommandBuffer &rc);
    FreshWrites() = default;
    FreshWrites(const FreshWrites &) = delete;
    FreshWrites &operator=(const FreshWrites &) = delete;
    ~FreshWrites();
    // An encode ahead's encoder [from, to): the fresh textures it names first taken; whether one of them needs its contents
    // written (written(): in a blit of its own, after the encoder before it ended and ahead of this one)
    bool claim(SwitchMetalCommandBuffer *cb, size_t from, size_t to);
    void written(gm20b::CommandBuffer &rc);
    void submitted(SwitchMetalCommandBuffer *cb);

private:
    std::vector<id> handled_, pending_;
    bool submitted_ = false;
};

// Before a CPU transfer into or out of `t` (replaceRegion, getBytes): a fresh texture's first contents written now, in a
// raw submission the transfer follows (its job names the resource set), not by a later one over what the transfer wrote;
// false when that submission failed (the texture stays fresh)
bool freshBeforeTransfer(SwitchMetalTexture *t);

#if SWITCH_METAL_DIAGNOSTICS
// The encoder at `begin`: a fresh texture it reads before any command wrote it is counted and logged with the encoder that
// wrote it first (a producer encoded after its consumer)
void traceFreshUses(SwitchMetalCommandBuffer *cb, size_t begin);
#endif
