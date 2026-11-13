// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVENC as an engine of SwitchGpuBacking: a job's fence (a threshold of NVENC's syncpoint) is the token of the memory
// it reads, which a later writer waits for, the other host1x engines and the GPU by a syncpoint wait in their streams
// and the CPU by tegra_nvenc_wait. The syncpoint is NVENC's own for the whole boot, kept through every engine reset, so
// a token keeps one meaning and needs no record of its own.
#include <IOKit/IOLib.h>
#include "TegraNVENC.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace {
constexpr uint32_t kResetGeneration = 1;
bool gRegistered;

int status(const switch_gpu_backing_token *token)
{
    if (!token || token->engine != SWITCH_GPU_ENGINE_NVENC || token->queue != tegra_nvenc_syncpt())
        return -22;
    const uint32_t reached = tegra_host1x_syncpt_read(token->queue);
    return static_cast<int32_t>(reached - static_cast<uint32_t>(token->value)) >= 0 ? 1 : 0;
}

// A read the engine cancelled or never ran leaves nothing for a later writer to wait for.
int wait(const switch_gpu_backing_token *token)
{
    const int now = status(token);
    if (now)
        return now;
    const int ret = tegra_nvenc_wait(static_cast<uint32_t>(token->value));
    return ret == 0 || ret == -5 || ret == -19 ? 1 : ret;
}

int retain(const switch_gpu_backing_token *)
{
    return 0;
}

void release(const switch_gpu_backing_token *)
{}
}  // namespace

bool tegra_nvenc_engine_start(void)
{
    const switch_gpu_backing_engine operations = {status, wait, retain, release, 32};
    const bool registered = !switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_NVENC, &operations);
    __atomic_store_n(&gRegistered, registered, __ATOMIC_RELEASE);
    return registered;
}

bool tegra_nvenc_engine_ready(void)
{
    return __atomic_load_n(&gRegistered, __ATOMIC_ACQUIRE);
}

switch_gpu_backing_token tegra_nvenc_token(uint32_t fence)
{
    return {fence, kResetGeneration, SWITCH_GPU_ENGINE_NVENC, tegra_nvenc_syncpt()};
}
