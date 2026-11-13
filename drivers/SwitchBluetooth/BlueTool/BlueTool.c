// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stand-in for /usr/sbin/BlueTool on the Switch, shown over the original through the Switch overlay
// (/private/var/Switch/overlay/usr/sbin/BlueTool, see tree/overlay/manifest.json). Built for iOS arm64, ad hoc
// signed as com.apple.BlueTool with the original's entitlements (iOS 17: BlueTool.entitlements, without its sandbox
// profile; iOS 27: BlueTool27.entitlements, with it); its CDHash goes into the static trust cache.
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
// (it bootstraps from its cache, launchd_cache_loader: com.brunostjohn.switch.joyconnfc never ran on 11-04). For the
// same reason it starts /usr/libexec/switch-appregd (../../SwitchAppRegistration/appregd.mm) when launchd starts it,
// which registers the apps a deploy changed. For that the stand-in has no sandbox profile (the original's
// com.apple.private.sandbox.profile:embedded "BlueTool" would refuse the spawn); it touches nothing but its XPC
// listener and those spawns. Children are not waited for (SIGCHLD ignored): joyconpaird runs for the boot and keeps a
// single instance itself, and switch-appregd ends once it has applied the deploy's journal. It starts the system shader
// compiler, /usr/libexec/SwitchShaderCompiler (../../TegraGM20BMetal/compiler), as soon as it runs and again at each chip
// boot (one still running keeps its lock and the new one exits), as an interactive daemon: a compile runs at the class of
// the GPU client waiting on it. It gives it the compiler's jetsam band and limits (SwitchShaderCompilerJetsam.h), which a
// child spawned without them lacks: it would sit in JETSAM_PRIORITY_DEFAULT (180, above backboardd) with the system's
// task limit. It watches each compiler it starts and starts it again when one dies (killed, or a
// failing exit), up to the kernel's three losses (SwitchGpuShaderCompilerPolicy.h kLosses); an exit of 0 is one that
// found another running or declined, and is left alone.
//
// iOS 27 sandboxes it either way. Without a seatbelt profile the kernel applies the original's learned autobox profile
// ("apply-protobox", keyed by the executable name), so the 27 build carries the original's seatbelt-profiles
// ["BlueTool"] and runs as the original does. Neither profile reaches bluetoothd or allows an exec, and 24B119 kills an
// untrusted binary that launchd did not start, so the 27 build starts neither (SWITCH_BLUETOOL_STARTS_HELPERS is 17's);
// launchd starts both there.
#include <dispatch/dispatch.h>
#include <errno.h>
#include <notify.h>
#include <os/lock.h>
#include <os/log.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xpc/xpc.h>
#if SWITCH_BLUETOOL_STARTS_HELPERS
#include "../../TegraGM20BMetal/compiler/SwitchShaderCompilerJetsam.h"
#endif

// Public on macOS, private (but exported by libxpc and used by every iOS daemon) on iOS.
extern xpc_connection_t
ios_xpc_connection_create_mach_service(const char *name, dispatch_queue_t queue,
                                       uint64_t flags) __asm__("_xpc_connection_create_mach_service");

static const char kService[] = "com.apple.BlueTool";
static const char kFirmware[] = "CYW4356A3_001.004.009.0092.0095.hcd";  // nx-firmware patch, loaded by the kernel
#if SWITCH_BLUETOOL_STARTS_HELPERS
static const char kPairingAgent[] = "/usr/libexec/joyconpaird";
static const char kAppRegistration[] = "/usr/libexec/switch-appregd";
static const char kShaderCompiler[] = "/usr/libexec/SwitchShaderCompiler";
// compiler_service.mm: the compiler started with it set says nothing starts it again
static const char kShaderCompilerLastStart[] = "SWITCH_SHADER_COMPILER_LAST_START";

// libsystem_kernel (spawn_private.h) and xnu's bsd/sys/spawn_internal.h POSIX_SPAWN_PROC_TYPE_DAEMON_INTERACTIVE: a daemon
// whose threads may run at the class of the client threads waiting on it, as launchd's ProcessType Interactive
extern int posix_spawnattr_setprocesstype_np(posix_spawnattr_t *attr, const int type);
static const int kDaemonInteractive = 0x400;
// spawn_private.h; flags: xnu's spawn_internal.h POSIX_SPAWN_JETSAM_MEMLIMIT_INACTIVE_FATAL, the active limit soft
extern int posix_spawnattr_setjetsam_ext(posix_spawnattr_t *attr, short flags, int priority, int memlimit_active,
                                         int memlimit_inactive);
static const short kInactiveLimitFatal = 0x08;
#endif

extern char **environ;

#if SWITCH_BLUETOOL_STARTS_HELPERS
// The overlay shows both helpers whenever it shows this stand-in, so a failed access is an error.
static void startHelper(const char *path, bool *reported)
{
    if (access(path, X_OK)) {
        if (!*reported)
            os_log_error(OS_LOG_DEFAULT, "BlueTool: %{public}s not started: access: %{errno}d", path, errno);
        *reported = true;
        return;
    }
    char *argv[] = {(char *)path, NULL};
    pid_t pid;
    const int error = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (error)
        os_log_error(OS_LOG_DEFAULT, "BlueTool: %{public}s not started: %d", path, error);
}

// As launchd's KeepAlive, the compiler is started again whenever it dies, but for a crash loop: three deaths in a row each
// within launchd's throttle interval of its start. The start that could be the loop's last tells the kernel nothing
// starts it again, so the kernel ends what waits on it when it dies (SwitchGpuShaderCompilerPolicy.h).
static const int kCompilerDeaths = 3;
static const uint64_t kCompilerThrottleNs = 10ull * NSEC_PER_SEC;
static int compilerDeaths;
static uint64_t compilerStartedNs;

static void startShaderCompiler(void);

// The child's exit, from a kqueue the main queue reads when it fires: its wait status comes with it (NOTE_EXITSTATUS, as
// its parent), whatever SIGCHLD's disposition reaps. A child gone before the watch is set is left alone.
static void watchShaderCompiler(pid_t pid)
{
    const int kq = kqueue();
    if (kq < 0)
        return;
    struct kevent watch;
    EV_SET(&watch, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT | NOTE_EXITSTATUS, 0, NULL);
    if (kevent(kq, &watch, 1, NULL, 0, NULL) < 0) {
        close(kq);
        return;
    }
    dispatch_source_t exited =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)kq, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(exited, ^{
        struct kevent event;
        const struct timespec none = {0, 0};
        if (kevent(kq, NULL, 0, &event, 1, &none) != 1)
            return;
        dispatch_source_cancel(exited);
        const int status = (int)event.data;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            return;
        const bool quick = clock_gettime_nsec_np(CLOCK_MONOTONIC) - compilerStartedNs < kCompilerThrottleNs;
        compilerDeaths = quick ? compilerDeaths + 1 : 0;
        os_log_error(OS_LOG_DEFAULT, "BlueTool: %{public}s (pid %d) died (status 0x%x), death %d of %d",
                     kShaderCompiler, pid, status, compilerDeaths, kCompilerDeaths);
        startShaderCompiler();
    });
    dispatch_source_set_cancel_handler(exited, ^{
        close(kq);
    });
    dispatch_resume(exited);
}

static void startShaderCompiler(void)
{
    if (compilerDeaths >= kCompilerDeaths)
        return;
    char *argv[] = {(char *)kShaderCompiler, NULL};
    posix_spawnattr_t attributes;
    pid_t pid;
    const bool last = compilerDeaths == kCompilerDeaths - 1;
    if (last)
        setenv(kShaderCompilerLastStart, "1", 1);
    int error = posix_spawnattr_init(&attributes);
    if (!error) {
        error = posix_spawnattr_setprocesstype_np(&attributes, kDaemonInteractive);
        if (!error)
            error = posix_spawnattr_setjetsam_ext(
                &attributes, kInactiveLimitFatal, SWITCH_SHADER_COMPILER_JETSAM_PRIORITY,
                SWITCH_SHADER_COMPILER_ACTIVE_LIMIT_MB, SWITCH_SHADER_COMPILER_INACTIVE_LIMIT_MB);
        if (!error)
            error = posix_spawn(&pid, kShaderCompiler, NULL, &attributes, argv, environ);
        posix_spawnattr_destroy(&attributes);
    }
    if (last)
        unsetenv(kShaderCompilerLastStart);
    compilerStartedNs = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (error)
        os_log_error(OS_LOG_DEFAULT, "BlueTool: %{public}s not started: %d", kShaderCompiler, error);
    else
        watchShaderCompiler(pid);
}

// The pairing agent, after a chip-boot request has been answered (see the file comment): at every one, so a bluetoothd
// restart (which ends the agent's session) gets a new agent; one already running keeps its lock and the new one exits.
static void startPairingAgent(void)
{
    static bool reported;
    startHelper(kPairingAgent, &reported);
}
#endif

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

#if SWITCH_DEBUG && SWITCH_BLUETOOL_STARTS_HELPERS
// Debug builds: HW Test's second process for shared textures (hw_test_app/peer/texpeer.mm). The sandboxed app can start no
// process, so it sends {kMsgId: "SwitchTexturePeer", endpoint: its anonymous listener's}; this starts the peer and keeps
// the endpoint until that process asks for it ({kMsgId: "SwitchTexturePeerEndpoint"}), then the peer connects to the app.
extern pid_t ios_xpc_connection_get_pid(xpc_connection_t connection) __asm__("_xpc_connection_get_pid");

static const char kTexturePeer[] = "/usr/bin/switch-texpeer";
static os_unfair_lock gPeerLock = OS_UNFAIR_LOCK_INIT;
static xpc_object_t gPeerEndpoint;
static pid_t gPeerPid;

static void startTexturePeer(xpc_object_t message, xpc_object_t reply)
{
    xpc_object_t endpoint = xpc_dictionary_get_value(message, "endpoint");
    int error = endpoint && xpc_get_type(endpoint) == XPC_TYPE_ENDPOINT ? 0 : EINVAL;
    pid_t pid = 0;
    os_unfair_lock_lock(&gPeerLock);
    if (!error) {
        char *argv[] = {(char *)kTexturePeer, NULL};
        error = posix_spawn(&pid, kTexturePeer, NULL, NULL, argv, environ);
    }
    if (!error) {
        if (gPeerEndpoint)
            xpc_release(gPeerEndpoint);
        gPeerEndpoint = xpc_retain(endpoint);
        gPeerPid = pid;
    }
    os_unfair_lock_unlock(&gPeerLock);
    os_log(OS_LOG_DEFAULT, "BlueTool: texture peer %{public}s: %d (pid %d)", kTexturePeer, error, pid);
    xpc_dictionary_set_int64(reply, "result", error);
    xpc_dictionary_set_int64(reply, "pid", pid);
}

static void giveTexturePeerEndpoint(xpc_connection_t peer, xpc_object_t reply)
{
    os_unfair_lock_lock(&gPeerLock);
    const bool started = gPeerEndpoint && ios_xpc_connection_get_pid(peer) == gPeerPid;
    if (started) {
        xpc_dictionary_set_value(reply, "endpoint", gPeerEndpoint);
        xpc_release(gPeerEndpoint);
        gPeerEndpoint = NULL;
    }
    os_unfair_lock_unlock(&gPeerLock);
    xpc_dictionary_set_int64(reply, "result", started ? 0 : EPERM);
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
#if SWITCH_DEBUG && SWITCH_BLUETOOL_STARTS_HELPERS
    if (id && !strcmp(id, "SwitchTexturePeer"))
        startTexturePeer(message, reply);
    if (id && !strcmp(id, "SwitchTexturePeerEndpoint"))
        giveTexturePeerEndpoint(peer, reply);
#endif
    xpc_connection_send_message(peer, reply);
    xpc_release(reply);
#if SWITCH_BLUETOOL_STARTS_HELPERS
    if (id && !strcmp(id, "RunBuiltinScript")) {
        startPairingAgent();
        startShaderCompiler();
    }
#endif
}

int main(void)
{
    // "-R" (server mode) is the only way launchd runs it; arguments are ignored.
    signal(SIGCHLD, SIG_IGN);  // the helpers are never waited for
#if SWITCH_BLUETOOL_STARTS_HELPERS
    startShaderCompiler();
    static bool appRegistrationReported;
    startHelper(kAppRegistration, &appRegistrationReported);
#endif
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
