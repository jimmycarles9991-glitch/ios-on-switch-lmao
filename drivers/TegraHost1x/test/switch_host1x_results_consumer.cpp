// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_host1x_results_calls.h"
#include "drivers/TegraHost1x/SwitchHost1xResults.h"

using VIC = SwitchHost1xResults<SWITCH_GPU_ENGINE_VIC>;
using NVDEC = SwitchHost1xResults<SWITCH_GPU_ENGINE_NVDEC>;

template <typename Results> static SwitchHost1xResult *allocate(uint64_t sequence, switch_gpu_backing_token *token)
{
    auto *record = Results::allocate();
    if (Results::prepare(record, sequence, sequence, token))
        Results::cancel(&record);
    return record;
}

SwitchHost1xResult *switch_host1x_test_consumer_allocate(uint32_t engine, uint64_t sequence,
                                                         switch_gpu_backing_token *token)
{
    return engine == SWITCH_GPU_ENGINE_VIC ? allocate<VIC>(sequence, token) : allocate<NVDEC>(sequence, token);
}

int switch_host1x_test_status(const switch_gpu_backing_token *token)
{
    return token->engine == SWITCH_GPU_ENGINE_VIC ? VIC::status(token) : NVDEC::status(token);
}

int switch_host1x_test_physical_status(const switch_gpu_backing_token *token)
{
    return token->engine == SWITCH_GPU_ENGINE_VIC ? VIC::physicalStatus(token) : NVDEC::physicalStatus(token);
}

void switch_host1x_test_physical(uint32_t engine, SwitchHost1xResult *record, int result)
{
    if (engine == SWITCH_GPU_ENGINE_VIC)
        VIC::physical(record, result);
    else
        NVDEC::physical(record, result);
}

void switch_host1x_test_semantic(const switch_gpu_backing_token *token, int result)
{
    if (token->engine == SWITCH_GPU_ENGINE_VIC)
        VIC::semantic(*token, result);
    else
        NVDEC::semantic(*token, result);
}

int switch_host1x_test_retain(const switch_gpu_backing_token *token)
{
    return token->engine == SWITCH_GPU_ENGINE_VIC ? VIC::retain(token) : NVDEC::retain(token);
}

void switch_host1x_test_release(const switch_gpu_backing_token *token)
{
    if (token->engine == SWITCH_GPU_ENGINE_VIC)
        VIC::release(token);
    else
        NVDEC::release(token);
}

int switch_host1x_test_history(uint32_t engine, uint64_t sequence)
{
    return engine == SWITCH_GPU_ENGINE_VIC ? VIC::waitLegacy(sequence) : NVDEC::waitLegacy(sequence);
}
