// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_host1x_results_calls.h"
#include "drivers/TegraHost1x/SwitchHost1xResults.h"

using VIC = SwitchHost1xResults<SWITCH_GPU_ENGINE_VIC>;
using NVDEC = SwitchHost1xResults<SWITCH_GPU_ENGINE_NVDEC>;

bool switch_host1x_test_start(uint32_t engine, uint32_t syncpoint)
{
    return engine == SWITCH_GPU_ENGINE_VIC ? VIC::start(syncpoint) : NVDEC::start(syncpoint);
}

template <typename Results>
static SwitchHost1xResult *produce(uint64_t first, uint64_t last, switch_gpu_backing_token *token)
{
    auto *record = Results::allocate();
    if (Results::prepare(record, first, last, token))
        Results::cancel(&record);
    return record;
}

SwitchHost1xResult *switch_host1x_test_produce(uint32_t engine, uint64_t first, uint64_t last,
                                               switch_gpu_backing_token *token)
{
    return engine == SWITCH_GPU_ENGINE_VIC ? produce<VIC>(first, last, token) : produce<NVDEC>(first, last, token);
}

int switch_host1x_test_producer_status(const switch_gpu_backing_token *token)
{
    return token->engine == SWITCH_GPU_ENGINE_VIC ? VIC::status(token) : NVDEC::status(token);
}

void switch_host1x_test_complete(uint32_t engine, SwitchHost1xResult **record, int result)
{
    if (engine == SWITCH_GPU_ENGINE_VIC)
        VIC::complete(record, result);
    else
        NVDEC::complete(record, result);
}
