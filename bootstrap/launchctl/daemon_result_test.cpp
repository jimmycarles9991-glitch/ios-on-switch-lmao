// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <initializer_list>

extern "C" {
#include "daemon_result.h"
}

static xpc_object_t switch_test_reply(int code)
{
    xpc_object_t reply = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_int64(reply, "bootstrap-error", code);
    return reply;
}

int main()
{
    assert(switch_launchctl_daemon_result(0, switch_test_reply(0), false) == SWITCH_LAUNCHCTL_CHANGED);
    for (int code : {EEXIST, EALREADY})
        assert(switch_launchctl_daemon_result(0, switch_test_reply(code), false) == SWITCH_LAUNCHCTL_PRESENT);
    assert(switch_launchctl_daemon_result(0, switch_test_reply(119), false) == SWITCH_LAUNCHCTL_DISABLED);
    assert(switch_launchctl_daemon_result(119, nullptr, false) == SWITCH_LAUNCHCTL_DISABLED);
    assert(switch_launchctl_daemon_result(113, nullptr, true) == SWITCH_LAUNCHCTL_ABSENT);
    assert(switch_launchctl_daemon_result(113, nullptr, false) == SWITCH_LAUNCHCTL_FAILED);
    assert(switch_launchctl_daemon_result(EACCES, nullptr, false) == SWITCH_LAUNCHCTL_FAILED);
    assert(switch_launchctl_daemon_result(0, nullptr, false) == SWITCH_LAUNCHCTL_FAILED);

    xpc_object_t reply = switch_test_reply(0);
    xpc_object_t errors = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_value(reply, "errors", errors);
    xpc_dictionary_set_int64(errors, "disabled.plist", 119);
    xpc_dictionary_set_int64(errors, "loaded.plist", EEXIST);
    assert(switch_launchctl_daemon_result(0, reply, false) == SWITCH_LAUNCHCTL_DISABLED);
    xpc_dictionary_set_int64(errors, "invalid.plist", EINVAL);
    assert(switch_launchctl_daemon_result(0, reply, false) == SWITCH_LAUNCHCTL_FAILED);
    xpc_dictionary_set_value(errors, "invalid.plist", nullptr);
    xpc_dictionary_set_int64(reply, "bootstrap-error", EACCES);
    assert(switch_launchctl_daemon_result(0, reply, false) == SWITCH_LAUNCHCTL_FAILED);
    xpc_dictionary_set_int64(reply, "bootstrap-error", 0);
    xpc_dictionary_set_string(errors, "invalid.plist", "119");
    assert(switch_launchctl_daemon_result(0, reply, false) == SWITCH_LAUNCHCTL_FAILED);
    xpc_dictionary_set_int64(errors, "invalid.plist", INT64_MAX);
    assert(switch_launchctl_daemon_result(0, reply, false) == SWITCH_LAUNCHCTL_FAILED);
    xpc_dictionary_set_string(reply, "errors", "invalid");
    assert(switch_launchctl_daemon_result(0, reply, false) == SWITCH_LAUNCHCTL_FAILED);

    reply = xpc_dictionary_create(nullptr, nullptr, 0);
    xpc_dictionary_set_int64(reply, "bootout-error", 113);
    assert(switch_launchctl_daemon_result(0, reply, true) == SWITCH_LAUNCHCTL_ABSENT);
    xpc_dictionary_set_bool(reply, "bootout-error", false);
    assert(switch_launchctl_daemon_result(0, reply, true) == SWITCH_LAUNCHCTL_FAILED);
}
