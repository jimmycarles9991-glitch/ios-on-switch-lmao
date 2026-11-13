// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"

class SwitchGpuAdmission
{
public:
	SwitchGpuAdmission() = default;
	~SwitchGpuAdmission();
	SwitchGpuAdmission(const SwitchGpuAdmission &) = delete;
	SwitchGpuAdmission &operator=(const SwitchGpuAdmission &) = delete;
	int begin(const switch_gpu_backing_use *uses, uint32_t count,
		  const switch_gpu_backing_guard *guards, uint32_t guardCount,
		  switch_gpu_cpu_lease *const *leases, uint32_t leaseCount);
	bool covers(const switch_gpu_backing_use *uses, uint32_t count) const;
	void end();

	bool active() const
	{
		return active_;
	}

private:
	switch_gpu_backing_use *uses_ = nullptr;
	uint32_t count_ = 0, capacity_ = 0;
	bool active_ = false;
};

extern "C" int switch_gpu_backing_admission_ready(switch_gpu_backing *, uint32_t access);
extern "C" int switch_gpu_backing_conversion_ready(switch_gpu_backing *, uint32_t access);
extern "C" void switch_gpu_admission_changed(void);
