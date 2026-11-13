// SPDX-License-Identifier: MIT
// /usr/libexec/SwitchShaderCompiler: the system shader compiler's process. The compiler is the GPU plugin's own code
// (compiler_service.mm), so it compiles with the very m2v and NVK every GPU client links; this holds the one-instance lock,
// loads the plugin and runs it. The BlueTool stand-in starts it on iOS 17 (as mobile), launchd on iOS 27
// (com.brunostjohn.switch.shadercompiler.plist, as root). Ad hoc signed with com.brunostjohn.switch.shader-compiler, which the
// kernel asks of the one task it lets register a compiler, or decline to be one.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <os/log.h>
#include <stdbool.h>
#include <sys/file.h>
#include <unistd.h>

static const char kPlugin[] = "/System/Library/Extensions/TegraGM20BMetal.bundle/TegraGM20BMetal";
static const char kLockPath[] = "/private/var/tmp/com.brunostjohn.switch.shadercompiler.lock";
static const uint32_t kOpenType = 0x4e56;  // TegraGM20BABI.h TegraGM20BOpenType
static const uint32_t kDecline = 0x5348;   // TegraGM20BCompilerABI.h TegraGM20BMethodCompilerDecline

static bool told;

// Without the plugin no compiler comes this boot: the kernel is told so once the GPU's service is up, so no client waits
static void declined(void *refcon, io_iterator_t iterator)
{
    (void)refcon;
    io_object_t service = IOIteratorNext(iterator);
    if (!service || told)
        return;
    told = true;
    io_connect_t connection = 0;
    if (IOServiceOpen(service, mach_task_self(), kOpenType, &connection) == KERN_SUCCESS) {
        IOConnectCallStructMethod(connection, kDecline, NULL, 0, NULL, NULL);
        IOServiceClose(connection);
    }
    IOObjectRelease(service);
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static void decline(void)
{
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    io_iterator_t matches = 0;
    if (!port || IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, IOServiceMatching("AGXAccelerator"),
                                                  declined, NULL, &matches) != KERN_SUCCESS)
        return;
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
    declined(NULL, matches);
    if (!told)
        CFRunLoopRun();
}

int main(void)
{
    const int lock = open(kLockPath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
        return 0;
    void *plugin = dlopen(kPlugin, RTLD_NOW | RTLD_LOCAL);
    int (*serve)(void) = plugin ? (int (*)(void))dlsym(plugin, "switch_shader_compiler_main") : NULL;
    if (!serve) {
        os_log_error(OS_LOG_DEFAULT, "SwitchShaderCompiler: no compiler in %{public}s: %{public}s", kPlugin, dlerror());
        decline();
        return 0;
    }
    return serve();
}
