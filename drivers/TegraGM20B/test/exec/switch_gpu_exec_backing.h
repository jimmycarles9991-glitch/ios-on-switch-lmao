// SPDX-License-Identifier: MIT
// The real backing layer with GM20B timelines the test completes, through SwitchGpuBacking.h only.
#pragma once
#include <cstdint>
#include "drivers/TegraGM20B/SwitchGpuBacking.h"

namespace switch_exec_backing
{
void register_engines();
switch_gpu_backing *make();
// What switch_gpu_timeline_token hands out for a channel with this id.
switch_gpu_backing_token token(uint64_t channel_id, uint32_t value);
void complete(const switch_gpu_backing_token &token);
// A committed job's write of the backing.
void publish_write(switch_gpu_backing *backing, const switch_gpu_backing_token &token);
// The engines with recorded work a reader of the backing would wait for.
uint32_t pending(switch_gpu_backing *backing);
// Every timeline failed and every record retired.
void retire_all();
}  // namespace switch_exec_backing
