// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
class OSObject;
extern "C" {
OSObject *switch_gpu_event_native_lookup(void *task, uint32_t port);
uint64_t switch_gpu_event_native_value(OSObject *event);
void switch_gpu_event_native_signal(OSObject *event, uint64_t value, bool notify);
OSObject *switch_gpu_event_native_listener_create(uint32_t priority);
using SwitchGpuEventNativeCallback = void (*)(OSObject *, OSObject *, uint64_t, void *);
OSObject *switch_gpu_event_native_waiter_create(OSObject *event, OSObject *listener, uint64_t value,
						void *refcon, OSObject *owner,
						SwitchGpuEventNativeCallback callback);
void switch_gpu_event_native_waiter_arm(OSObject *event, OSObject *waiter);
}
bool switch_gpu_event_native_waiter_cancel(OSObject *event, OSObject *waiter);
