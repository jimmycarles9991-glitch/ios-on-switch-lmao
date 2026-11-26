// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xpc/xpc.h>

typedef CFTypeRef (*da_session_create_fn)(CFAllocatorRef);
typedef void (*xpc_stream_handler_fn)(const char *, dispatch_queue_t, xpc_handler_t);

static void wakeDiskArbitration(void)
{
    void *framework = dlopen("/System/Library/PrivateFrameworks/DiskArbitration.framework/DiskArbitration", RTLD_LAZY);
    if (!framework) {
        os_log_error(OS_LOG_DEFAULT, "SwitchSDWake: dlopen DiskArbitration failed: %{public}s", dlerror());
        return;
    }
    da_session_create_fn create = (da_session_create_fn)dlsym(framework, "DASessionCreate");
    if (!create) {
        os_log_error(OS_LOG_DEFAULT, "SwitchSDWake: dlsym DASessionCreate failed: %{public}s", dlerror());
        dlclose(framework);
        return;
    }
    CFTypeRef session = create(kCFAllocatorDefault);
    if (!session)
        os_log_error(OS_LOG_DEFAULT, "SwitchSDWake: DASessionCreate failed");
    else
        CFRelease(session);
    dlclose(framework);
}

int main(void)
{
    dispatch_queue_t queue = dispatch_queue_create("com.brunostjohn.switch.sdwake", DISPATCH_QUEUE_SERIAL);
    xpc_stream_handler_fn setStreamHandler = (xpc_stream_handler_fn)dlsym(RTLD_DEFAULT, "xpc_set_event_stream_handler");
    if (!setStreamHandler) {
        os_log_error(OS_LOG_DEFAULT, "SwitchSDWake: xpc_set_event_stream_handler missing");
        wakeDiskArbitration();
        return 0;
    }
    __block bool handled = false;
    setStreamHandler("com.apple.iokit.matching", queue, ^(xpc_object_t event) {
        (void)event;
        if (handled)
            return;
        handled = true;
        // Let libxpc finish acknowledging the LaunchEvents stream message before exiting.
        dispatch_async(queue, ^{
            wakeDiskArbitration();
            exit(0);
        });
    });
    dispatch_main();
}
