// SPDX-License-Identifier: MIT
#pragma once

#if DEVELOPMENT || DEBUG
#if defined(__APPLE__)
extern "C" __attribute__((weak_import)) void switch_sched_render_dependency(int entering);
#else
extern "C" __attribute__((weak)) void switch_sched_render_dependency(int entering);
#endif
#endif

class SwitchGpuDependencyTrace
{
public:
	SwitchGpuDependencyTrace()
	{
#if DEVELOPMENT || DEBUG
		if (switch_sched_render_dependency)
			switch_sched_render_dependency(1);
#endif
	}

	~SwitchGpuDependencyTrace()
	{
#if DEVELOPMENT || DEBUG
		if (switch_sched_render_dependency)
			switch_sched_render_dependency(0);
#endif
	}

	SwitchGpuDependencyTrace(const SwitchGpuDependencyTrace &) = delete;
	SwitchGpuDependencyTrace &operator=(const SwitchGpuDependencyTrace &) = delete;
};
