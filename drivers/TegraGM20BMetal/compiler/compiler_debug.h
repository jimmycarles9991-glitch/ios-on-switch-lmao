// SPDX-License-Identifier: MIT
// What only a debug compiler does (compiler_service.mm): its lines' names for results, its footprint, and the HW Test's
// death at a chosen process's next request.
#pragma once
#include <os/log.h>
#include <cstdint>

namespace switch_compiler {

#if SWITCH_DEBUG
const char *resultName(int32_t result);
// What jetsam holds against the plist's JetsamProperties: the footprint now and its peak this process, and the heap's part
void logFootprint(os_log_t log, const char *when);
// HW Test gpu.compile.first_draw: the compiler dies as the memory limit kills one, holding the next request of the pid
// com.brunostjohn.switch.debug.compiler-dies-at-next-request's notify state holds, so a run shows that request's
// creation completing once launchd's next compiler takes it
void armDeath();
void dieIfArmedFor(int32_t pid);
#endif

}  // namespace switch_compiler
