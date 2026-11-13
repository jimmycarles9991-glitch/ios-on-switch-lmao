// SPDX-License-Identifier: MIT
// TegraGM20B Metal plugin: Metal on the Tegra X1 GPU (GM20B) for iPadOS 17.7.11, reached through the original discovery:
// IOServiceMatching("IOAcceleratorES") -> AGXAccelerator (drivers/TegraGM20B, published unless switch-no-gpu) -> MetalPluginName
// TegraGM20BMetal -> /System/Library/Extensions/TegraGM20BMetal.bundle -> MetalPluginClassName TegraGM20BMetalDevice ->
// -initWithAcceleratorPort:. That returns the native Metal frontend's device (drivers/NativeMetal/NativeBridgeMetalFrontend.mm:
// the metal2vulkan runtime) over Mesa NVK (libvulkan_nouveau.dylib beside this executable, nvkmd_darwin backend), which opens
// the accelerator's AGXDeviceUserClient (scratchpad/gpu/CONTRACT.md).
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
// keeps rendering on the CPU, as does one whose device does not open (the kernel refuses it, the GPU pool is full). Every process but the render server compiles the
// Metal source and stitched libraries Core Image makes (NMB_SYSTEM_COMPILER, the frontend's in-process MTLCompiler).
//
// Knobs, environment variables of the process (none is a boot-arg):
//   TEGRA_GPU_ALL_CLIENTS=0   only the processes named above get a device (default: every process)
//   TEGRA_M2V_TELEMETRY=0     no 5 s rates line (default 1)
//   TEGRA_M2V_CACHE_DIR       the shader cache directory instead of the search below; "memory" for none
//   NMB_MEMO_MB=<MiB>         the runtime's memo of compiled pipelines (default 32 outside backboardd, the runtime's 256 in it)
//   M2V_BLOCK_MIB=<MiB>       the largest pooled memory block (default 4 outside backboardd, 16 in it: each process keeps one
//                             in three pools, the first a quarter of it, at least 1 MiB, mapped whole and charged to it; each
//                             block is one BoAlloc on the thread that ran out of room, and goes back to the kernel once empty
//                             and the GPU idle, or on a memory warning: GpuMemoryRelease.h)
//   NMB_IDLE_RELEASE_MS=<ms>  idle GPU memory goes back to the kernel once the GPU idled this long (default 1000 outside
//                             backboardd, 0 in it: on memory warnings alone)
//   NMB_CACHE_MB=<MiB>        the shader cache directory's cap (default 256; the runtime's is 25 GiB)
//   NMB_ANSWER_CACHE=0        ask the runtime every device question again (default: keep each answer)
//   NMB_REFUSE_REBINDS=0      encoders log their calls for rebinds again (default: nothing rebinds)
//   NMB_PADDED_ALIAS=1        textures over padded rows rendered in place (default off: untried on NVK)
//   M2V_YCBCR_MEMO=<N>        skip unchanged two-plane YCbCr conversions (N sampled chunks; default off)
//   M2V_YCBCR_FILTER=device   the device's filter for converted YCbCr on any device (default: as every texture's sample,
//                             the device's own here)
//   M2V_ZERO_FRESH=1          zero fresh memory and upload staging on the CPU again (default off)
//   TEGRA_M2V_SEED=0          no shared shader cache seed (default 1)
//   NMB_SYSTEM_COMPILER       Metal source and stitched libraries compiled in-process (default 1 outside backboardd, 0 in it)
//   TEGRA_RAW_METAL=0         this process keeps the m2v device even with a raw pack staged
//
// The raw GM20B path (raw/runtime/RawMetal.h): when the build's pipeline pack is staged beside the bundle
// (<Extensions>/TegraGM20BMetal.raw/pack.bin, build-tegra-gm20b-metal.py makes it, stage-tegra-gm20b-metal.py copies it) and the
// process's sandbox lets it read it, the device every process gets is the raw facade: its command buffers go straight to
// MAXWELL_B methods, and any the raw path cannot draw exactly as m2v (a state not in the pack, an extended-range format,
// compute, mipmaps...) are replayed on m2v whole, its m2v device opened at the first such need (RawM2V). The render server's
// raw device is made beside the m2v device's open, every other process's memory, channel and shader heap at their first use
// (raw_gpu.h Device::Role). A pack of another build is
// refused (its stack key, TEGRA_RAW_STACK, differs). Off for a boot: remove or rename <Data>/ngpu/addons/TegraGM20BMetal.raw
// (stage-tegra-gm20b-metal.py --no-raw does); for a process: TEGRA_RAW_METAL=0 in its environment.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <notify.h>
#include <errno.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <pthread.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"
#include "RawMetal.h"
#include "SwitchRefusal.h"
#include "bridge/NativeMetalObjects.h"

// The build's stack key (build-tegra-gm20b-metal.py); 0 takes a pack of any build (never shipped so).
#ifndef TEGRA_RAW_STACK
#define TEGRA_RAW_STACK 0
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

// <bundle>/libvulkan_nouveau.dylib, beside this executable (<bundle>/TegraGM20BMetal).
bool libraryPath(char *path, size_t size)
{
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&libraryPath), &info) || !info.dli_fname)
        return false;
    snprintf(path, size, "%s", info.dli_fname);
    char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    snprintf(slash + 1, size - size_t(slash + 1 - path), "libvulkan_nouveau.dylib");
    return access(path, R_OK) == 0;
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

// r10-rawapps: why a process's shader cache is its memory alone, for the ready line: each candidate directory and what refused it
// ("sandbox": its profile, asked without a report; else the probe's errno). A memory-only process never journals, so the seed never
// gets its states and every launch compiles them again (r7o: com.apple.MapKit.SnapshotService, 14 states, ~2 s a launch).
char gCacheRefusals[640];

void noteCacheRefusal(const char *dir, const char *why)
{
    const size_t at = strlen(gCacheRefusals);
    snprintf(gCacheRefusals + at, sizeof gCacheRefusals - at, "%s%s: %s", at ? "; " : "", dir, why);
}

// <process system container>/Library/Caches/m2v. The container survives reboots and the process's sandbox writes it once
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
    uint64_t error = 0;
    char *container = lookup(identifier, &error);
    if (!container)
        return false;
    snprintf(out, size, "%s/Library/Caches/m2v", container);
    free(container);
    return true;
}

// The shader cache directories to try, most persistent first; each is this process's alone (m2v makes its files owner-only):
//  - the user cache directory when it is inside a data container (an app's own Library/Caches);
//  - the process's system container (above): backboardd's;
//  - the user cache directory elsewhere (a daemon with a home);
//  - the Library/Caches of the data container whose tmp is the temp directory, as m2v-<process> (WebKit's GPU process writes Safari's
//    container, a poster extension its own: iOS may clear a container's tmp, never its Library/Caches but under storage pressure, so
//    their pipelines no longer compile again after a reboot; r7n/r7o: the GPU process's and AegirPoster's caches were in tmp/m2v);
//  - /private/var/tmp/m2v-<process> (iconservicesagent, whose home is /var/empty), then the user temp directory, then $TMPDIR (WebKit's
//    GPU process: its per-user temp directory is the one its sandbox extension covers).
// One the sandbox would refuse is left out, asked without a report (sandboxMayCreate): the order of the others is unchanged.
unsigned cacheCandidates(char candidates[][512], unsigned most)
{
    unsigned count = 0;
    auto add = [&](const char *format, const char *a, const char *b) {
        if (count >= most)
            return;
        snprintf(candidates[count], 512, format, a, b);
        for (unsigned i = 0; i < count; ++i)
            if (!strcmp(candidates[i], candidates[count]))
                return;
        if (sandboxMayCreate(candidates[count]))
            ++count;
        else
            noteCacheRefusal(candidates[count], "sandbox");
    };
    char user[512] = {}, temp[512] = {}, container[512] = {};
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, user, sizeof user))
        user[0] = 0;
    const bool appContainer = strstr(user, "/Containers/Data/") != nullptr;
    if (user[0] && appContainer)
        add("%s%s", user, "m2v");
    if (!appContainer && systemContainerCache(container, sizeof container))
        add("%s%s", container, "");
    if (user[0] && !appContainer)
        add("%s%s", user, "m2v");
    const bool haveTemp = confstr(_CS_DARWIN_USER_TEMP_DIR, temp, sizeof temp) != 0;
    for (const char *dir : {haveTemp ? (const char *)temp : nullptr, (const char *)getenv("TMPDIR")}) {
        // <...>/Containers/Data/<kind>/<UUID>/tmp/... -> <...>/Containers/Data/<kind>/<UUID>/Library/Caches/m2v-<process>
        const char *data = dir ? strstr(dir, "/Containers/Data/") : nullptr;
        const char *tmp = data ? strstr(data, "/tmp/") : nullptr;
        if (!tmp && data && (tmp = strstr(data, "/tmp")) && tmp[4])
            tmp = nullptr;  // only a container's own tmp
        if (!tmp)
            continue;
        char root[512];
        snprintf(root, sizeof root, "%.*s/Library/Caches/m2v-", (int)(tmp - dir), dir);
        add("%s%s", root, getprogname());
    }
    add("%s%s", "/private/var/tmp/m2v-", getprogname());
    if (haveTemp)
        add("%s%s", temp, "m2v");
    if (const char *tmpdir = getenv("TMPDIR"); tmpdir && *tmpdir == '/')
        add(tmpdir[strlen(tmpdir) - 1] == '/' ? "%s%s" : "%s/%s", tmpdir, "m2v");
    return count;
}

// The shader cache seed shipped beside the bundle, <Extensions>/TegraGM20BMetal.m2v-seed/shared (build-tegra-gm20b-metal.py: the
// device journals' function specializations and render pipeline states made on the host with this build's m2v and the NVK twin,
// byte for byte what the device would write), named to the runtime as METAL2VULKAN_SHADER_CACHE_SEED: every process's runtime then
// reads a name its own cache directory misses from the seed, read only and in place (nothing is copied, written or scanned there),
// and writes what it compiles to its own directory as before. A seed of another build is never hit: its translations sit under
// another build's name and its driver records name another driver. The seed's SEED line (what the build made) into `name`; false
// when there is no seed this process can read, or the knob is off.
bool sharedSeed(char *name, size_t size)
{
    if (!knobOn("TEGRA_M2V_SEED", true))
        return false;
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&sharedSeed), &info) || !info.dli_fname)
        return false;
    char seed[1024], path[1100];
    strlcpy(seed, info.dli_fname, sizeof seed);
    for (int i = 0; i < 2; ++i)  // <Extensions>/TegraGM20BMetal.bundle/TegraGM20BMetal -> <Extensions>
        if (char *slash = strrchr(seed, '/'))
            *slash = 0;
    strlcat(seed, "/TegraGM20BMetal.m2v-seed/shared", sizeof seed);
    snprintf(path, sizeof path, "%s/SEED", seed);
    // a process whose sandbox may not read the seed never names it (asked without a report)
    if (!sandboxAllows("file-read-data", kSandboxFilterPath, path))
        return false;
    FILE *file = fopen(path, "r");
    if (!file)
        return false;
    const bool named = fgets(name, int(size), file) != nullptr;
    fclose(file);
    if (!named)
        return false;
    name[strcspn(name, "\n")] = 0;
    setenv("METAL2VULKAN_SHADER_CACHE_SEED", seed, 0);  // read by the runtime when the device opens
    return true;
}

// <Extensions>/TegraGM20BMetal.raw/pack.bin, beside the bundle (and the seed): the raw path's pipeline pack. Null if this process
// may read it, else why not (a process whose sandbox may not read it never tries: asked without a report).
const char *rawPackRefusal(char *path, size_t size)
{
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&rawPackRefusal), &info) || !info.dli_fname)
        return "the plugin's path is unknown";
    strlcpy(path, info.dli_fname, size);
    for (int i = 0; i < 2; ++i)  // <Extensions>/TegraGM20BMetal.bundle/TegraGM20BMetal -> <Extensions>
        if (char *slash = strrchr(path, '/'))
            *slash = 0;
    strlcat(path, "/TegraGM20BMetal.raw/pack.bin", size);
    if (!sandboxAllows("file-read-data", kSandboxFilterPath, path))
        return "its sandbox does not let it read the pack";
    return access(path, R_OK) == 0 ? nullptr : "no pack staged";
}

// The installed native records beside the pack at `pack` (<Extensions>/TegraGM20BMetal.raw/native, RecordCache::attachSeed) into
// `seed`; false if this process's sandbox may not read their index (asked without a report).
bool rawNativeSeed(const char *pack, char *seed, size_t size)
{
    const char *slash = strrchr(pack, '/');
    if (!slash)
        return false;
    snprintf(seed, size, "%.*s/native", int(slash - pack), pack);
    char index[1200];
    snprintf(index, sizeof index, "%s/raw-%016llx/index", seed, (unsigned long long)TEGRA_RAW_STACK);
    return sandboxAllows("file-read-data", kSandboxFilterPath, index);
}

bool gRaw;  // this process's device is the raw facade

#if SWITCH_DEBUG
// r17-widgets: the frame rates this process's display links are asked for. Each distinct request (a CAFrameRateRange or a
// preferredFramesPerSecond, with the image+offset that made it) is logged once, up to 8 per process, and passed on unchanged.
// With the commit stack line this tells where WidgetRenderer's steady 20 commits/s (the clock widget) come from: a rate SwiftUI
// or RenderBox asks the display link for (quantised to 60/3 on this 60 Hz panel) or a cadence of their own.
struct FrameRateRange {  // CAFrameRateRange's layout
    float minimum, maximum, preferred;
};

using SetRange = void (*)(id, SEL, FrameRateRange);
using SetFps = void (*)(id, SEL, NSInteger);
SetRange gSetRange;
SetFps gSetFps;

void callerName(const void *pc, char *out, size_t size)
{
    Dl_info info = {};
    if (dladdr(pc, &info) && info.dli_fname && info.dli_fbase) {
        const char *name = strrchr(info.dli_fname, '/');
        snprintf(out, size, "%s+0x%lx", name ? name + 1 : info.dli_fname,
                 (unsigned long)((uintptr_t)pc - (uintptr_t)info.dli_fbase));
    } else {
        snprintf(out, size, "0x%lx", (unsigned long)(uintptr_t)pc);
    }
}

bool firstRequest(uint64_t key)
{
    static std::mutex lock;
    static uint64_t seen[8];
    static unsigned count;
    std::lock_guard<std::mutex> guard(lock);
    for (unsigned i = 0; i < count; i++)
        if (seen[i] == key)
            return false;
    if (count == 8)
        return false;
    seen[count++] = key;
    return true;
}

void observedSetRange(id link, SEL cmd, FrameRateRange range)
{
    const void *pc = __builtin_return_address(0);
    uint32_t bits[3];
    memcpy(bits, &range, sizeof bits);
    if (firstRequest(((uint64_t)bits[0] * 31 + bits[1]) * 31 + bits[2] + ((uint64_t)(uintptr_t)pc << 20))) {
        char who[128];
        callerName(pc, who, sizeof who);
        os_log(logger(),
               "TegraGM20BMetal: %{public}s display link frame rate range min %.1f max %.1f preferred %.1f from "
               "%{public}s",
               getprogname(), (double)range.minimum, (double)range.maximum, (double)range.preferred, who);
    }
    gSetRange(link, cmd, range);
}

void observedSetFps(id link, SEL cmd, NSInteger fps)
{
    const void *pc = __builtin_return_address(0);
    if (firstRequest(((uint64_t)fps << 1 | 1) + ((uint64_t)(uintptr_t)pc << 20))) {
        char who[128];
        callerName(pc, who, sizeof who);
        os_log(logger(), "TegraGM20BMetal: %{public}s display link preferredFramesPerSecond %ld from %{public}s",
               getprogname(), (long)fps, who);
    }
    gSetFps(link, cmd, fps);
}

void observeDisplayLinkRates()
{
    static std::atomic<bool> done;  // swapped once per process: a second swap would make the observer its own original
    if (done.exchange(true))
        return;
    if (!strcmp(getprogname(), "backboardd"))
        return;  // the render server's own display links stay untouched
    Class cls = objc_getClass("CADisplayLink");
    if (!cls)
        return;  // no QuartzCore in this process
    if (Method m = class_getInstanceMethod(cls, sel_registerName("setPreferredFrameRateRange:"))) {
        // The original first: a call may land as soon as the method is swapped.
        gSetRange = (SetRange)method_getImplementation(m);
        method_setImplementation(m, (IMP)observedSetRange);
    }
    if (Method m = class_getInstanceMethod(cls, sel_registerName("setPreferredFramesPerSecond:"))) {
        gSetFps = (SetFps)method_getImplementation(m);
        method_setImplementation(m, (IMP)observedSetFps);
    }
}

// r31-rawds: what makes WidgetRenderer redraw (a steady 20 commits/s on the Home Screen, each a Core Animation transaction
// displaying a RenderBox layer: CA::Layer::layout_and_display_if_needed in the commit stack, with no display link rate asked
// for). In WidgetRenderer_* processes, each distinct caller (image+offset) of -[CALayer setNeedsDisplay] with the layer's class,
// and each display link made with the target's class and selector, once, up to 8 lines in all.
using SetNeedsDisplay = void (*)(id, SEL);
using MakeDisplayLink = id (*)(id, SEL, id, SEL);
SetNeedsDisplay gSetNeedsDisplay;
MakeDisplayLink gMakeDisplayLink;

bool firstRedrawCause(uint64_t key)
{
    static std::mutex lock;
    static uint64_t seen[8];
    static unsigned count;
    std::lock_guard<std::mutex> guard(lock);
    for (unsigned i = 0; i < count; i++)
        if (seen[i] == key)
            return false;
    if (count == 8)
        return false;
    seen[count++] = key;
    return true;
}

void observedSetNeedsDisplay(id layer, SEL cmd)
{
    const void *pc = __builtin_return_address(0);
    Class cls = object_getClass(layer);
    if (firstRedrawCause((uint64_t)(uintptr_t)pc ^ ((uint64_t)(uintptr_t)cls << 1))) {
        char who[128];
        callerName(pc, who, sizeof who);
        os_log(logger(), "TegraGM20BMetal: %{public}s setNeedsDisplay of a %{public}s from %{public}s", getprogname(),
               class_getName(cls), who);
    }
    gSetNeedsDisplay(layer, cmd);
}

id observedMakeDisplayLink(id cls, SEL cmd, id target, SEL selector)
{
    const void *pc = __builtin_return_address(0);
    if (firstRedrawCause((uint64_t)(uintptr_t)selector ^ ((uint64_t)(uintptr_t)object_getClass(target) << 1) ^ 1)) {
        char who[128];
        callerName(pc, who, sizeof who);
        os_log(logger(), "TegraGM20BMetal: %{public}s display link to -[%{public}s %{public}s] from %{public}s",
               getprogname(), class_getName(object_getClass(target)), sel_getName(selector), who);
    }
    return gMakeDisplayLink(cls, cmd, target, selector);
}

void observeRedrawCauses()
{
    static std::atomic<bool> done;
    if (strncmp(getprogname(), "WidgetRenderer", 14) || done.exchange(true))
        return;
    if (Method m = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("setNeedsDisplay"))) {
        gSetNeedsDisplay = (SetNeedsDisplay)method_getImplementation(m);
        method_setImplementation(m, (IMP)observedSetNeedsDisplay);
    }
    if (Method m =
            class_getClassMethod(objc_getClass("CADisplayLink"), sel_registerName("displayLinkWithTarget:selector:"))) {
        gMakeDisplayLink = (MakeDisplayLink)method_getImplementation(m);
        method_setImplementation(m, (IMP)observedMakeDisplayLink);
    }
}

// r31-rawds: how this GPU rounds a float fragment into an RGBA16Float target (the frontend's RGB8_444_2P stand-in, whose store
// takes Metal's round-to-nearest-even half of each channel). On com.brunostjohn.switch.debug.rf16-rounding a WidgetRenderer_*
// process draws four floats into a 4x1 RGBA16Float target through its device and logs the halves it holds ("rf16" category):
// just above, on and just below the midpoint of 0x37ff and 0x3800, and on the midpoint of 0x3800 and 0x3801. Nearest even:
// 3800 3800 37ff 3800; toward zero: 37ff 37ff 37ff 3800; ties away: 3800 3800 37ff 3801.
void checkRf16Rounding()
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "rf16");
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                       @"struct V { float4 p [[position]]; };\n"
                       @"vertex V cover(uint i [[vertex_id]]) { float2 c = float2((i << 1) & 2, i & 2); V v;"
                       @" v.p = float4(c * 2 - 1, 0, 1); return v; }\n"
                       @"fragment float4 values(V in [[stage_in]], constant float4 *k [[buffer(0)]]) {"
                       @" return k[uint(in.p.x)]; }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:@"cover"];
    d.fragmentFunction = [library newFunctionWithName:@"values"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    id<MTLRenderPipelineState> state = library ? [device newRenderPipelineStateWithDescriptor:d error:&error] : nil;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                  width:4
                                                                                 height:1
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLBuffer> out = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
    if (!state || !target || !out) {
        os_log_error(log, "TegraGM20BMetal: %{public}s rf16 rounding: no state or target (%{public}@)", getprogname(),
                     error.localizedDescription);
        return;
    }
    const uint32_t bits[4] = {0x3efff001u, 0x3efff000u, 0x3effefffu, 0x3f001000u};
    float values[16] = {};
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++)
            memcpy(&values[4 * i + c], &bits[i], 4);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
    [render setRenderPipelineState:state];
    [render setFragmentBytes:values length:sizeof values atIndex:0];
    [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [render endEncoding];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(4, 1, 1)
                        toBuffer:out
               destinationOffset:0
          destinationBytesPerRow:32
        destinationBytesPerImage:32];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint16_t *h = static_cast<const uint16_t *>(out.contents);
    os_log(log, "TegraGM20BMetal: %{public}s rf16 rounding: %04x %04x %04x %04x (nearest even 3800 3800 37ff 3800)",
           getprogname(), h[0], h[4], h[8], h[12]);
}

void observeRf16Request()
{
    static std::atomic<bool> done;
    if (strncmp(getprogname(), "WidgetRenderer", 14) || done.exchange(true))
        return;
    int token;
    notify_register_dispatch("com.brunostjohn.switch.debug.rf16-rounding", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 checkRf16Rounding();
                             });
}

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

// The frontend's rates line, every 5 s while the device works (it says nothing when idle). r16-idle: with a whole
// interval of leeway, so the timer rides a wakeup the process or the kernel takes anyway instead of waking every
// Metal process every 5 s; both rates functions measure their own window, so a late tick only makes a longer one.
void startRates()
{
    if (!knobOn("TEGRA_M2V_TELEMETRY", true))
        return;
    observeDisplayLinkRates();
    observeRedrawCauses();
    observeRf16Request();
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
        if (native_bridge_metal_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        // r17-widgets: the stack of one commit per 30 s (what drives this process's commit rate).
        if (native_bridge_metal_commit_origin(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        // The raw path's own: command buffers on each path and why, encode, draws, memory.
        if (gRaw && raw_metal_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (gRaw && raw_metal_fetch_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (gRaw && raw_metal_encode_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (gRaw && raw_metal_fetch_keys(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
    });
    dispatch_resume(timer);
}
#endif

// A shared event for another process (a handle to send, or one received): no kernel-backed event exists for one yet, so
// the log says who asks before anything is built for it.
static void crossProcessEvent(const char *cls, const char *selector)
{
    if (!strncmp(selector, "newSharedEventHandle", strlen("newSharedEventHandle")) ||
        !strcmp(selector, "newSharedEventWithHandle:"))
        switch_refusal("frontend", "sharedevent.handle", "-[%s %s]: no cross-process shared events", cls, selector);
}

// A Metal selector the frontend does not implement (the frontend's own report, made before it raises).
void missing(const char *cls, const char *selector)
{
    crossProcessEvent(cls, selector);
    os_log_error(logger(), "TegraGM20BMetal: unimplemented -[%{public}s %{public}s]", cls, selector);
}

// The frontend raises on a Metal selector it does not implement (NBObject / device -doesNotRecognizeSelector:), and an
// exception in the render server restarts it into the same exception: no UI at all. Instead, a selector that some MTL*
// protocol declares is answered zero/nil/NO (native_bridge_metal_answer_unimplemented), with this line once per class and
// selector, as an absent optional feature would be; respondsToSelector: still says NO, so optional paths take their
// unsupported branch. A selector no Metal protocol declares raises as before. The protocol walk that finds a selector's types
// is the frontend's, once per selector (r14-maps: here it ran per call, Maps' setDepthClipMode: lag).
void answeredZero(const char *cls, const char *selector)
{
    crossProcessEvent(cls, selector);
    os_log_error(logger(), "TegraGM20BMetal: unimplemented -[%{public}s %{public}s] answered zero", cls, selector);
}

// The m2v device (the frontend over the bridge: NVK's device and m2v's runtime), opened once by its first user: the raw
// device's first need, the render server's open, or a process the raw path is off in. The runtime keeps its shader cache
// across reboots where the directory lasts: the first candidate (cacheCandidates) that takes a write, or
// TEGRA_M2V_CACHE_DIR, which the raw path's records share. A device that does not open for another reason than its cache is
// not tried again with the next one. With none writable the cache is the runtime's memory alone (a bridge without that
// mode refuses, and the process gets no device as before).
class M2VOpening {
public:
    M2VOpening(const char *loader, uint64_t registryID) : registryID_(registryID)
    {
        strlcpy(loader_, loader, sizeof loader_);
        const char *chosen = getenv("TEGRA_M2V_CACHE_DIR");
        if (chosen && strcmp(chosen, "memory") && sandboxMayCreate(chosen))
            strlcpy(candidates_[count_++], chosen, sizeof candidates_[0]);
        else if (!chosen)
            count_ = cacheCandidates(candidates_, 6);
        for (; first_ < count_ && !writable(candidates_[first_]); first_++)
            noteCacheRefusal(candidates_[first_], strerror(errno));
        seeded_ = sharedSeed(seedName_, sizeof seedName_);
    }

    const char *cacheDir() const
    {
        return first_ < count_ ? candidates_[first_] : nullptr;
    }

    // Opens it now on a thread of its own: device() then waits for that open
    void start()
    {
        std::thread([this] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            pthread_setname_np("com.brunostjohn.switch.m2v.open");
            device();
        }).detach();
    }

    id device()
    {
        std::call_once(once_, [this] {
            opened_ = open();
        });
        return opened_;
    }

private:
    id open()
    {
        const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        id made = nil;
        const char *cache = nullptr;
        for (unsigned i = first_; i < count_ && !made; ++i) {
            if (i > first_ && !writable(candidates_[i])) {
                noteCacheRefusal(candidates_[i], strerror(errno));
                continue;
            }
            cache = candidates_[i];
#if SWITCH_DEBUG
            // The runtime's making log (m2v 0118): one line per driver record this process writes (a pipeline compiled here,
            // which neither its cache nor the seed held), naming the record and the making's description, beside the journal
            // so harvest.py --save takes it; past 2 MiB it starts again, the previous kept as makings.log.old.
            char makings[600];
            snprintf(makings, sizeof makings, "%s/makings.log", candidates_[i]);
            struct stat makingsStat;
            if (stat(makings, &makingsStat) == 0 && makingsStat.st_size > (2 << 20)) {
                char old[620];
                snprintf(old, sizeof old, "%s.old", makings);
                rename(makings, old);
            }
            setenv("METAL2VULKAN_MAKING_LOG", makings, 1);
#endif
            made = bridgeOpen(cache);
            if (!made && !strstr(nmb_last_error(), "ShaderCache"))
                break;
        }
        if (!made && (!cache || strstr(nmb_last_error(), "ShaderCache"))) {
            cache = nullptr;
            made = bridgeOpen(nullptr);
        }
        if (!made) {
            os_log_error(logger(), "TegraGM20BMetal: no m2v device in %{public}s: %{public}s", getprogname(),
                         nmb_last_error());
            return nil;
        }
        // Every frontend object (NBObject), then the device.
        native_bridge_metal_answer_unimplemented(NSClassFromString(@"NBObject"), answeredZero);
        native_bridge_metal_answer_unimplemented(object_getClass(made), answeredZero);
        // seed: "shared (<its SEED line>)" (the runtime reads the shared seed beneath the cache), "none" (no readable seed,
        // or off)
        os_log(logger(),
               "TegraGM20BMetal: m2v device %{public}s open in %{public}s, cache "
               "%{public}s%{public}s%{public}s%{public}s, "
               "seed %{public}s%{public}s%{public}s, open %llu ms (bridge %llu ms)",
               [[(id<MTLDevice>)made name] UTF8String], getprogname(), cache ? cache : "memory only",
               !cache ? " (" : "", !cache ? (gCacheRefusals[0] ? gCacheRefusals : "no candidate") : "",
               !cache ? ")" : "", seeded_ ? "shared (" : "none", seeded_ ? seedName_ : "", seeded_ ? ")" : "",
               (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1000000),
               (unsigned long long)(bridgeNs_ / 1000000));
        return made;
    }

    id bridgeOpen(const char *cache)
    {
        const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        id made = nil;
        @try {
            made = native_bridge_metal_open(loader_, cache, registryID_, missing);
        } @catch (NSException *exception) {
            made = nil;
        }
        bridgeNs_ += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began;
        return made;
    }

    char loader_[1024] = {};
    uint64_t registryID_ = 0;
    char candidates_[6][512] = {};
    unsigned count_ = 0, first_ = 0;
    char seedName_[128] = {};
    bool seeded_ = false;
    uint64_t bridgeNs_ = 0;
    std::once_flag once_;
    id opened_ = nil;
};

id openDevice(unsigned port)
{
    static id device;
    static bool tried;
    @synchronized([TegraGM20BMetalDevice class]) {
        if (tried)
            return device;
        tried = true;
        const bool renderServer = strcmp(getprogname(), "backboardd") == 0;
        // ScreenshotServicesService: PencilKit in the screenshot thumbnail aborts the process without a device (TegraGM20B.cpp).
        if (!renderServer && strcmp(getprogname(), "iconservicesagent") != 0 &&
            strcmp(getprogname(), "ScreenshotServicesService") != 0 && !knobOn("TEGRA_GPU_ALL_CLIENTS", true))
            return nil;
        // NVK opens the first service of this class (nvkmd_darwin.c darwin_open): ours, never the IOAcceleratorES stub
        // (drivers/SwitchAccelStub) that only ends QuartzCore's accelerator poll. Set before the raw path's setup below, which
        // may be the first to open NVK's connection.
        setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
        const bool rawWanted = knobOn("TEGRA_RAW_METAL", true);
        char pack[1100] = "";
        const char *packRefusal = rawWanted ? rawPackRefusal(pack, sizeof pack) : nullptr;
        if (!renderServer)
            setenv("NMB_MEMO_MB", "32", 0);  // read by the bridge
        // Read by the runtime. In the render server too: its pools double their blocks, and each block is one BoAlloc on the
        // thread whose resource creation found no room (Core Animation's render thread). At the kernel's ~1.5 ms per MiB
        // for cached memory a 64 MiB block stalled it ~100 ms (r5c: 66 MB of BoAllocs, 103 ms, in the 5 s Settings opened);
        // 16 MiB blocks bound that to a quarter and still pool any 720p texture, half a block being the dedicated limit.
        setenv("M2V_BLOCK_MIB", renderServer ? "16" : "4", 0);
        if (renderServer)
            setenv("NMB_IDLE_RELEASE_MS", "0", 0);  // read by the frontend (GpuMemoryRelease.h)
        if (!renderServer)
            setenv("NMB_SYSTEM_COMPILER", "1", 0);  // read by the frontend: Core Image's kernels are compiled here
        if (const char *zero = getenv("M2V_ZERO_FRESH"); zero && !strcmp(zero, "0"))
            unsetenv("M2V_ZERO_FRESH");  // the runtime reads any value as on
        char loader[1024];
        if (!libraryPath(loader, sizeof loader)) {
            os_log_error(logger(), "TegraGM20BMetal: no device: libvulkan_nouveau.dylib missing beside the plugin");
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
        const uint64_t openBegan = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#if !SWITCH_DEBUG
        setenv("NMB_SHADOW_LOG", "0",
               0);  // the bridge's shadow.log and its every-texture announcements: nothing reads them
#endif
        // NVK holds the process's one GPU connection, which the raw path shares: loaded here, once, with the
        // environment set, before the raw path's setup looks for it on its own thread, so that lookup never races
        // the m2v device's load of the same image.
        if (!dlopen(loader, RTLD_NOW | RTLD_LOCAL)) {
            os_log_error(logger(), "TegraGM20BMetal: no device in %{public}s: %{public}s", getprogname(), dlerror());
            return nil;
        }
        // The render server's raw GM20B path's own setup (its pack, channel and context, the first pool chunk) starts now, beside
        // the m2v device's open below; raw_metal_device_open_lazy takes what it made, so its first frame waits for neither twice.
        if (renderServer && rawWanted && !packRefusal)
            raw_metal_prepare(pack, uint64_t(TEGRA_RAW_STACK));
        M2VOpening &m2v = *new M2VOpening(loader, registryID);
        // Every process draws through the raw GM20B path when the build's pack is staged and readable (above). Its m2v device is
        // opened at the first thing only m2v does, so a process that draws only packed states never has one; the render server's
        // opens beside the raw path's setup, as before, so no first need of its waits for it on Core Animation's render thread.
        // One line either way, so a boot shows which device each process got.
        if (!rawWanted) {
            os_log(logger(), "TegraGM20BMetal: raw GM20B path off in %{public}s (TEGRA_RAW_METAL=0): m2v device kept",
                   getprogname());
        } else if (packRefusal) {
            os_log(logger(),
                   "TegraGM20BMetal: raw GM20B path off in %{public}s (%{public}s at %{public}s): m2v device kept",
                   getprogname(), packRefusal, pack);
        } else {
            if (renderServer)
                m2v.start();
            RawM2V lazy;
            lazy.open = ^{
                return m2v.device();
            };
            lazy.methods = NSClassFromString(@"NBDeviceMethods");
            lazy.answerSelector = nmb_device_answer_selector;
            NSString *why = nil;
            if (id raw = raw_metal_device_open_lazy(pack, uint64_t(TEGRA_RAW_STACK), registryID, lazy, &why)) {
                os_log(logger(), "TegraGM20BMetal: raw GM20B path on in %{public}s (pack %{public}s, stack %016llx)",
                       getprogname(), pack, (unsigned long long)TEGRA_RAW_STACK);
                device = raw;
                gRaw = true;
                char nativeSeed[1100];
                raw_metal_complete_states(m2v.cacheDir(),
                                          rawNativeSeed(pack, nativeSeed, sizeof nativeSeed) ? nativeSeed : nullptr);
            } else {
                os_log_error(logger(),
                             "TegraGM20BMetal: raw GM20B path off in %{public}s (%{public}@): m2v device kept",
                             getprogname(), why);
            }
        }
        if (!device)
            device = m2v.device();
        if (!device)
            return nil;
        const uint64_t openNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - openBegan;
        os_log(logger(), "TegraGM20BMetal: device %{public}s ready in %{public}s, open %llu ms%{public}s",
               [[(id<MTLDevice>)device name] UTF8String], getprogname(), (unsigned long long)(openNs / 1000000),
               gRaw ? " (m2v device at its first need)" : "");
#if SWITCH_DEBUG
        startRates();
#endif
        return device;
    }
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
