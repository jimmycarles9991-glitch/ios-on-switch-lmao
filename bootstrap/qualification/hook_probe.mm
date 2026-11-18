// SPDX-License-Identifier: BSD-2-Clause-Patent
#if SWITCH_DEBUG
#import <Foundation/Foundation.h>

#include <atomic>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <string.h>

using SwitchPhysicalMemory = unsigned long long (*)(id, SEL);
using SwitchHookFunction = void (*)(void *, void *, void **);

static std::atomic<SwitchPhysicalMemory> switch_probe_original{nullptr};
static std::atomic<unsigned long long> switch_probe_memory{0};
static thread_local bool switch_probe_called_original;

static unsigned long long switch_probe_replacement(id receiver, SEL selector)
{
    SwitchPhysicalMemory original = switch_probe_original.load(std::memory_order_acquire);
    // ElleKit publishes the trampoline after replacing the entry point.
    if (!original)
        return switch_probe_memory.load(std::memory_order_acquire);
    const unsigned long long result = original(receiver, selector);
    switch_probe_called_original = true;
    return result;
}

static const char *switch_probe_run(const char **targetImage, int *targetSubtype)
{
    NSProcessInfo *receiver = NSProcessInfo.processInfo;
    const SEL selector = @selector(physicalMemory);
    const Method method = class_getInstanceMethod(object_getClass(receiver), selector);
    const char *encoding = method ? method_getTypeEncoding(method) : nullptr;
    if (!encoding || encoding[0] != 'Q' || method_getNumberOfArguments(method) != 2)
        return "unexpected physicalMemory signature";
    const IMP target = method_getImplementation(method);
    Dl_info image = {};
    if (!target || !dladdr(reinterpret_cast<const void *>(target), &image) || !image.dli_fbase || !image.dli_fname)
        return "missing target image";
    *targetImage = image.dli_fname;
    const auto *header = static_cast<const mach_header_64 *>(image.dli_fbase);
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64)
        return "unexpected target architecture";
    *targetSubtype = header->cpusubtype;
    if (strcmp(image.dli_fname, "/System/Library/Frameworks/Foundation.framework/Foundation"))
        return "target is outside Apple Foundation";

    void *ellekit = dlopen("/var/jb/usr/lib/libellekit.dylib", RTLD_NOW | RTLD_LOCAL);
    const auto hook = ellekit ? reinterpret_cast<SwitchHookFunction>(dlsym(ellekit, "MSHookFunction")) : nullptr;
    if (!hook)
        return "missing ElleKit MSHookFunction";
    const auto invoke = reinterpret_cast<SwitchPhysicalMemory>(target);
    const unsigned long long expected = invoke(receiver, selector);
    if (!expected)
        return "invalid original physicalMemory";
    switch_probe_memory.store(expected, std::memory_order_release);

    void *trampoline = nullptr;
    hook(reinterpret_cast<void *>(target), reinterpret_cast<void *>(switch_probe_replacement), &trampoline);
    const auto original = reinterpret_cast<SwitchPhysicalMemory>(trampoline);
    if (!original || original == invoke || original == switch_probe_replacement)
        return "missing original trampoline";
    switch_probe_original.store(original, std::memory_order_release);
    switch_probe_called_original = false;
    const unsigned long long actual = invoke(receiver, selector);
    return switch_probe_called_original && actual == expected ? nullptr : "hook or original result was not preserved";
}

__attribute__((constructor)) static void switch_hook_probe(void)
{
    @autoreleasepool {
        NSString *bundle = NSBundle.mainBundle.bundleIdentifier.lowercaseString;
        if (![bundle isEqualToString:@"org.coolstar.sileostore"] && ![bundle isEqualToString:@"com.apple.springboard"])
            return;
        const char *image = "unknown";
        int subtype = -1;
        const char *failure = switch_probe_run(&image, &subtype);
        os_log_with_type(OS_LOG_DEFAULT, failure ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
                         "SwitchHookProbe: %{public}@ %{public}s image=%{public}s cpu_subtype=%#x", bundle,
                         failure ?: "original call preserved", image, static_cast<unsigned>(subtype));
    }
}
#endif
