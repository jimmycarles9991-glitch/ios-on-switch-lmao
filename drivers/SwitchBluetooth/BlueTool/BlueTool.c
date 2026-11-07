// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stand-in for /usr/sbin/BlueTool on the Switch, shown over the original through the Switch overlay
// (/private/var/Switch/overlay/usr/sbin/BlueTool, see tree/overlay/manifest.json). Built for iOS arm64, ad hoc
// signed as com.apple.BlueTool with the original's entitlements; its CDHash goes into the static trust cache.
//
// On an iPad, bluetoothd's chip boot (21H461 bluetoothd FUN_1002ef860 / FUN_1002f0268) sends XPC messages to the
// Mach service com.apple.BlueTool, which launchd starts from com.apple.BlueTool.plist as "BlueTool -R" (user mobile):
//   {kMsgId: "RunBuiltinScript", kMsgArgs: {script: "boot" | "init" | "deepsleep", btaddress?}}
//       BlueTool powers the chip, opens the UART and downloads its compiled-in .hcd for the (ProductHash, LMP
//       subversion, WifiVendor) it recognises; reply {result, manufacturer, firmware}.
//   {kMsgId: "RunCommand", kMsgArgs: {command}}   reply {result, output?}
//   {kMsgId: "RunExternalScript", kMsgArgs: {script}}   reply {result}
// (the original's handler FUN_1000465b4). bluetoothd reads "result" (0 = done; 10 or 13 = unknown
// platform/chip/vendor, which makes it restart, FUN_1002efcdc), "firmware" (kept as the firmware name) and
// "manufacturer" (logged only). The original knows no BCM4356A3, so it can only fail here. On the Switch the kernel
// (drivers/SwitchBluetooth) has already powered the chip, switched it to 3 Mbaud and loaded its patch RAM, and
// brings it up again whenever bluetoothd reconnects after using it; every request therefore succeeds without
// touching the hardware. The firmware name is the patch's own version string, in the "<chip>_<a>.<b>.<c>.<d>" form
// bluetoothd's version parsing expects.
//
// It also starts the Joy-Con pairing agent, /usr/libexec/joyconpaird (../../SwitchJoyCon/helper/joyconpaird.c), after
// answering each of bluetoothd's chip-boot requests: bluetoothd is then up, and this is the one process of ours launchd
// starts (Apple's com.apple.BlueTool plist). launchd does not load LaunchDaemons plists from the Data overlay
// (it bootstraps from its cache, launchd_cache_loader: com.brunostjohn.switch.joyconnfc never ran on 11-04). For that the
// stand-in has no sandbox profile (the original's com.apple.private.sandbox.profile:embedded "BlueTool" would refuse
// the spawn); it touches nothing but its XPC listener and that one spawn. Children are not waited for (SIGCHLD
// ignored): joyconpaird runs for the boot and keeps a single instance itself.
#include <dispatch/dispatch.h>
#include <errno.h>
#include <notify.h>
#include <os/log.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <xpc/xpc.h>

// Public on macOS, private (but exported by libxpc and used by every iOS daemon) on iOS.
extern xpc_connection_t
ios_xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue,
                                       uint64_t flags) __asm__("_xpc_connection_create_mach_service");

static const char kService[] = "com.apple.BlueTool";
static const char kFirmware[] = "CYW4356A3_001.004.009.0092.0095.hcd";  // nx-firmware patch, loaded by the kernel
static const char kPairingAgent[] = "/usr/libexec/joyconpaird";         // shown by the overlay unless switch-no-bt

extern char **environ;

// The pairing agent, after a chip-boot request has been answered (see the file comment): at every one, so a bluetoothd
// restart (which ends the agent's session) gets a new agent; one already running keeps its lock and the new one exits.
// Without the overlay's binary there is nothing to start.
static void startPairingAgent(void)
{
    if (access(kPairingAgent, X_OK))
        return;
    char *argv[] = {(char *)kPairingAgent, NULL};
    pid_t pid;
    const int error = posix_spawn(&pid, kPairingAgent, NULL, NULL, argv, environ);
    if (error)
        os_log_error(OS_LOG_DEFAULT, "BlueTool: %{public}s not started: %d", kPairingAgent, error);
}

#if SWITCH_DEBUG
// Debug builds: the GPU context-switch stress (drivers/TegraGM20BMetal/raw/test/fetchform.cpp), once per Darwin notification
// the Mac posts (`idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.ctxsw-stress`). Nothing else starts
// it; it runs a bounded time and exits.
static const char kStress[] = "/usr/bin/switch-fetchform";

static void watchStress(void)
{
    int token;
    notify_register_dispatch("com.brunostjohn.switch.debug.ctxsw-stress", &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        os_log(OS_LOG_DEFAULT, "BlueTool: ctxsw-stress posted, %{public}s %{public}s", kStress,
               access(kStress, X_OK) ? "missing" : "starting");
        if (access(kStress, X_OK))
            return;
        char *argv[] = {(char *)kStress, NULL};
        pid_t pid;
        const int error = posix_spawn(&pid, kStress, NULL, NULL, argv, environ);
        if (error)
            os_log_error(OS_LOG_DEFAULT, "BlueTool: %{public}s not started: %d", kStress, error);
    });
}

// Debug builds: the SoC watchdog bench (kernel/switch/shutdown/shutdown.c), once per Darwin notification the Mac posts
// (`idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.watchdog-bench`): the watchdog armed for 20 s
// and never petted. Nothing else arms it.
static void watchWatchdogBench(void)
{
    int token;
    notify_register_dispatch(
        "com.brunostjohn.switch.debug.watchdog-bench", &token, dispatch_get_main_queue(), ^(int t) {
            (void)t;
            int seconds = 20;
            const int error =
                sysctlbyname("debug.switch_watchdog_bench", NULL, NULL, &seconds, sizeof(seconds)) ? errno : 0;
            os_log(OS_LOG_DEFAULT, "BlueTool: watchdog-bench posted: SoC watchdog %d s, %{public}s", seconds,
                   error ? strerror(error) : "armed");
        });
}

// Debug builds: window A's column-scan experiment (drivers/NativeMobileDisplay/NativeColumnScan.h), once per Darwin
// notification the Mac posts: com.brunostjohn.switch.debug.column-scan scans the 4K test frame in the UI's 16-GOB
// blocks, ...column-scan-nvdec in NVDEC's 2-GOB ones. The kernel logs the result.
static void watchColumnScan(const char *name, int blockHeightLog2)
{
    int token;
    notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        int value = blockHeightLog2;
        const int error =
            sysctlbyname("debug.switch_display_column_scan", NULL, NULL, &value, sizeof(value)) ? errno : 0;
        os_log(OS_LOG_DEFAULT, "BlueTool: %{public}s posted: %{public}s", name, error ? strerror(error) : "started");
    });
}
#endif

#if SWITCH_DEBUG
// Debug builds: the GPU's clock gating off and on again for a frame-time A/B within one boot (drivers/TegraGM20B
// debug.switch_gpu_clkgate), once per Darwin notification the Mac posts
// (`idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.gpu-clkgate-off` / `-on`).
static void watchClkgate(const char *name, int on)
{
    int token;
    notify_register_dispatch(name, &token, dispatch_get_main_queue(), ^(int t) {
        (void)t;
        int value = on;
        const int error = sysctlbyname("debug.switch_gpu_clkgate", NULL, NULL, &value, sizeof(value)) ? errno : 0;
        os_log(OS_LOG_DEFAULT, "BlueTool: %{public}s posted: GPU clock gating %{public}s, %{public}s", name,
               on ? "on" : "off", error ? strerror(error) : "done");
    });
}
#endif

static void answer(xpc_connection_t peer, xpc_object_t message)
{
    if (xpc_get_type(message) != XPC_TYPE_DICTIONARY)
        return;  // errors: the peer went away
    xpc_object_t reply = xpc_dictionary_create_reply(message);
    if (!reply)
        return;
    const char *id = xpc_dictionary_get_string(message, "kMsgId");
    xpc_dictionary_set_int64(reply, "result", 0);
    if (id && !strcmp(id, "RunBuiltinScript")) {
        xpc_dictionary_set_int64(reply, "manufacturer", 0);
        xpc_dictionary_set_string(reply, "firmware", kFirmware);
    }
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
    if (id && !strcmp(id, "RunBuiltinScript"))
        startPairingAgent();
}

int main(void)
{
    // "-R" (server mode) is the only way launchd runs it; arguments are ignored.
    signal(SIGCHLD, SIG_IGN);  // the pairing agent is never waited for
    xpc_connection_t listener =
        ios_xpc_connection_create_mach_service(kService, NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
            return;
        xpc_connection_t connection = (xpc_connection_t)peer;
        xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
            answer(connection, message);
        });
        xpc_connection_resume(connection);
    });
    xpc_connection_resume(listener);
#if SWITCH_DEBUG
    watchStress();
    watchWatchdogBench();
    watchColumnScan("com.brunostjohn.switch.debug.column-scan", 4);
    watchColumnScan("com.brunostjohn.switch.debug.column-scan-nvdec", 1);
    watchClkgate("com.brunostjohn.switch.debug.gpu-clkgate-off", 0);
    watchClkgate("com.brunostjohn.switch.debug.gpu-clkgate-on", 1);
#endif
    dispatch_main();
}
