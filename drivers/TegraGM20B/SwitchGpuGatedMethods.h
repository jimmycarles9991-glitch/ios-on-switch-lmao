// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "TegraGM20BABI.h"

// The connection-lock methods that close the submit gate: what they change (channels, resource sets, the surfaces' lists,
// the priority) a SubmitJob in flight reads in place. BOs and bindings are published instead (SwitchGpuBoIndex.h,
// SwitchGpuBindingTable.h), so BoAlloc, BoImport and BoFree do not close it, nor does a VmBind into VA no binding holds;
// a VmBind that cuts a live binding, and a BoFree of a resource set's member, close it themselves. SurfaceWrites only
// marks the surface's coherency entry.
static inline bool switch_gpu_method_closes_gate(uint32_t method)
{
	switch (method) {
	case TegraGM20BMethodGetInfo:
	case TegraGM20BMethodGetStatus:
	case TegraGM20BMethodSurfaceLayout:
	case TegraGM20BMethodSurfaceLayoutInfo:
	case TegraGM20BMethodBoAlloc:
	case TegraGM20BMethodBoImport:
	case TegraGM20BMethodBoFree:
	case TegraGM20BMethodVmBind:
	case TegraGM20BMethodSurfaceWrites:
		return false;
	default:
		return true;
	}
}
