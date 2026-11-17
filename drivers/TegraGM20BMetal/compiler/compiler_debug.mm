// SPDX-License-Identifier: MIT
// See compiler_debug.h.
#include "compiler_debug.h"

#if SWITCH_DEBUG
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <notify.h>
#include <signal.h>
#include <unistd.h>

namespace switch_compiler {
namespace {

constexpr const char *kDiesAtNextRequest = "com.brunostjohn.switch.debug.compiler-dies-at-next-request";
int gDeathToken = -1;

}  // namespace

const char *resultName(int32_t result)
{
    static const char *names[] = {"made", "refused", "not made", "not taken"};
    return result >= 0 && result <= 3 ? names[result] : "?";
}

void logFootprint(os_log_t log, const char *when)
{
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) != KERN_SUCCESS)
        return;
    malloc_statistics_t heap = {};
    malloc_zone_statistics(nullptr, &heap);
    os_log(
        log,
        "SwitchShaderCompiler: footprint %{public}s: %llu KiB, peak %llu KiB; internal %llu KiB, compressed %llu KiB; "
        "heap %llu KiB in use of %llu KiB",
        when, (unsigned long long)(vm.phys_footprint >> 10), (unsigned long long)(vm.ledger_phys_footprint_peak >> 10),
        (unsigned long long)(vm.internal >> 10), (unsigned long long)(vm.compressed >> 10),
        (unsigned long long)(heap.size_in_use >> 10), (unsigned long long)(heap.size_allocated >> 10));
}

void armDeath()
{
    notify_register_check(kDiesAtNextRequest, &gDeathToken);
}

// The state is cleared first, so the compiler launchd starts next serves the request
void dieIfArmedFor(int32_t pid)
{
    uint64_t armed = 0;
    if (!pid || gDeathToken < 0 || notify_get_state(gDeathToken, &armed) != NOTIFY_STATUS_OK || armed != uint64_t(pid))
        return;
    notify_set_state(gDeathToken, 0);
    kill(getpid(), SIGKILL);
}

}  // namespace switch_compiler
#endif
