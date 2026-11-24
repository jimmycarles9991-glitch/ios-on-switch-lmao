// SPDX-License-Identifier: MIT
// What only a debug compiler does (compiler_service.mm): its lines' names for results, its footprint, and the HW Test's
// death at a chosen process's next request.
#pragma once
#include <os/log.h>
#include <cstdint>
#include <string>

namespace switch_compiler {

#if SWITCH_DEBUG
const char *resultName(int32_t result);
// A job's line: its result, where its wait went (the kernel's hand-over at `takenNs`, its prepare to `preparedNs`, then
// the pool) and where its compile went (compiler_times.h, this worker's), for pid `pid`'s request of `key`
void logJob(os_log_t log, int32_t result, uint64_t takenNs, uint64_t preparedNs, unsigned workerClass, int32_t pid,
            const std::string &key);
// What jetsam holds against the plist's JetsamProperties: the footprint now and its peak this process, and the heap's part
void logFootprint(os_log_t log, const char *when);
// HW Test gpu.compile.first_draw: the compiler dies as the memory limit kills one, holding the next request of the pid
// com.brunostjohn.switch.debug.compiler-dies-at-next-request's notify state holds, so a run shows that request's
// creation completing once launchd's next compiler takes it
void armDeath();
void dieIfArmedFor(int32_t pid);
#endif

}  // namespace switch_compiler
