// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bring-up steps that raise no interrupt and for which Linux gives only "poll until done, up to N": one look after
// `first_ms`, the last at N, nothing in between and no spinning. Thread context, never on a transfer path. Development
// builds say when the first look came too early, so a device run gives the step's real time.
#pragma once
#include <IOKit/IOLib.h>
#include <stdint.h>
#if DEVELOPMENT || DEBUG
#include <os/log.h>
#endif

namespace xusb {

template <typename Ready>
bool settle([[maybe_unused]] const char *step, uint32_t first_ms, uint32_t bound_ms, Ready ready)
{
    IOSleep(first_ms);
    if (ready())
        return true;
    bool done = false;
    if (bound_ms > first_ms) {
        IOSleep(bound_ms - first_ms);
        done = ready();
    }
#if DEVELOPMENT || DEBUG
    os_log(OS_LOG_DEFAULT, "SwitchUSB: %s not done after %u ms, %s after %u ms", step, first_ms,
           done ? "done" : "still not done", bound_ms);
#endif
    return done;
}

}  // namespace xusb
