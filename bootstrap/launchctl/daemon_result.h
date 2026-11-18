// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdbool.h>
#include <xpc/xpc.h>

enum switch_launchctl_daemon_status {
    SWITCH_LAUNCHCTL_CHANGED = 0,
    SWITCH_LAUNCHCTL_PRESENT = 200,
    SWITCH_LAUNCHCTL_DISABLED = 201,
    SWITCH_LAUNCHCTL_ABSENT = 202,
    SWITCH_LAUNCHCTL_FAILED = 203,
};

int switch_launchctl_daemon_result(int native_error, xpc_object_t reply, bool removing);
void switch_launchctl_daemon_diagnostic(int native_error, xpc_object_t reply, bool removing);
