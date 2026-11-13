// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"
#include "TegraGM20BViews.h"

struct SwitchGpuSurfaceRefresh {
	switch_gpu_backing *backing;
	void *surface;
	const volatile uint64_t *generation;
	uint64_t *cached;
	uint32_t id, access;
	uint64_t expected;
	switch_gpu_backing *logical;
	switch_gpu_cpu_lease *owned;
	void *descriptor;

	static void release(SwitchGpuSurfaceRefresh *surfaces, uint32_t count)
	{
		for (uint32_t i = 0; i < count; ++i) {
			auto &surface = surfaces[i];
			if (surface.owned)
				switch_gpu_backing_cpu_release_owned(&surface.owned, 0);
			if (surface.logical)
				switch_gpu_backing_release(surface.logical);
			if (surface.descriptor)
				switch_gpu_surface_finish(surface.descriptor);
			surface.logical = nullptr;
			surface.descriptor = nullptr;
		}
	}

	static int prepare(SwitchGpuSurfaceRefresh *surfaces, uint32_t count, void *task,
			   switch_gpu_backing_guard *guards, switch_gpu_cpu_lease **leases,
			   uint32_t *leaseCount)
	{
		*leaseCount = 0;
		bool changed = false;
		for (uint32_t i = 0; i < count; ++i) {
			const auto &surface = surfaces[i];
			const uint64_t expected = __atomic_load_n(surface.cached, __ATOMIC_ACQUIRE);
			const uint64_t current =
				__atomic_load_n(surface.generation, __ATOMIC_ACQUIRE);
			changed |= current != expected ||
				   (current > 1 && (surface.access & SWITCH_GPU_BACKING_WRITE));
			guards[i] = {surface.backing, surface.generation, expected};
		}
		if (!changed)
			return 0;
		// Reservations survive individual backing locks; all callers must acquire them in the same order.
		for (uint32_t i = 1; i < count; ++i) {
			auto value = surfaces[i];
			uint32_t j = i;
			while (j && uintptr_t(surfaces[j - 1].backing) > uintptr_t(value.backing)) {
				surfaces[j] = surfaces[j - 1];
				--j;
			}
			surfaces[j] = value;
		}
		for (uint32_t i = 0; i < count; ++i) {
			auto &surface = surfaces[i];
			uint32_t access = surface.access;
			const uint64_t current =
				__atomic_load_n(surface.generation, __ATOMIC_ACQUIRE);
			surface.expected = __atomic_load_n(surface.cached, __ATOMIC_ACQUIRE);
			const bool refresh = current != surface.expected ||
					     (current > 1 && (access & SWITCH_GPU_BACKING_WRITE));
			if (!refresh) {
				guards[i] = {surface.backing, surface.generation, surface.expected};
				continue;
			}
			for (uint32_t j = 0; j < count; ++j)
				if (surfaces[j].backing == surface.backing)
					access |= surfaces[j].access;
			bool shared = false;
			for (uint32_t j = 0; j < i; ++j) {
				const auto &previous = surfaces[j];
				if (previous.backing != surface.backing || !previous.descriptor)
					continue;
				if (previous.generation != surface.generation) {
					if (refresh)
						return -95;
					continue;
				}
				surface.expected = previous.expected;
				shared = true;
				break;
			}
			if (refresh && !shared) {
				const int error = switch_gpu_surface_prepare_gpu(
					surface.surface, surface.id, task, access, &surface.logical,
					&surface.owned, &surface.descriptor);
				if (error)
					return error;
				if (surface.logical && surface.logical != surface.backing)
					return -95;
				surface.expected =
					__atomic_load_n(surface.generation, __ATOMIC_ACQUIRE);
				if (surface.owned)
					leases[(*leaseCount)++] = surface.owned;
			}
			__atomic_store_n(surface.cached, surface.expected, __ATOMIC_RELEASE);
			guards[i] = {surface.backing, surface.generation, surface.expected};
		}
		return 0;
	}
};
