// SPDX-License-Identifier: MIT
// The kernel model's check of every job it holds whole: each method against the class of its subchannel
// (kits/SwitchGM20BEmit/test/method_checks.h), so a test over the model fails where the GPU would kill the channel.
#pragma once
#include "raw_kernel_model.h"
#include "kits/SwitchGM20BEmit/test/method_checks.h"
#include <vector>

namespace kernel_model {

inline void checkJobMethods(const uint32_t *words, const TegraGM20BAbiPush *pushes, uint32_t count)
{
    std::vector<uint32_t> segments(count);
    for (uint32_t i = 0; i < count; i++)
        segments[i] = pushes[i].dwords;
    EXPECT_TRUE(switch_gm20b_test::expectSegmentMethods(words, segments.data(), segments.size()))
        << "a job's words decode as methods";
}

inline const bool jobMethodsChecked = (checkJob = checkJobMethods, true);

}  // namespace kernel_model
