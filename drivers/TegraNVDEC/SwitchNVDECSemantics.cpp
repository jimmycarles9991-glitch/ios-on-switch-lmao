// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchNVDECSemantics.h"
#include "SwitchNVDECQueue.h"
#include "../TegraHost1x/SwitchHost1xSemantics.h"
using Timeline = SwitchHost1xSemantics<SWITCH_GPU_ENGINE_NVDEC, SwitchNVDECQueue::kSlots>;

bool switch_nvdec_semantics_start(uint32_t id, void (*retired)(switch_gpu_event_waits *))
{
    return Timeline::start(id, retired);
}

bool switch_nvdec_semantics_pending()
{
    return Timeline::hasPendingWork();
}

int switch_nvdec_semantics_bind(switch_gpu_event_waits *batch, const switch_gpu_backing_token *token,
                                SwitchNVDECSemantic **out)
{
    Timeline::Node *node = nullptr;
    const int result = Timeline::bind(batch, token, &node);
    *out = reinterpret_cast<SwitchNVDECSemantic *>(node);
    return result;
}

void switch_nvdec_semantics_commit(SwitchNVDECSemantic *node)
{
    Timeline::commit(reinterpret_cast<Timeline::Node *>(node));
}

void switch_nvdec_semantics_physical(uint64_t)
{
    switch_gpu_events_completed();
}
