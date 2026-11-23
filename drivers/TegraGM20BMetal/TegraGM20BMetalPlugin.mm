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
#include <notify.h>
#include <errno.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
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
    };
    char user[512] = {}, temp[512] = {}, container[512] = {};
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, user, sizeof user))
        user[0] = 0;
    const bool appContainer = strstr(user, "/Containers/Data/") != nullptr;
    if (user[0] && appContainer)
        add("%s%s", user, "TegraGM20BMetal");
    if (!appContainer && systemContainerCache(container, sizeof container))
        add("%s%s", container, "");
    if (user[0] && !appContainer)
        add("%s%s", user, "TegraGM20BMetal");
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
        add("%s%s", root, getprogname());
    }
    add("%s%s", "/private/var/tmp/TegraGM20BMetal-", getprogname());
    if (haveTemp)
        add("%s%s", temp, "TegraGM20BMetal");
    if (const char *tmpdir = getenv("TMPDIR"); tmpdir && *tmpdir == '/')
        add(tmpdir[strlen(tmpdir) - 1] == '/' ? "%s%s" : "%s/%s", tmpdir, "TegraGM20BMetal");
    return count;
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

// How this GPU rounds a float fragment into an RGBA16Float target, which Apple's GPU stores rounded toward zero at
// textureWriteRoundingMode 0 and 1, the default. On com.brunostjohn.switch.debug.rf16-rounding a WidgetRenderer_*
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
    os_log(log, "TegraGM20BMetal: %{public}s rf16 rounding: %04x %04x %04x %04x (toward zero 37ff 37ff 37ff 3800)",
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

// The driver's rates lines, every 5 s while the device works (they say nothing when idle). r16-idle: with a whole
// interval of leeway, so the timer rides a wakeup the process or the kernel takes anyway instead of waking every
// Metal process every 5 s; each rates function measures its own window, so a late tick only makes a longer one.
void startRates()
{
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
        // command buffers on each path and why, encode, draws, memory
        if (metal_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_fetch_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_split_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_encode_rates(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
        if (metal_fetch_keys(line, sizeof line))
            os_log(logger(), "TegraGM20BMetal: %{public}s %{public}s", processWord(), line);
    });
    dispatch_resume(timer);
}
#endif

// The first record cache directory this process can write (cacheCandidates), for the driver's records; null: none, and
// they stay in its memory
const char *recordCacheDir()
{
    static char candidates[6][512];
    const unsigned count = cacheCandidates(candidates, 6);
    for (unsigned i = 0; i < count; i++)
        if (writable(candidates[i]))
            return candidates[i];
    return nullptr;
}

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
    const uint64_t openBegan = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    // The render server's raw GM20B path's own setup (its pack, channel and context, the first pool chunk) starts now on a
    // thread of its own, beside the cache directory search below; metal_device_open takes what it made.
    if (renderServer)
        metal_prepare(pack, uint64_t(TEGRA_STACK_KEY));
    const char *records = recordCacheDir();
    id device = metal_device_open(pack, uint64_t(TEGRA_STACK_KEY), registryID, &why);
    if (!device) {
        failure = metal_open_failure();
        return nil;
    }
    os_log(logger(), "TegraGM20BMetal: raw GM20B path on in %{public}s (pack %{public}s, stack %016llx)", getprogname(),
           pack, (unsigned long long)TEGRA_STACK_KEY);
    char nativeSeed[1100];
    metal_complete_states(records, gm20bNativeSeed(pack, nativeSeed, sizeof nativeSeed) ? nativeSeed : nullptr);
    const uint64_t openNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - openBegan;
    os_log(logger(), "TegraGM20BMetal: device %{public}s ready in %{public}s, open %llu ms",
           [[(id<MTLDevice>)device name] UTF8String], getprogname(), (unsigned long long)(openNs / 1000000));
#if SWITCH_DEBUG
    startRates();
#endif
    return device;
}

id openDevice(unsigned port)
{
    static id device;
    @synchronized([TegraGM20BMetalDevice class]) {
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
