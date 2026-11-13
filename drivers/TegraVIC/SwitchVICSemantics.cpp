// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchVICSemantics.h"
#include "SwitchVICQueue.h"
#include "../TegraHost1x/SwitchHost1xSemantics.h"
using Timeline = SwitchHost1xSemantics<SWITCH_GPU_ENGINE_VIC, SwitchVICQueue::kSlots>;

bool switch_vic_semantics_start(uint32_t id, void (*retired)(switch_gpu_event_waits *))
{
    return Timeline::start(id, retired);
}

bool switch_vic_semantics_pending()
{
    return Timeline::hasPendingWork();
}

int switch_vic_semantics_bind(switch_gpu_event_waits *batch, const switch_gpu_backing_token *token,
                              SwitchVICSemantic **out)
{
    Timeline::Node *node = nullptr;
    const int result = Timeline::bind(batch, token, &node);
    *out = reinterpret_cast<SwitchVICSemantic *>(node);
    return result;
}

void switch_vic_semantics_commit(SwitchVICSemantic *node)
{
    Timeline::commit(reinterpret_cast<Timeline::Node *>(node));
}

void switch_vic_semantics_physical(uint64_t)
{
    switch_gpu_events_completed();
}
