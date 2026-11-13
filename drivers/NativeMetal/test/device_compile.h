// SPDX-License-Identifier: MIT
// The semantic harnesses' compile through NVK's device path (switch_nvk_compile_stage on the runtime's VkDevice), which
// raw's direct compile no longer takes: they compare NVK's own routes, so they keep the device.
#pragma once
#include "../../TegraGM20BMetal/raw/runtime/raw_direct_nvk.h"

namespace raw::nvk {

struct DeviceNvk {
    void *compileStage = nullptr, *stageFinish = nullptr;

    static DeviceNvk find(void *(*symbol)(const char *name))
    {
        return {symbol("switch_nvk_compile_stage"), symbol("switch_nvk_stage_finish")};
    }
};

inline int32_t compileStage(const DeviceNvk &n, void *device, const NvkStageInfo &info, NvkStage &out)
{
    return reinterpret_cast<int32_t (*)(void *, const NvkStageInfo *, NvkStage *)>(n.compileStage)(device, &info, &out);
}

inline void stageFinish(const DeviceNvk &n, void *device, NvkStage &stage)
{
    reinterpret_cast<void (*)(void *, NvkStage *)>(n.stageFinish)(device, &stage);
}

}  // namespace raw::nvk
