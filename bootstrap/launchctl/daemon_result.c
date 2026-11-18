// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "daemon_result.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>

static int switch_launchctl_service_result(int error, bool removing)
{
    if (!error)
        return SWITCH_LAUNCHCTL_CHANGED;
    if (removing && (error == 113 || error == EALREADY))
        return SWITCH_LAUNCHCTL_ABSENT;
    if (!removing && (error == EEXIST || error == EALREADY))
        return SWITCH_LAUNCHCTL_PRESENT;
    if (!removing && error == 119)
        return SWITCH_LAUNCHCTL_DISABLED;
    return SWITCH_LAUNCHCTL_FAILED;
}

int switch_launchctl_daemon_result(int native_error, xpc_object_t reply, bool removing)
{
    if (native_error)
        return switch_launchctl_service_result(native_error, removing);
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY)
        return SWITCH_LAUNCHCTL_FAILED;
    xpc_object_t errors = xpc_dictionary_get_value(reply, "errors");
    if (errors && xpc_get_type(errors) != XPC_TYPE_DICTIONARY)
        return SWITCH_LAUNCHCTL_FAILED;
    __block int result = SWITCH_LAUNCHCTL_CHANGED;
    if (errors)
        xpc_dictionary_apply(errors, ^bool(const char *path, xpc_object_t value) {
            (void)path;
            if (xpc_get_type(value) != XPC_TYPE_INT64) {
                result = SWITCH_LAUNCHCTL_FAILED;
                return false;
            }
            const int64_t code = xpc_int64_get_value(value);
            const int status = code >= 0 && code <= INT_MAX ? switch_launchctl_service_result((int)code, removing)
                                                            : SWITCH_LAUNCHCTL_FAILED;
            if (status > result)
                result = status;
            return result != SWITCH_LAUNCHCTL_FAILED;
        });
    xpc_object_t aggregate = xpc_dictionary_get_value(reply, removing ? "bootout-error" : "bootstrap-error");
    if (aggregate) {
        if (xpc_get_type(aggregate) != XPC_TYPE_INT64)
            return SWITCH_LAUNCHCTL_FAILED;
        const int64_t code = xpc_int64_get_value(aggregate);
        const int status = code >= 0 && code <= INT_MAX ? switch_launchctl_service_result((int)code, removing)
                                                        : SWITCH_LAUNCHCTL_FAILED;
        if (status > result)
            result = status;
    }
    return result;
}

void switch_launchctl_daemon_diagnostic(int native_error, xpc_object_t reply, bool removing)
{
    fprintf(stderr, "launchctl: native daemon request failed (%d)\n", native_error);
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY)
        return;
    xpc_object_t errors = xpc_dictionary_get_value(reply, "errors");
    if (errors && xpc_get_type(errors) == XPC_TYPE_DICTIONARY)
        xpc_dictionary_apply(errors, ^bool(const char *path, xpc_object_t value) {
            if (xpc_get_type(value) == XPC_TYPE_INT64)
                fprintf(stderr, "%.512s: %lld\n", path, (long long)xpc_int64_get_value(value));
            return true;
        });
    xpc_object_t aggregate = xpc_dictionary_get_value(reply, removing ? "bootout-error" : "bootstrap-error");
    if (aggregate && xpc_get_type(aggregate) == XPC_TYPE_INT64)
        fprintf(stderr, "%s: %lld\n", removing ? "bootout-error" : "bootstrap-error",
                (long long)xpc_int64_get_value(aggregate));
}
