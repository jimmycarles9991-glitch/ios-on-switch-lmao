// SPDX-License-Identifier: MIT
// TegraGM20B Metal plugin: Metal on the Tegra X1 GPU (GM20B) for iPadOS 17.7.11, reached through the original discovery:
// IOServiceMatching("IOAcceleratorES") -> AGXAccelerator (drivers/TegraGM20B, published unless switch-no-gpu) -> MetalPluginName
// TegraGM20BMetal -> /System/Library/Extensions/TegraGM20BMetal.bundle -> MetalPluginClassName TegraGM20BMetalDevice ->
// -initWithAcceleratorPort:. That returns the facade (TegraGM20BMetal.h) over the accelerator's
// AGXDeviceUserClient, on a connection of its own (kernel.cpp Kernel::open).
//
// Adapted from UniXNU-sheng's drivers/NativeMetal/NativeAdrenoMetalPlugin.mm (published mode): the same device open, cache
// directory search and render-server preferences, without the Adreno user test, the marker stream, the scanout hand-off
// (CA's frames reach the panel through IOMobileFramebuffer and NativeLegacyDisplay as they do today), the benchmark,
// telemetry and cache-export threads, and the direct writes into QuartzCore's state.
//
// Only the processes the accelerator's NativeSandboxGrantProcess admits get a device: the render server (backboardd), the
// screenshot service (ScreenshotServicesService: PencilKit aborts its thumbnail without one) and
// the icon service (iconservicesagent: CoreUI draws every graphic icon, Settings' rows among them, through Core Image, whose
// -[CIContext initWithOptions:] in 21H461 has Metal or GLES and no CPU renderer; without Metal it makes a GLES context that does
// not exist and crashes at once, and launchd's respawns hang Settings) and, by default, every other process (the kernel's
// grant admits them too; TEGRA_GPU_ALL_CLIENTS=0 limits it to those named here). A process left out gets nil from here and
// keeps rendering on the CPU, as does one whose device does not open (device_open.h: one error line naming the cause's
// class); Metal's next ask opens afresh. The Metal source and stitched libraries Core Image makes are compiled by the
// shader compiler daemon (compiler/).
//
// Knob, an environment variable of the process (not a boot-arg):
//   TEGRA_GPU_ALL_CLIENTS=0   only the processes named above get a device (default: every process)
//
// The driver (TegraGM20BMetal.h): when the build's pipeline pack is staged beside the bundle
// (<Extensions>/TegraGM20BMetal.pack/pack.bin, build-tegra-gm20b-metal.py makes it, stage-tegra-gm20b-metal.py copies it) and the
// process's sandbox lets it read it, the device every process gets is the facade: its command buffers go straight to
// MAXWELL_B methods, and one the driver cannot run fails with Metal's error. The render
// server's raw device is set up on a thread of its own while the plugin opens, every other process's memory, channel and
// shader heap at their first use (gpu.h Device::Role). A pack of another build is
// refused (its stack key, TEGRA_STACK_KEY, differs). A process that cannot read the pack gets no device.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <errno.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include "TegraGM20BMetal.h"
#include "device_open.h"

// The build's stack key (build-tegra-gm20b-metal.py); 0 takes a pack of any build (never shipped so).
#ifndef TEGRA_STACK_KEY
#define TEGRA_STACK_KEY 0
#endif

@interface _MTLDevice : NSObject
@end
@interface TegraGM20BMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

namespace {
os_log_t logger()
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "plugin");
    return log;
}

bool knobOn(const char *env, bool otherwise)
{
    const char *value = getenv(env);
    return value ? strcmp(value, "0") != 0 : otherwise;
}

// Whether this process's sandbox, if it has one, lets it do `operation` with `argument`, asked without a report (sandbox_check with
// SANDBOX_CHECK_NO_REPORT, libsystem_sandbox; the way Apple's own code asks before it tries): every lookup, read and write below that a
// sandbox may refuse is asked first, so a refusing sandbox (WebKit's GPU process: no containermanagerd, no /private/var/tmp) never
// reports one of ours. Not sandboxed: always yes.
extern "C" int sandbox_check(pid_t pid, const char *operation, int type, ...);
extern "C" const int SANDBOX_CHECK_NO_REPORT;
constexpr int kSandboxFilterPath = 1, kSandboxFilterGlobalName = 2;  // SANDBOX_FILTER_PATH, SANDBOX_FILTER_GLOBAL_NAME

bool sandboxAllows(const char *operation, int filter, const char *argument)
{
    return sandbox_check(getpid(), operation, filter | SANDBOX_CHECK_NO_REPORT, argument) == 0;
}

// Whether the sandbox lets this process create `dir` and files in it: file-write-create asked of `dir`'s deepest existing ancestor
// (sandbox_check answers no for a path that does not exist yet, whatever the profile).
bool sandboxMayCreate(const char *dir)
{
    char path[1024];
    strlcpy(path, dir, sizeof path);
    struct stat st;
    while (stat(path, &st) != 0) {
        char *slash = strrchr(path, '/');
        if (!slash || slash == path) {
            strlcpy(path, "/", sizeof path);
            break;
        }
        *slash = 0;
    }
    return sandboxAllows("file-write-create", kSandboxFilterPath, path);
}

// `path` and its missing parents, owner-only.
void makeDirectories(const char *path)
{
    char partial[1024];
    strlcpy(partial, path, sizeof partial);
    for (char *slash = strchr(partial + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        struct stat st;
        if (stat(partial, &st) != 0)
            mkdir(partial, 0700);
        *slash = '/';
    }
    mkdir(partial, 0700);
}

// Whether this process can write in `dir`, made if missing: a probe file is created there and removed. access(2) would only ask the
// file's mode; the sandbox decides at the create.
bool writable(const char *dir)
{
    makeDirectories(dir);
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/.probe.%d", dir, getpid());
    const int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    const bool wrote = write(fd, "probe", 5) == 5;
    const int failure = errno;
    close(fd);
    unlink(probe);
    errno = wrote ? 0 : failure;
    return wrote;
}

extern "C" XPC_RETURNS_RETAINED xpc_object_t xpc_copy_entitlement_for_self(const char *key);

bool entitledToSystemContainer(const char *identifier)
{
    const auto matches = [&](const char *key, bool allowWildcard) -> bool {
        xpc_object_t value = xpc_copy_entitlement_for_self(key);
        if (!value)
            return false;
        __block bool ok = false;
        const xpc_type_t type = xpc_get_type(value);
        if (type == XPC_TYPE_BOOL) {
            ok = xpc_bool_get_value(value);
        } else if (type == XPC_TYPE_STRING) {
            const char *s = xpc_string_get_string_ptr(value);
            ok = s && (!strcmp(s, identifier) || (allowWildcard && !strcmp(s, "*")));
        } else if (type == XPC_TYPE_ARRAY) {
            xpc_array_apply(value, ^bool(size_t, xpc_object_t item) {
                if (xpc_get_type(item) == XPC_TYPE_STRING) {
                    const char *s = xpc_string_get_string_ptr(item);
                    if (s && (!strcmp(s, identifier) || (allowWildcard && !strcmp(s, "*")))) {
                        ok = true;
                        return false;
                    }
                }
                return true;
            });
        }
#if !__has_feature(objc_arc)
        xpc_release(value);
#endif
        return ok;
    };
    return matches("com.apple.security.system-container", false) ||
           matches("com.apple.private.security.system-container", false) ||
           matches("com.apple.security.system-containers", true) ||
           matches("com.apple.private.security.storage.SystemContainers", false) ||
           matches("com.apple.private.MobileContainerManager.allowed", false);
}

// <process system container>/Library/Caches/TegraGM20BMetal. The container survives reboots and the process's sandbox writes it once
// container_system_path_for_identifier (libsystem_containermanager: identifier, error out; a malloc'd path) has consumed its
// extension; backboardd's StartWindowServer makes the same lookup for Core Animation's shader cache before Metal opens. The
// identifier is the main bundle's, else com.apple.<process name> (backboardd's and iconservicesagent's own).
bool systemContainerCache(char *out, size_t size)
{
    static auto lookup =
        (char *(*)(const char *, uint64_t *))dlsym(RTLD_DEFAULT, "container_system_path_for_identifier");
    if (!lookup)
        return false;
    // A sandbox that refuses containermanagerd (WebKit's GPU process) would refuse each of libsystem_containermanager's six tries.
    if (!sandboxAllows("mach-lookup", kSandboxFilterGlobalName, "com.apple.containermanagerd.system"))
        return false;
    char identifier[256];
    CFStringRef bundle = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (!bundle || !CFStringGetCString(bundle, identifier, sizeof identifier, kCFStringEncodingUTF8))
        snprintf(identifier, sizeof identifier, "com.apple.%s", getprogname());
    // Unentitled daemons such as iconservicesagent and cameracaptured can reach containermanagerd.system over Mach,
    // which triggers a containermanagerd fault if they ask for a systemData container without the entitlement.
    if (!entitledToSystemContainer(identifier))
        return false;
    uint64_t error = 0;
    char *container = lookup(identifier, &error);
    if (!container)
        return false;
    snprintf(out, size, "%s/Library/Caches/TegraGM20BMetal", container);
    free(container);
    return true;
}

// The record cache directories to try, most persistent first; each is this process's alone:
//  - the user cache directory when it is inside a data container (an app's own Library/Caches);
//  - the process's system container (above): backboardd's;
//  - the user cache directory elsewhere (a daemon with a home);
//  - the Library/Caches of the data container whose tmp is the temp directory, as m2v-<process> (WebKit's GPU process writes Safari's
//    container, a poster extension its own: iOS may clear a container's tmp, never its Library/Caches but under storage pressure, so
//    their pipelines no longer compile again after a reboot; r7n/r7o: the GPU process's and AegirPoster's caches were in tmp/TegraGM20BMetal);
//  - /private/var/tmp/TegraGM20BMetal-<process> (iconservicesagent, whose home is /var/empty), then the user temp directory, then $TMPDIR (WebKit's
//    GPU process: its per-user temp directory is the one its sandbox extension covers).
// One the sandbox would refuse is left out, asked without a report (sandboxMayCreate): the order of the others is unchanged.
// First writable record cache directory in priority order, short-circuiting before containermanagerd or temp probing
// when an earlier candidate is already writable.
bool findRecordCacheDir(char *out, size_t size)
{
    char tried[7][512];
    unsigned count = 0;
    auto tryDir = [&](const char *format, const char *a, const char *b) -> bool {
        if (count >= 7)
            return false;
        char candidate[512];
        snprintf(candidate, sizeof candidate, format, a, b);
        for (unsigned i = 0; i < count; ++i)
            if (!strcmp(tried[i], candidate))
                return false;
        strlcpy(tried[count++], candidate, sizeof tried[0]);
        if (!sandboxMayCreate(candidate) || !writable(candidate))
            return false;
        strlcpy(out, candidate, size);
        return true;
    };
    char user[512] = {}, temp[512] = {}, container[512] = {};
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, user, sizeof user))
        user[0] = 0;
    const bool appContainer = strstr(user, "/Containers/Data/") != nullptr;
    if (user[0] && appContainer && tryDir("%s%s", user, "TegraGM20BMetal"))
        return true;
    if (!appContainer && systemContainerCache(container, sizeof container) && tryDir("%s%s", container, ""))
        return true;
    if (user[0] && !appContainer && tryDir("%s%s", user, "TegraGM20BMetal"))
        return true;
    const bool haveTemp = confstr(_CS_DARWIN_USER_TEMP_DIR, temp, sizeof temp) != 0;
    for (const char *dir : {haveTemp ? (const char *)temp : nullptr, (const char *)getenv("TMPDIR")}) {
        // <...>/Containers/Data/<kind>/<UUID>/tmp/... -> <...>/Containers/Data/<kind>/<UUID>/Library/Caches/TegraGM20BMetal-<process>
        const char *data = dir ? strstr(dir, "/Containers/Data/") : nullptr;
        const char *tmp = data ? strstr(data, "/tmp/") : nullptr;
        if (!tmp && data && (tmp = strstr(data, "/tmp")) && tmp[4])
            tmp = nullptr;  // only a container's own tmp
        if (!tmp)
            continue;
        char root[512];
        snprintf(root, sizeof root, "%.*s/Library/Caches/TegraGM20BMetal-", (int)(tmp - dir), dir);
        if (tryDir("%s%s", root, getprogname()))
            return true;
    }
    if (tryDir("%s%s", "/private/var/tmp/TegraGM20BMetal-", getprogname()))
        return true;
    if (haveTemp && tryDir("%s%s", temp, "TegraGM20BMetal"))
        return true;
    if (const char *tmpdir = getenv("TMPDIR"); tmpdir && *tmpdir == '/')
        if (tryDir(tmpdir[strlen(tmpdir) - 1] == '/' ? "%s%s" : "%s/%s", tmpdir, "TegraGM20BMetal"))
            return true;
    return false;
}

// <Extensions>/TegraGM20BMetal.pack/pack.bin, beside the bundle: the driver's pipeline pack. Null if this process
// may read it, else why not (a process whose sandbox may not read it never tries: asked without a report).
const char *gm20bPackRefusal(char *path, size_t size)
{
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&gm20bPackRefusal), &info) || !info.dli_fname)
        return "the plugin's path is unknown";
    strlcpy(path, info.dli_fname, size);
    for (int i = 0; i < 2; ++i)  // <Extensions>/TegraGM20BMetal.bundle/TegraGM20BMetal -> <Extensions>
        if (char *slash = strrchr(path, '/'))
            *slash = 0;
    strlcat(path, "/TegraGM20BMetal.pack/pack.bin", size);
    if (!sandboxAllows("file-read-data", kSandboxFilterPath, path))
        return "its sandbox does not let it read the pack";
    return access(path, R_OK) == 0 ? nullptr : "no pack staged";
}

// The installed native records beside the pack at `pack` (<Extensions>/TegraGM20BMetal.pack/native, RecordCache::attachSeed) into
// `seed`; false if this process's sandbox may not read their index (asked without a report).
bool gm20bNativeSeed(const char *pack, char *seed, size_t size)
{
    const char *slash = strrchr(pack, '/');
    if (!slash)
        return false;
    snprintf(seed, size, "%.*s/native", int(slash - pack), pack);
    char index[1200];
    snprintf(index, sizeof index, "%s/rec-%016llx/index", seed, (unsigned long long)TEGRA_STACK_KEY);
    return sandboxAllows("file-read-data", kSandboxFilterPath, index);
}

#if SWITCH_DEBUG && defined(SWITCH_STATS_FRAME) && SWITCH_STATS_FRAME
#define SWITCH_PLUGIN_FRAME 1
#else
#define SWITCH_PLUGIN_FRAME 0
#endif

#if SWITCH_DEBUG && ((defined(SWITCH_STATS_GPU) && SWITCH_STATS_GPU) || (defined(SWITCH_PROF_GPU) && SWITCH_PROF_GPU))
#define SWITCH_PLUGIN_RATES 1
#else
#define SWITCH_PLUGIN_RATES 0
#endif

#if SWITCH_PLUGIN_FRAME
// SpringBoard's Home Screen labels arrive 50-80 s after its icons at boot (perf1/icon-labels, 25 Nov) and no Apple line
// says when a label's image is set: one line for each of the first kLabelLines images set on an icon label's
// legibility view, with its mach time, for tools/perf's labels row. It logs and calls through, nothing else.
using SetLabelImage = void (*)(id, SEL, id);
SetLabelImage gSetLabelImage;
Class gIconLabelView;
std::atomic<uint32_t> gLabelImages{0};
constexpr uint32_t kLabelLines = 64;

void observedSetLabelImage(id view, SEL cmd, id image)
{
    if (image && [view isKindOfClass:gIconLabelView]) {
        const uint32_t n = gLabelImages.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= kLabelLines)
            os_log(logger(), "SwitchLabels: icon label image %u set at %llu", n, mach_absolute_time());
    }
    gSetLabelImage(view, cmd, image);
}

void observeLabelImages()
{
    static std::atomic<bool> done;
    if (strcmp(getprogname(), "SpringBoard") || done.exchange(true, std::memory_order_relaxed))
        return;
    gIconLabelView = objc_getClass("SBIconPLKLegibilityView");
    Method m = gIconLabelView ? class_getInstanceMethod(gIconLabelView, sel_registerName("setImage:")) : nullptr;
    if (!m) {
        os_log(logger(), "SwitchLabels: no -[SBIconPLKLegibilityView setImage:] in this SpringBoard: no label times");
        return;
    }
    // The original first: a call may land as soon as the method is swapped.
    gSetLabelImage = (SetLabelImage)method_getImplementation(m);
    method_setImplementation(m, (IMP)observedSetLabelImage);
}
#endif

#if SWITCH_PLUGIN_RATES
// This process's name as one word for the rates lines (an app's executable may have spaces), which tools/devicerun reads per
// process
const char *processWord()
{
    static char word[128];
    static std::once_flag made;
    std::call_once(made, [] {
        strlcpy(word, getprogname(), sizeof word);
        for (char *c = word; *c; c++)
            if (*c == ' ')
                *c = '_';
    });
    return word;
}

// The driver's rates lines, every 5 s while the device works (they say nothing when idle). r16-idle: with a whole
// interval of leeway, so the timer rides a wakeup the process or the kernel takes anyway instead of waking every
// Metal process every 5 s; each rates function measures its own window, so a late tick only makes a longer one.
void startRates()
{
    // kprof keys a user PC as main+<offset from the process's main header> within 256 MiB of it, else as "other": the
    // plugin's base beside its main header turns a main+ sample into an offset in this bundle's binary
    if (Dl_info self = {}; dladdr(reinterpret_cast<const void *>(&startRates), &self) && self.dli_fbase)
        os_log(logger(), "TegraGM20BMetal: %{public}s image base 0x%lx, main header 0x%lx", processWord(),
               (unsigned long)(uintptr_t)self.dli_fbase, (unsigned long)(uintptr_t)_dyld_get_image_header(0));
    static dispatch_source_t timer;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC,
                              5 * NSEC_PER_SEC);
    dispatch_source_set_event_handler(timer, ^{
        // 1024: the raw rates line passed 768 with its replay parts (rsplit= rdraw= tw=).
        char line[1024];
        // command buffers on each path and why, encode, draws, memory
        if (metal_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_fetch_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_split_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_encode_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_frame_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_fetch_keys(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
    });
    dispatch_resume(timer);
}
#endif

// One open of the process's device; nil with what it met in `failure` (device_open.h) and in `why`
id openOnce(unsigned port, gm20b::OpenFailure &failure, NSString *__strong &why)
{
    failure = gm20b::OpenFailure::Structural;
    const bool renderServer = strcmp(getprogname(), "backboardd") == 0;
    // ScreenshotServicesService: PencilKit in the screenshot thumbnail aborts the process without a device (TegraGM20B.cpp).
    if (!renderServer && strcmp(getprogname(), "iconservicesagent") != 0 &&
        strcmp(getprogname(), "ScreenshotServicesService") != 0 && !knobOn("TEGRA_GPU_ALL_CLIENTS", true)) {
        why = @"left out (TEGRA_GPU_ALL_CLIENTS=0)";
        return nil;
    }
    char pack[1100] = "";
    if (const char *refusal = gm20bPackRefusal(pack, sizeof pack)) {
        why = [NSString stringWithFormat:@"%s at %s", refusal, pack];
        return nil;
    }
    uint64_t registryID = 0;
    if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
        if (auto entryID = (int (*)(unsigned, uint64_t *))dlsym(iokit, "IORegistryEntryGetRegistryEntryID"))
            entryID(port, &registryID);
    }
    // The render server compresses its framebuffer and surfaces in an Apple GPU layout unless told not to; this GPU and the
    // display driver take plain rows. The kernel also writes these as backboardd's preferences (read before this code runs).
    if (renderServer) {
        setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
        CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse,
                                 kCFPreferencesCurrentApplication);
        CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse,
                                 kCFPreferencesCurrentApplication);
    }
#if SWITCH_DEBUG
    const uint64_t openBegan = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    if (renderServer)
        metal_prepare(pack, uint64_t(TEGRA_STACK_KEY));
#if SWITCH_DEBUG
    const uint64_t searched = renderServer ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
#endif
    id device = metal_device_open(pack, uint64_t(TEGRA_STACK_KEY), registryID, &why);
    if (!device) {
        failure = metal_open_failure();
        return nil;
    }
#if SWITCH_DEBUG
    const uint64_t opened = renderServer ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
    os_log(logger(), "TegraGM20BMetal: raw GM20B path on in %{public}s (pack %{public}s, stack %016llx)", getprogname(),
           pack, (unsigned long long)TEGRA_STACK_KEY);
#endif
    char nativeSeed[1100];
    metal_complete_states_lazy(&findRecordCacheDir,
                               gm20bNativeSeed(pack, nativeSeed, sizeof nativeSeed) ? nativeSeed : nullptr);
#if SWITCH_DEBUG
    const uint64_t openNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - openBegan;
    os_log(logger(), "TegraGM20BMetal: device %{public}s ready in %{public}s, open %llu ms",
           [[(id<MTLDevice>)device name] UTF8String], getprogname(), (unsigned long long)(openNs / 1000000));
    if (renderServer) {
        uint64_t prepOpenNs = 0, prepWarmNs = 0, waitedNs = 0;
        metal_prepare_split(&prepOpenNs, &prepWarmNs, &waitedNs);
        os_log(logger(),
               "SwitchMetal: open split in %{public}s (ms): cache directory %llu, device %llu (of it waiting for the "
               "prepare thread %llu, which took open %llu and warm %llu), states %llu",
               getprogname(), (searched - openBegan) / 1000000, (opened - searched) / 1000000, waitedNs / 1000000,
               prepOpenNs / 1000000, prepWarmNs / 1000000, (openBegan + openNs - opened) / 1000000);
    }
#endif
#if SWITCH_PLUGIN_FRAME
    observeLabelImages();
#endif
#if SWITCH_PLUGIN_RATES
    startRates();
#endif
    return device;
}

id openDevice(unsigned port)
{
    static id device;
    static std::mutex lock;
    std::lock_guard<std::mutex> hold(lock);
    if (device)
        return device;
    gm20b::OpenFailure failure = gm20b::OpenFailure::Structural;
    NSString *why = nil;
    device = openOnce(port, failure, why);
    if (!device)
        os_log_error(logger(), "TegraGM20BMetal: no device in %{public}s (%{public}s): %{public}@", getprogname(),
                     gm20b::openFailureText(failure), why);
    return device;
}
}  // namespace

@implementation TegraGM20BMetalDevice

- (id)initWithAcceleratorPort:(unsigned)port
{
    self = [super init];
    if (!self || !port)
        return nil;
    return openDevice(port);
}

@end
