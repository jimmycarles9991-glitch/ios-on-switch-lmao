// SPDX-License-Identifier: MIT
#include <cstddef>
#include <cstdio>

extern "C" void switch_gpu_bridge_completed()
{
}

extern "C" void switch_gpu_events_completed()
{
}

extern "C" void switch_gpu_copy_completed(void)
{
}

extern "C" void switch_gpu_admission_changed(void)
{
}

int switch_gpu_test_ready_waiting;

extern "C" int switch_gpu_ready_waiting(void)
{
	return switch_gpu_test_ready_waiting;
}

// The path counts (SwitchGpuPathStats.h) are the kernel's; a test that reads them defines its own.
extern "C" __attribute__((weak)) void switch_gpu_path_taken(int, unsigned)
{
}

// No writer promise is given up on unless a test says so.
extern "C" __attribute__((weak)) int tgm_promise_abandoned(unsigned, unsigned)
{
	return 0;
}

// A CPU reader's park on a pending promise (SwitchGpuPromiseGiveUp.h) and its leave, which a test may count.
extern "C" __attribute__((weak)) int tgm_promise_park(unsigned, unsigned, const char *,
						      const char *, const void *, int)
{
	return 1;
}

extern "C" __attribute__((weak)) void tgm_promise_leave(unsigned, unsigned, int)
{
}

extern "C" __attribute__((weak)) void proc_selfname(char *buf, int size)
{
	if (size > 0)
		std::snprintf(buf, size_t(size), "%s", "host-test");
}
