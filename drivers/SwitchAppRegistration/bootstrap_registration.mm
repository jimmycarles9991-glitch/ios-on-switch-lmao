// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "bootstrap_registration.h"
#include "uicache_options.h"

#include <limits.h>
#include <mach/message.h>
#include <os/log.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sysexits.h>
#include <unistd.h>
#include <xpc/xpc.h>

// libxpc exports the Mach service constructor on iOS, but its SDK declaration is macOS-only.
extern "C" xpc_connection_t switch_registration_mach_service(const char *, dispatch_queue_t,
                                                             uint64_t) __asm__("_xpc_connection_create_mach_service");
extern "C" void xpc_dictionary_get_audit_token(xpc_object_t, audit_token_t *);
extern char **environ;
extern int switch_registration_apps(NSArray<NSString *> *, NSString **);

static const char switch_registration_service[] = "com.brunostjohn.switch.bootstrap-apps";

static uid_t switch_registration_sender(xpc_object_t message)
{
    audit_token_t token = INVALID_AUDIT_TOKEN_VALUE;
    xpc_dictionary_get_audit_token(message, &token);
    if (!token.val[5] || token.val[5] > INT_MAX)
        return (uid_t)-1;
    return token.val[1];
}

static NSArray<NSString *> *switch_registration_arguments(xpc_object_t request)
{
    if (xpc_dictionary_get_count(request) != 1)
        return nil;
    xpc_object_t array = xpc_dictionary_get_value(request, "arguments");
    if (!array || xpc_get_type(array) != XPC_TYPE_ARRAY || !xpc_array_get_count(array) ||
        xpc_array_get_count(array) > 256)
        return nil;
    NSMutableArray *arguments = [NSMutableArray new];
    for (size_t index = 0; index < xpc_array_get_count(array); ++index) {
        xpc_object_t value = xpc_array_get_value(array, index);
        if (xpc_get_type(value) != XPC_TYPE_STRING || xpc_string_get_length(value) >= PATH_MAX)
            return nil;
        const char *text = xpc_string_get_string_ptr(value);
        if (strlen(text) != xpc_string_get_length(value))
            return nil;
        NSString *argument = @(text);
        if (!argument)
            return nil;
        [arguments addObject:argument];
    }
    return arguments;
}

static void switch_registration_answer(xpc_connection_t peer, xpc_object_t request)
{
    if (xpc_get_type(request) != XPC_TYPE_DICTIONARY)
        return;
    xpc_object_t reply = xpc_dictionary_create_reply(request);
    if (!reply)
        return;
    int status = EX_NOPERM;
    NSString *output = nil;
    const uid_t sender = switch_registration_sender(request);
    if (sender == 0 || sender == 501) {
        NSArray *arguments = switch_registration_arguments(request);
        status = arguments ? switch_registration_apps(arguments, &output) : EX_USAGE;
    }
    xpc_dictionary_set_int64(reply, "status", status);
    if (output.length <= 1024 * 1024 && output)
        xpc_dictionary_set_string(reply, "output", output.UTF8String);
    xpc_connection_send_message(peer, reply);
}

int switch_registration_serve(void)
{
    if (geteuid() != 501)
        return EX_NOPERM;
    dispatch_queue_t work = dispatch_queue_create(switch_registration_service, DISPATCH_QUEUE_SERIAL);
    xpc_connection_t listener =
        switch_registration_mach_service(switch_registration_service, work, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener)
        return EX_UNAVAILABLE;
    xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
        if (event == XPC_ERROR_CONNECTION_INVALID) {
            os_log_error(OS_LOG_DEFAULT, "SwitchAppRegistration: bootstrap Mach service unavailable");
            exit(EX_UNAVAILABLE);
        }
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION)
            return;
        xpc_connection_t peer = (xpc_connection_t)event;
        if (@available(iOS 17.4, macOS 14.4, *)) {
            if (xpc_connection_set_peer_entitlement_matches_value_requirement(
                    peer, "com.apple.private.coreservices.can-register-install-results", XPC_BOOL_TRUE)) {
                xpc_connection_cancel(peer);
                return;
            }
        } else {
            xpc_connection_cancel(peer);
            return;
        }
        xpc_connection_set_target_queue(peer, work);
        xpc_connection_set_event_handler(peer, ^(xpc_object_t request) {
            @autoreleasepool {
                switch_registration_answer(peer, request);
            }
        });
        xpc_connection_activate(peer);
    });
    xpc_connection_activate(listener);
    dispatch_main();
}

int switch_registration_request(NSArray<NSString *> *arguments)
{
    if (geteuid() != 0 && geteuid() != 501)
        return EX_NOPERM;
    // Supplemental jobs on iPadOS 27 share the inherited user/501 bootstrap namespace.
    xpc_connection_t connection = switch_registration_mach_service(switch_registration_service, NULL, 0);
    if (!connection)
        return EX_UNAVAILABLE;
    if (@available(iOS 17.4, macOS 14.4, *)) {
        if (xpc_connection_set_peer_entitlement_matches_value_requirement(
                connection, "com.apple.private.coreservices.can-register-install-results", XPC_BOOL_TRUE)) {
            xpc_connection_cancel(connection);
            return EX_NOPERM;
        }
    } else {
        xpc_connection_cancel(connection);
        return EX_UNAVAILABLE;
    }
    xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
        (void)event;
    });
    xpc_connection_activate(connection);
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_object_t values = xpc_array_create(NULL, 0);
    for (NSString *argument in arguments)
        xpc_array_set_string(values, XPC_ARRAY_APPEND, argument.UTF8String);
    xpc_dictionary_set_value(request, "arguments", values);
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    __block xpc_object_t reply = nil;
    xpc_connection_send_message_with_reply(connection, request, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                                           ^(xpc_object_t response) {
                                               reply = response;
                                               dispatch_semaphore_signal(completed);
                                           });
    const long timeout = dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_SEC));
    xpc_connection_cancel(connection);
    if (timeout)
        return EX_TEMPFAIL;
    if (reply && xpc_get_type(reply) == XPC_TYPE_ERROR) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAppRegistration: bootstrap request failed: %{public}s",
                     xpc_dictionary_get_string(reply, XPC_ERROR_KEY_DESCRIPTION));
        return EX_UNAVAILABLE;
    }
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY)
        return EX_UNAVAILABLE;
    if (switch_registration_sender(reply) != 501)
        return EX_NOPERM;
    xpc_object_t value = xpc_dictionary_get_value(reply, "status");
    if (!value || xpc_get_type(value) != XPC_TYPE_INT64)
        return EX_PROTOCOL;
    const char *output = xpc_dictionary_get_string(reply, "output");
    if (output)
        fputs(output, stdout);
    const int64_t status = xpc_int64_get_value(value);
    return status == EX_OK || (status >= EX_USAGE && status <= EX_CONFIG) ? (int)status : EX_PROTOCOL;
}

int switch_uicache_main(int argc, char **argv)
{
    const SwitchUICacheOptions options = switch_uicache_parse(argc, argv);
    if (!options.error.empty() || options.help) {
        if (!options.error.empty())
            fprintf(stderr, "uicache: %s\n", options.error.c_str());
        fprintf(options.help ? stdout : stderr, "usage: uicache [-a] [-p APP...] [-u APP...] [-l] [-r]\n");
        return options.error.empty() ? EX_OK : EX_USAGE;
    }
    // The bootstrap transaction drains dpkg triggers before the one final registration request.
    const char *updating = getenv("SWITCH_BOOTSTRAP_UPDATE");
    if (updating && !strcmp(updating, "1"))
        return EX_OK;
    NSMutableArray *arguments = [NSMutableArray new];
    if (options.all)
        [arguments addObject:@"-a"];
    if (options.list)
        [arguments addObject:@"-l"];
    for (const std::string &path : options.paths) {
        NSString *value = @(path.c_str());
        if (!value)
            return EX_USAGE;
        [arguments addObjectsFromArray:@[ @"-p", value ]];
    }
    for (const std::string &path : options.unregister) {
        NSString *value = @(path.c_str());
        if (!value)
            return EX_USAGE;
        [arguments addObjectsFromArray:@[ @"-u", value ]];
    }
    const int result = arguments.count ? switch_registration_request(arguments) : EX_OK;
    if (result || !options.respring)
        return result;
    char *const command[] = {const_cast<char *>("/var/jb/usr/bin/sbreload"), NULL};
    pid_t child = -1;
    if (posix_spawn(&child, command[0], NULL, NULL, command, environ))
        return EX_OSERR;
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && !WEXITSTATUS(status) ? EX_OK : EX_SOFTWARE;
}
