// SPDX-License-Identifier: MIT
// When the shader compiler broker stops waiting for a compiler (SwitchGpuShaderCompiler.cpp): the rules, without IOKit, so
// a host test holds them.
#pragma once
#include <stdint.h>

namespace switch_gpu_compiler
{
// Compilers lost in a row, none registering between them, before the kernel stops waiting for the next: a compiler that
// dies before it serves, every time its launcher starts it
constexpr uint32_t kLosses = 3;

// After the compiler's connection closed, registered or only announced: whether no compiler comes this boot. A decline
// stands until a compiler registers; one that nothing starts again is a decline (the BlueTool stand-in's last start says
// so); one its launcher starts again is waited for until the third loss in a row. A compiler the memory limit kills while
// it serves comes back and registers each time, and the requests it held wait for it.
inline bool declinedAfterClose(bool declined, bool restarts, uint32_t &losses)
{
	if (declined || !restarts)
		return true;
	return ++losses >= kLosses;
}

// A compiler registered: the losses before it were not in a row
inline void registeredAfterLosses(uint32_t &losses)
{
	losses = 0;
}

}  // namespace switch_gpu_compiler
