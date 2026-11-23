// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"
#include "reach.h"

// A render pass's draws in its command buffer's plan, from its OpRender at `pass`, as its reach pass draws them again
// (reach.h): each with its own state, applied through a cursor of its own. Nothing is read of the plan until a turned
// pass asks.
class PlanDraws final : public gm20b::DrawReplay {
public:
    PlanDraws(SwitchMetalCommandBuffer *cb, size_t pass, size_t to, gm20b::CommandBuffer &commands)
        : cb_(cb), commands_(commands), from_(pass + 1), to_(to)
    {}

    bool any() const override;
    bool atFrameResolution() const override;
    bool replay(gm20b::RenderEncoder &e, const char **why) override;

private:
    SwitchMetalCommandBuffer *cb_;
    gm20b::CommandBuffer &commands_;
    size_t from_, to_;  // to_: the command buffer's range end; the pass ends at its OpEnd before it
};

// A reach, or the work of a pass it bounds, that failed (`why`, its command buffer's `refusal`): the command buffer fails
// OutOfMemory where it had no memory for a target (`memory`: gm20b::PassReach's), else as the driver's own draw the
// encoder refused (TegraGM20BMetalPassErrors.h gm20bOwnDrawFails), whose line fail() then logs
void gm20bReachFails(SwitchMetalCommandBuffer *cb, bool ahead, const char *why, bool memory,
                     const gm20b::Refusal &refusal);
