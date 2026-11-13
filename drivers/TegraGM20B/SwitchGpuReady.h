// SPDX-License-Identifier: MIT
#pragma once

struct switch_gpu_ready_watch;
using switch_gpu_ready_query = int (*)(void *);
using switch_gpu_ready_callback = void (*)(void *, int);

// Query returns zero while blocked, one when ready, or a terminal error. Context lives through callback.
int switch_gpu_ready_observe(switch_gpu_ready_query, switch_gpu_ready_callback, void *context,
			     switch_gpu_ready_watch **);
void switch_gpu_ready_cancel(switch_gpu_ready_watch **);
extern "C" void switch_gpu_admission_changed(void);
