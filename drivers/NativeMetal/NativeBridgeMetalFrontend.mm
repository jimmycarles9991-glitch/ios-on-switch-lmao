// SPDX-License-Identifier: MIT
// Native Metal API frontend over the bridge (bridge/NativeMetalObjects.h): the metal2vulkan runtime on Turnip, on the Adreno 740 through
// NativeAdreno. Same shape as the offline frontend: the device is a dynamically allocated subclass of the target's own _MTLDevice that
// only adds this file's implementations; every other object is an own class answering the Metal protocol methods it implements.
// Every result comes from the runtime and the GPU: a refused call returns nil/NO with the runtime's reason, an encoder call the runtime
// refuses fails its command buffer (status Error, handlers run) instead of being dropped. A selector this frontend does not implement is
// reported through the missing-selector hook before the usual exception, so a hardware run names the gap.
// Shared events work within this process (no cross-process handle yet). Not yet implemented here: indirect command buffers, visible function
// tables, function stitching. Drawables are Core Animation's (CAMetalLayer, IOSurface textures); command buffers present them.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/runtime.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <os/log.h>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>
#include "bridge/NativeMetalCompile.h"
#include "bridge/NativeMetalDirect.h"
#include "bridge/NativeMetalObjects.h"
#include "NativeBridgeMetalFrontend.h"
#include "NativeComputeReflection.h"
#include "NativeComputeArtifactStorage.h"
#include "SwitchCompileReply.h"
#include "AsyncPipelines.h"
#include "GpuMemoryRelease.h"
#include "ImageFilterLink.h"
#include "LimitsTable.h"
#include "MetalABI.h"
#include "MetallibEntries.h"
#include "MetallibSlice.h"
#include "QueuePriority.h"
#include "SurfaceLayout.h"
#include "SwitchFrameTimeline.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchRefusal/SwitchRefusalSite.h"
// The two-plane render targets' load and store functions (TwoPlaneStandIn.metal).
#include "TwoPlaneStandIn.h"

#if SWITCH_FRAME_TIMELINE
#define SWITCH_METAL_DIAGNOSTIC(...) __VA_ARGS__
#else
#define SWITCH_METAL_DIAGNOSTIC(...)
#endif

static NmbDevice *gDevice;
static NmbCompileContext *gCompile;
static NativeComputeArtifactStorage gNativeArtifactStorage;
static std::atomic<uint32_t> gNativeArtifactWrites{0};
// Never destroyed: its dispatch sources' handlers may still run while the process exits.
static GpuMemoryRelease &gMemoryRelease = *new GpuMemoryRelease;
static id gDeviceObject;
static uint64_t gRegistryID;
static NSString *gName;
static void (*gMissing)(const char *, const char *);

// What this device has been asked and refused, for the plugin's status line (native_bridge_metal_status): commits, render/compute pipelines made,
// refusals, and the last two refusals' texts. Counters only; nothing here changes a result.
static std::atomic<uint64_t> gCommits, gCommitFailures, gPipelines, gProblems, gCompleted;
// What the render server actually asks of the GPU (int-141: 59 commits in 200 s, and a desktop that behaves like CPU compositing): render passes,
// draws, the pixels of their first colour attachment, compute dispatches, blits and the largest pass (pixels).
static std::atomic<uint64_t> gPasses, gDraws, gDispatches, gBlits, gBiggestPass, gFullScreenPasses;

// Where the time goes on the CPU side of the bridge: pipeline, library and function creation (count, total and slowest, in microseconds), and the
// last commits with what each held and how long the GPU took (commit to completion).
static uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

struct Timed {
    std::atomic<uint64_t> count{0}, total{0}, slowest{0};

    void add(uint64_t began)
    {
        const uint64_t took = nowUs() - began;
        count++;
        total += took;
        uint64_t seen = slowest.load();
        while (took > seen && !slowest.compare_exchange_weak(seen, took)) {
        }
    }
};

static Timed gTimePipeline, gTimeLibrary, gTimeFunction, gTimeEncodeToCommit;
static Timed gTimeWaitScheduled, gTimeWaitCompleted;
static Timed gTimeResourceWait;

// Per-command-buffer time distributions for the rates line (the kernel's display stats line has the presents): fixed bins
// (1 ms to 64 ms, 4 ms to 128, 8 ms to 256, 64 ms to 1024, one beyond) of relaxed atomics, added from any thread, read and cleared
// by native_bridge_metal_rates. A quantile is its bin's upper edge, capped at the window's maximum.
struct NBHist {
    static constexpr unsigned kBins = 64 + 16 + 16 + 12 + 1;
    std::atomic<uint32_t> bins[kBins];
    std::atomic<uint64_t> top{0};

    static unsigned binOf(uint64_t us)
    {
        const uint64_t ms = us / 1000;
        if (ms < 64)
            return (unsigned)ms;
        if (ms < 128)
            return 64 + (unsigned)((ms - 64) / 4);
        if (ms < 256)
            return 80 + (unsigned)((ms - 128) / 8);
        if (ms < 1024)
            return 96 + (unsigned)((ms - 256) / 64);
        return kBins - 1;
    }

    static uint64_t upperUs(unsigned bin)
    {
        if (bin < 64)
            return (bin + 1) * 1000ull;
        if (bin < 80)
            return (64 + (bin - 63) * 4) * 1000ull;
        if (bin < 96)
            return (128 + (bin - 79) * 8) * 1000ull;
        if (bin < 108)
            return (256 + (bin - 95) * 64) * 1000ull;
        return UINT64_MAX;
    }

    void add(uint64_t us)
    {
        bins[binOf(us)].fetch_add(1, std::memory_order_relaxed);
        uint64_t seen = top.load(std::memory_order_relaxed);
        while (us > seen && !top.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
        }
    }

    // The window's counts, cleared: how many, p50, p99 and the maximum (microseconds), and how many reached the bin of overUs, and
    // of over2Us, or beyond.
    struct Taken {
        uint64_t n, p50, p99, max, over, over2;
    };

    Taken take(uint64_t overUs, uint64_t over2Us)
    {
        uint32_t counts[kBins];
        uint64_t n = 0;
        for (unsigned bin = 0; bin < kBins; bin++) {
            counts[bin] = bins[bin].exchange(0, std::memory_order_relaxed);
            n += counts[bin];
        }
        const uint64_t most = top.exchange(0, std::memory_order_relaxed);
        auto quantile = [&](unsigned permille) -> uint64_t {
            if (!n)
                return 0;
            const uint64_t rank = (n * permille + 999) / 1000;
            uint64_t seen = 0;
            for (unsigned bin = 0; bin < kBins; bin++) {
                seen += counts[bin];
                if (seen >= rank) {
                    const uint64_t up = upperUs(bin);
                    return up < most ? up : most;
                }
            }
            return most;
        };
        uint64_t over = 0, over2 = 0;
        for (unsigned bin = binOf(overUs); bin < kBins; bin++)
            over += counts[bin];
        for (unsigned bin = binOf(over2Us); bin < kBins; bin++)
            over2 += counts[bin];
        return {n, quantile(500), quantile(990), most, over, over2};
    }
};

// enc: creation to commit (the encoding thread's wall time for one command buffer); gap: one committing thread's commit to its
// next (CA's render server commits its frames from one thread; gaps of 250 ms or more are idle and not counted); sched: each
// waitUntilScheduled (CA waits for it before it swaps); gpu: commit to completion; pso: each render pipeline state made.
static NBHist gHistEncode, gHistGap, gHistSched, gHistGpu, gHistPso;
#if SWITCH_FRAME_TIMELINE
static std::atomic<uint64_t> gPassPixels;
static thread_local uint64_t tLastCommitUs;
#endif
static std::atomic<uint64_t> gResourceChecks, gResourceSkipped;
// The rest of the 5 s rates line (native_bridge_metal_rates): the runtime's commit call (submission, and any CPU wait inside it), each
// command buffer's encoding (creation to commit, wall time), the time its encoding thread spent inside this frontend's encoding calls
// (apiTime: what of the encoding is ours, the rest being the caller's own work between calls), and the render encoders that open on
// exactly the attachments the command buffer's previous encoder rendered, loading what it stored (passes one pass could hold).
static Timed gTimeSubmit;
static std::atomic<uint64_t> gEncodeUs, gApiUs, gJoinablePasses;
// Mach ticks spent in the encoding calls on this thread since its last commit (one thread encodes a command buffer, Metal's rule; the
// committing thread hands its sum to the rates). Off with TEGRA_M2V_TELEMETRY=0.
#if SWITCH_FRAME_TIMELINE
static thread_local uint64_t tApiTicks;

static bool apiTiming()
{
    static const bool on = [] {
        const char *v = getenv("TEGRA_M2V_TELEMETRY");
        return !v || strcmp(v, "0");
    }();
    return on;
}
#endif

struct NBApiTime {
#if SWITCH_FRAME_TIMELINE
    const uint64_t began = apiTiming() ? mach_absolute_time() : 0;

    ~NBApiTime()
    {
        if (began)
            tApiTicks += mach_absolute_time() - began;
    }
#else
    ~NBApiTime()
    {}
#endif
};

#if SWITCH_FRAME_TIMELINE
static uint64_t ticksToUs(uint64_t ticks)
{
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b{};
        mach_timebase_info(&b);
        return b;
    }();
    return base.denom ? ticks * base.numer / base.denom / 1000 : 0;
}
#endif

// replaceRegion/getBytes calls (the runtime makes some of them an internal submission it waits for: a CPU wait for the GPU).
static Timed gTimeTexelCopy;
// Planar IOSurfaces a texture was asked over (refused), and the device answers asked of the runtime and kept (askAnswer).
static std::atomic<uint64_t> gSurfaceMultiPlane;
// Host imports the GPU wrote, invalidated for this process's CPU readback (invalidateForCpu): count, bytes, microseconds.
static std::atomic<uint64_t> gReadbackInvalidates, gReadbackBytes, gReadbackUs;
// Surface imports (surfaceBuffer): new ones, timed (the runtime's host import: the kernel's BoImport and VmBind, the memory and
// its block), and lookups an import still alive answered; textures made over a surface import, timed (the runtime's image).
static Timed gTimeSurfaceImport, gTimeSurfaceTexture;
static std::atomic<uint64_t> gSurfaceImportHits;
static std::atomic<uint64_t> gAnswersAsked, gAnswersKept;

struct EncodedWork {
    uint32_t passes, draws, dispatches, blits;
    uint64_t pixels;
};

struct CommitRecord {
    uint64_t committedUs = 0, callbackUs = 0, ageUs = 0;
    int64_t scheduledUs = -1;
    uint32_t index = 0;
    EncodedWork work = {};
};

static CommitRecord gCommitRing[32];
static std::mutex gCommitLock;
static std::mutex gProblemLock;
static char gLastProblems[2][200];

static void noteProblem(NSString *what)
{
    std::lock_guard<std::mutex> lock(gProblemLock);
    memcpy(gLastProblems[1], gLastProblems[0], sizeof gLastProblems[0]);
    strlcpy(gLastProblems[0], what.UTF8String ?: "?", sizeof gLastProblems[0]);
}

#define NBLOG(...)                                                  \
    do {                                                            \
        NSString *nbText = [NSString stringWithFormat:__VA_ARGS__]; \
        NSLog(@"%@", nbText);                                       \
        SWITCH_METAL_DIAGNOSTIC(gProblems++);                       \
        noteProblem(nbText);                                        \
    } while (0)
// Work iPadOS sent the frontend that it refuses or answers with nothing: counted every time, its line (kits/SwitchRefusal,
// domain "frontend") once per process and reason, or reason and detail
#define NBREFUSE(reason, detail, ...)                                                                                  \
    do {                                                                                                               \
        SWITCH_METAL_DIAGNOSTIC(gProblems++);                                                                          \
        if (switch_refusal_first(switch_refusal_key_with(SWITCH_REFUSAL_KEY("frontend", reason), uint64_t(detail)))) { \
            NSString *nbText = [NSString stringWithFormat:__VA_ARGS__];                                                \
            noteProblem(nbText);                                                                                       \
            switch_refusal_log("frontend", reason, nbText.UTF8String);                                                 \
        }                                                                                                              \
    } while (0)
// The pixel formats textures were asked for (first eight distinct), with how many were refused: native_bridge_metal_formats.
static uint32_t gFormats[8];
static unsigned gFormatCount;
static std::atomic<uint64_t> gTextureRefusals;

#if SWITCH_FRAME_TIMELINE
static void noteFormat(uint32_t format, bool made)
{
    std::lock_guard<std::mutex> lock(gProblemLock);
    if (!made)
        gTextureRefusals++;
    for (unsigned i = 0; i < gFormatCount; i++)
        if (gFormats[i] == format)
            return;
    if (gFormatCount < 8)
        gFormats[gFormatCount++] = format;
}

#endif
static NSError *nbError(NSString *why)
{
    return [NSError errorWithDomain:@"org.blu-sharky.native-metal"
                               code:-1
                           userInfo:@{NSLocalizedDescriptionKey : why ?: @"refused"}];
}

static NSString *lastError()
{
    return [NSString stringWithUTF8String:nmb_last_error()];
}

// The render pipeline journal: one line per render pipeline creation (when, on which thread, how long, the outcome, both functions with
// every constant value, the attachments, blending and vertex descriptor) appended to pipelines.log in the runtime's cache directory, which
// the board log exports with the caches (CacheExport.h): every pipeline a boot made, or was refused, can be made again on the host.
// The file is opened once (fopen's "a": append, created 0666 less the umask) and each line is one write(2) of the line and its
// newline, so a line lands whole even if the process dies right after; an open per line would cost the thread making the state a
// sandbox evaluation and an APFS lookup each. A journal the sandbox refuses is not asked again. The device open renames a journal past 2 MiB before the first line (native_bridge_metal_open).
static std::mutex gJournalLock;
static std::string gJournalPath;
static uint64_t gJournalBegan;
static int gJournalFd = -1;
static bool gJournalRefused;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
constexpr bool kJournal = true;
#else
constexpr bool kJournal = false;  // its only readers are the host's harvest tools
#endif

static bool journalOn()
{
    return kJournal && !gJournalPath.empty();
}

static void journal(const std::string &line)
{
    if (!journalOn())
        return;
    std::lock_guard<std::mutex> lock(gJournalLock);
    if (gJournalFd < 0 && !gJournalRefused) {
        gJournalFd = open(gJournalPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        gJournalRefused = gJournalFd < 0;
    }
    if (gJournalFd < 0)
        return;
    std::string whole = line;
    whole += '\n';
    (void)write(gJournalFd, whole.data(), whole.size());
}

@interface NBObject : NSObject
@property (atomic, copy) NSString *label;
@end
@implementation NBObject

- (id<MTLDevice>)device
{
    return gDeviceObject;
}

- (void)doesNotRecognizeSelector:(SEL)selector
{
    if (gMissing)
        gMissing(class_getName(object_getClass(self)), sel_getName(selector));
    noteProblem(
        [NSString stringWithFormat:@"missing %s %s", class_getName(object_getClass(self)), sel_getName(selector)]);
    [super doesNotRecognizeSelector:selector];
}

@end

// Metal selectors this file lacks, answered zero (native_bridge_metal_answer_unimplemented). The protocols are walked once per
// selector and its signature kept, never per call: objc_copyProtocolList walks the shared cache's protocols (tens of thousands)
// and the runtime's and mallocs the list it returns, and an app can send an unimplemented selector many times a frame (per call,
// 70-80% of Maps' CPU and about one frame a second). A forwarded call costs the runtime's forwarding (an NSInvocation) and no more.
static void (*gAnsweredZero)(const char *, const char *);
static std::mutex gForwardLock;
static std::unordered_map<SEL, NSMethodSignature *> *gForwardSignatures;  // under gForwardLock; never shrinks
static std::atomic<uint64_t> gForwardCalls, gForwardUs;                   // the rates line's fwd=
#if SWITCH_FRAME_TIMELINE
static thread_local uint64_t tForwardBegan;
#endif

// The types of `selector` as the first MTL* protocol that declares it has them (required methods before optional ones), or null.
static const char *metalProtocolTypes(SEL selector)
{
    unsigned count = 0;
    Protocol *__unsafe_unretained *protocols = objc_copyProtocolList(&count);
    const char *types = nullptr;
    for (unsigned i = 0; i < count && !types; ++i) {
        if (strncmp(protocol_getName(protocols[i]), "MTL", 3) != 0)
            continue;
        for (int required = 1; required >= 0 && !types; --required) {
            const objc_method_description d = protocol_getMethodDescription(protocols[i], selector, required, YES);
            if (d.name)
                types = d.types;
        }
    }
    free(protocols);
    return types;
}

static NSMethodSignature *forwardSignature(id self, SEL _cmd, SEL selector)
{
    // NSObject's own answer (the methods the object really has) first; the same IMP serves every class it is added to.
    static auto base = (NSMethodSignature * (*)(id, SEL, SEL))
        class_getMethodImplementation([NSObject class], @selector(methodSignatureForSelector:));
    if (NSMethodSignature *signature = base(self, _cmd, selector))
        return signature;
    SWITCH_METAL_DIAGNOSTIC(tForwardBegan = nowUs());
    {
        std::lock_guard<std::mutex> lock(gForwardLock);
        if (gForwardSignatures) {
            auto found = gForwardSignatures->find(selector);
            if (found != gForwardSignatures->end())
                return found->second;
        }
    }
    // First sight of the selector (or one no Metal protocol declares, which raises as usual: not kept).
    const char *types = metalProtocolTypes(selector);
    NSMethodSignature *signature = types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
    if (!signature)
        return nil;
    std::lock_guard<std::mutex> lock(gForwardLock);
    if (!gForwardSignatures)
        gForwardSignatures = new std::unordered_map<SEL, NSMethodSignature *>;
    return gForwardSignatures->emplace(selector, signature).first->second;
}

static void forwardZero(id self, SEL, NSInvocation *invocation)
{
    const SEL selector = invocation.selector;
    Class cls = object_getClass(self);
    const uint64_t key = switch_refusal_key_with(
        switch_refusal_key_with(SWITCH_REFUSAL_KEY("frontend", "selector.zero"), uintptr_t(cls)), uintptr_t(selector));
    if (switch_refusal_first(key)) {
        char text[256];
        snprintf(text, sizeof text, "-[%s %s] answered zero", class_getName(cls), sel_getName(selector));
        switch_refusal_log("frontend", "selector.zero", text);
        if (gAnsweredZero)
            gAnsweredZero(class_getName(cls), sel_getName(selector));
    }
    if (const NSUInteger length = invocation.methodSignature.methodReturnLength) {
        uint64_t small[8] = {};
        void *zero = length <= sizeof small ? small : calloc(1, length);
        [invocation setReturnValue:zero];
        if (zero != small)
            free(zero);
    }
#if SWITCH_FRAME_TIMELINE
    gForwardCalls++;
    if (const uint64_t began = tForwardBegan) {
        gForwardUs += nowUs() - began;
        tForwardBegan = 0;
    }
#endif
}

// A selector some MTL* protocol declares that `cls` (or a subclass) lacks is answered zero/nil/NO, as an absent optional feature
// would be, `answered` told the class and selector once; respondsToSelector: still says NO, so optional paths take their
// unsupported branch. A selector no Metal protocol declares raises as usual (doesNotRecognizeSelector:). The plugin's: an exception
// in the render server restarts it into the same exception, no UI at all.
extern "C" void native_bridge_metal_answer_unimplemented(Class cls, void (*answered)(const char *, const char *))
{
    if (!cls)
        return;
    if (answered)
        gAnsweredZero = answered;
    class_addMethod(cls, @selector(methodSignatureForSelector:), (IMP)forwardSignature, "@@::");
    class_addMethod(cls, @selector(forwardInvocation:), (IMP)forwardZero, "v@:@");
}

// ---- resources ----
// The wait takes a strong snapshot of already-committed buffers; new commits require the
// caller's usual resource synchronization. Storage dependency checks are in the runtime.
static NSMutableSet *gInFlight;
// How long waitUntilScheduled waits before it asks whether a shared event holds the command buffer (heldBehindEvent).
static constexpr unsigned kHeldGraceMs = 500;
static std::atomic<uint64_t> gHeldReturns;

// The bound on every wait the frontend makes (no userland wait without an end): 20 s, past the kernel's GPU-wait panics
// (10 s) and Core Animation's own 5 s shared-event timeout. A host test may shorten it (NMB_WAIT_LIMIT_MS, macOS only).
static uint64_t waitLimitMs()
{
#if !TARGET_OS_IPHONE
    static const uint64_t limit = [] {
        const char *value = getenv("NMB_WAIT_LIMIT_MS");
        return value && atoll(value) > 0 ? (uint64_t)atoll(value) : 20000ull;
    }();
    return limit;
#else
    return 20000;
#endif
}

// Past the bound: one line naming the wait and the object, then abort(), for a crash report with the stack instead of a silent
// freeze (the userland twin of the kernel's missing-interrupt panic).
[[noreturn]] static void waitTooLong(const char *what, const void *object, uint64_t value)
{
    os_log_fault(OS_LOG_DEFAULT,
                 "native-metal: %{public}s %p not done after %llu ms (event value %llu): abort() for the stack", what,
                 object, (unsigned long long)waitLimitMs(), (unsigned long long)value);
#if !TARGET_OS_IPHONE
    fprintf(stderr, "native-metal: %s %p not done after %llu ms\n", what, object, (unsigned long long)waitLimitMs());
#endif
    abort();
}

static void waitForResource(id resource);
// Resource SPI the runtime has no state for: no protected content (Metal's default options), and no owner transfer
// (memory attribution is the kernel driver's, by the importing task), refused as not supported. makeAliasable and
// isAliasable are the runtime's (kind: buffer or texture): an automatic heap's resource gives its range to the next one
// the heap places. Espresso and MPS size their heaps for that; without it Vision's networks run out of heap mid-plan (`heap
// texture: OutOfMemory(Heap)`, then an Espresso exception).
#define NB_RESOURCE_SPI(kind)                                               \
    -(uint64_t)protectionOptions                                            \
    {                                                                       \
        return 0;                                                           \
    }                                                                       \
    -(id<MTLHeap>)heap                                                      \
    {                                                                       \
        return _parentHeap;                                                 \
    }                                                                       \
    -(BOOL)isAliasable                                                      \
    {                                                                       \
        return nmb_##kind##_is_aliasable(_handle);                          \
    }                                                                       \
    -(int)setOwnerWithIdentity : (unsigned)identity                         \
    {                                                                       \
        (void)identity;                                                     \
        return 46; /* KERN_NOT_SUPPORTED */                                 \
    }                                                                       \
    -(void)waitUntilComplete                                                \
    {                                                                       \
        waitForResource(self);                                              \
    }                                                                       \
    -(void)makeAliasable                                                    \
    {                                                                       \
        if (!nmb_##kind##_make_aliasable(_handle))                          \
            NBLOG(@"native-metal: makeAliasable refused: %@", lastError()); \
    }
// Every allocation of this device is resident for its whole life (NBResidencySet), so naming a residency set changes nothing
// a queue, command buffer or encoder does.
#define NB_RESIDENCY_SETS(verb)                                                                                      \
    -(void)verb##ResidencySet : (id<MTLResidencySet>)set API_AVAILABLE(ios(18.0), macos(15.0))                       \
    {                                                                                                                \
        (void)set;                                                                                                   \
    }                                                                                                                \
    -(void)verb##ResidencySets : (const id<MTLResidencySet> _Nonnull[])sets count : (NSUInteger)count API_AVAILABLE( \
                                                                                        ios(18.0), macos(15.0))      \
    {                                                                                                                \
        (void)sets;                                                                                                  \
        (void)count;                                                                                                 \
    }

@interface NBBuffer : NBObject
@property (nonatomic) NmbBuffer *handle;
@property (nonatomic) MTLResourceOptions options;
@property (nonatomic, copy) void (^deallocator)(void *, NSUInteger);
@property (nonatomic) void *wrapped;
@property (nonatomic) IOSurfaceRef surfaceOwner;      // keeps a shared import's CPU mapping alive
@property (nonatomic) surface_layout::Import layout;  // a surface import's layout (SurfaceLayout.h)
@property (nonatomic)
    uint32_t gpuDirty;  // the GPU wrote this host import since its lines were last invalidated (invalidateForCpu)
@property (nonatomic, strong) id<MTLHeap> parentHeap;  // the heap that made the buffer, nil otherwise
@property (nonatomic) NSUInteger heapOffset;
- (bool)invalidateForCpu;
- (void)gpuWrote;
@end

// Two-plane render targets. RenderBox renders widget and poster snapshots into '_RB4' surfaces as RGB8_444_2P (542) on an Apple3
// device or later (RB::ColorMode::pixel_format), with blends and loads; the runtime renders no pass into a two-plane code format
// (it refuses every WidgetRenderer_Default pass). Metal holds such an attachment as half texels and stores each channel's
// code round(h * 255), green into the luma plane and blue and red as the chroma plane's pairs (an M4 under macOS 27.0.1: clears,
// source-over blends and loads into '_RB4' beside RGBA8Unorm, which rounds its float tile and lands up to 1 off). So a pass renders
// such an attachment into the texture's RGBA16Float stand-in (made at its first pass and kept with it), the pipelines target
// RGBA16Float in its place (renderDescriptor), a pass that loads it (or stores a DontCare load) first moves the planes' codes into
// the stand-in, and the
// encoder's end stores the stand-in into the planes through R8Unorm and RG8Unorm textures over them (TwoPlaneStandIn.metal): one
// full-target draw each, ordered on the GPU like any pass, no CPU wait. Samples, blits and CPU copies of the texture read and
// write its planes directly.
static bool rendersThroughStandIn(NSUInteger format)
{
    return format == 542;  // MTLPixelFormatRGB8_444_2P
}
@class NBTwoPlaneStandIn;
// RG8Unorm_sRGB held as RGBA8Unorm_sRGB (heldAsRGBA8): a texture the device samples with its sRGB decode before the filter, as
// Metal's does, where the device has no sampled R8G8_SRGB (GM20B refuses every such draw with "Format VK_FORMAT_R8G8_SRGB
// SampledImage", and KaleidoscopePoster's wallpaper draws black). Its texels are the RG pairs widened to (r, g, 0, 255), which Metal's own RG8 texture
// reads as; its CPU copies and buffer blits narrow and widen them (rgWide).
static bool gHoldRG8SrgbAsRGBA8;
@interface NBTexture : NBObject
@property (nonatomic) NmbTexture *handle;
@property (nonatomic) bool rgWide;  // RG8Unorm_sRGB held as RGBA8Unorm_sRGB (gHoldRG8SrgbAsRGBA8)
// A two-plane render target's stand-in (above), made at its first pass.
@property (nonatomic, strong) NBTwoPlaneStandIn *standIn;
@property (nonatomic) NmbTextureDescriptor descriptor;
@property (nonatomic, strong) NBBuffer *buffer;
@property (nonatomic) IOSurfaceRef surface;
@property (nonatomic) NSUInteger offset, rowBytes;
@property (nonatomic, strong) id<MTLTexture> parent;
@property (nonatomic, strong) id<MTLHeap> parentHeap;
@property (nonatomic) NSUInteger heapOffset;
@property (atomic) bool gpuWritesDeclared;
- (NBBuffer *)hostImport;
@end

static void declareGpuWrites(IOSurfaceRef surface);

// The IOSurface a texture's storage is, through views and buffer-backed textures; nullptr: none.
static IOSurfaceRef textureSurface(NBTexture *t)
{
    for (unsigned depth = 0; t && depth < 8; depth++) {
        if (t.surface)
            return t.surface;
        if (t.buffer.surfaceOwner)
            return t.buffer.surfaceOwner;
        t = [(id)t.parent isKindOfClass:[NBTexture class]] ? (NBTexture *)t.parent : nil;
    }
    return nullptr;
}

static void textureGpuWrites(NBTexture *t)
{
    if (!t || t.gpuWritesDeclared)
        return;
    if (IOSurfaceRef surface = textureSurface(t))
        declareGpuWrites(surface);
    t.gpuWritesDeclared = true;
}

@implementation NBBuffer

// A host import the GPU wrote (gpuWrote, set at the writing command buffer's completion): invalidate its lines so a CPU read in
// this process reads what the GPU wrote, not lines it cached before (a fresh surface's zeros, a frame it read earlier). Lazy: the
// flag is set cheaply at completion and the invalidate paid here, only when the CPU reads (getBytes, contents), so a surface the
// CPU never reads (the framebuffer) costs nothing. The kernel's IOSurface-lock hook does the same for readers in other processes.
- (bool)invalidateForCpu
{
    if (!__atomic_exchange_n(&_gpuDirty, 0, __ATOMIC_ACQUIRE))
        return false;
    const size_t bytes = nmb_buffer_length(_handle);
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    switch_dcache_clean_invalidate(nmb_buffer_contents(_handle), bytes);
    SWITCH_METAL_DIAGNOSTIC(gReadbackInvalidates++);
    SWITCH_METAL_DIAGNOSTIC(gReadbackBytes += bytes);
    SWITCH_METAL_DIAGNOSTIC(gReadbackUs += nowUs() - began);
    return true;
}

- (void)gpuWrote
{
    __atomic_store_n(&_gpuDirty, 1, __ATOMIC_RELEASE);
}

- (void)dealloc
{
    const NSUInteger length = nmb_buffer_length(_handle);
    nmb_buffer_release(_handle);
    if (_deallocator)
        _deallocator(_wrapped, length);
    if (_surfaceOwner)
        CFRelease(_surfaceOwner);
}

- (NSUInteger)length
{
    return nmb_buffer_length(_handle);
}

- (void *)contents
{
    [self invalidateForCpu];
    return nmb_buffer_contents(_handle);
}  // a CPU read after a GPU write sees the GPU's bytes

- (uint64_t)gpuAddress
{
    return nmb_buffer_gpu_address(_handle);
}

- (MTLResourceOptions)resourceOptions
{
    return _options;
}

- (MTLStorageMode)storageMode
{
    return (MTLStorageMode)((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return (MTLCPUCacheMode)((_options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return (MTLHazardTrackingMode)((_options & MTLResourceHazardTrackingModeMask) >>
                                   MTLResourceHazardTrackingModeShift);
}

- (void)didModifyRange:(NSRange)range
{
    (void)range;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const uint64_t answer = nmb_buffer_set_purgeable_state(_handle, state);
    if (!answer)
        NBLOG(@"native-metal: buffer setPurgeableState refused: %@", lastError());
    return (MTLPurgeableState)answer;
}

- (NSUInteger)allocatedSize
{
    return nmb_buffer_length(_handle);
}

- (IOSurfaceRef)iosurface
{
    return nullptr;
}  // a buffer is never the backing of a surface here (surfaces back textures)
NB_RESOURCE_SPI(buffer)
@end
// Reuse only an import still held by a texture/view or pending command. Weak
// values never extend a GPU allocation's lifetime. The strong buffer snapshot
// and its retained surface prevent object/address reuse during a hit. A new
// mapping, allocation identity or size needs a separate import; ordinary
// caller-owned no-copy buffers do not enter this cache.
static NSMapTable<NSValue *, NBBuffer *> *gSurfaceImports;
static std::mutex gSurfaceImportLock;
// Associations kept before the dead ones are dropped. Above the live imports a busy screen holds (307), so a live import's
// association is never the one dropped: a lookup that missed it would import the same surface a second time.
static constexpr NSUInteger kSurfaceImportKeys = 1024;

// The NVK the device was opened over (native_bridge_metal_open's loader), whatever its file is named (rawpack's twin copy).
static std::string gNvkPath;

// A symbol of NVK (the one the device opened, else libvulkan_nouveau.dylib: the one this process loaded, else the one beside
// this image), which the Vulkan loader opened RTLD_LOCAL: looked up through that image rather than RTLD_DEFAULT.
static void *nvkSymbol(const char *name)
{
    static void *const nvk = []() -> void * {
        if (void *opened = gNvkPath.empty() ? nullptr : dlopen(gNvkPath.c_str(), RTLD_LAZY | RTLD_NOLOAD))
            return opened;
        static const char kNvk[] = "/libvulkan_nouveau.dylib";
        for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
            const char *image = _dyld_get_image_name(i);
            const size_t length = image ? strlen(image) : 0;
            if (length > sizeof kNvk - 1 && !strcmp(image + length - (sizeof kNvk - 1), kNvk))
                return dlopen(image, RTLD_LAZY | RTLD_NOLOAD);
        }
        Dl_info info = {};
        if (!dladdr(reinterpret_cast<const void *>(&nvkSymbol), &info) || !info.dli_fname)
            return nullptr;
        std::string path(info.dli_fname);
        const size_t slash = path.rfind('/');
        if (slash == std::string::npos)
            return nullptr;
        path.replace(slash + 1, std::string::npos, "libvulkan_nouveau.dylib");
        return dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    }();
    return nvk ? dlsym(nvk, name) : nullptr;
}

// The IOSurface the next host import on this thread covers, so the kernel keeps that import coherent at every process's
// IOSurfaceLock/Unlock. NVK imports it as only sampled by the GPU until declareGpuWrites names it.
static void noteImportSurface(IOSurfaceID surfaceId, const void *base, size_t bytes)
{
    using Note = void (*)(uint32_t, const void *, uint64_t);
    static const Note note = reinterpret_cast<Note>(nvkSymbol("switch_nvk_import_surface"));
    if (note)
        note(surfaceId, base, bytes);
}

// Before the GPU first renders, blits or stores into the surface: from then on each submission marks its imports
// GPU-written, so the next CPU lock in any process invalidates them. A surface never named here is taken as sampled only.
static void declareGpuWrites(IOSurfaceRef surface)
{
    using Declare = bool (*)(uint32_t);
    static const Declare declare = reinterpret_cast<Declare>(nvkSymbol("switch_nvk_surface_gpu_writes"));
    if (!surface || (declare && declare(IOSurfaceGetID(surface))))
        return;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        NBLOG(@"native-metal: the GPU's writes to IOSurface %u were not declared: CPU readers may see stale lines",
              IOSurfaceGetID(surface));
}

static NmbBuffer *bufferOverForeign(const struct switch_foreign_memory *memory, void *bytes, uint64_t length,
                                    uint64_t options)
{
    using Foreign =
        void (*)(const void *, uint64_t, uint64_t, uint64_t, uint32_t, void (*)(void *), void (*)(void *), void *);
    static const Foreign foreign = reinterpret_cast<Foreign>(nvkSymbol("switch_nvk_import_foreign"));
    if (!foreign || !gDevice)
        return nullptr;
    foreign(memory->cpu, memory->bytes, memory->handle, memory->va, memory->kind, memory->hold, memory->drop,
            memory->lease);
    NmbBuffer *handle = nmb_buffer_new_no_copy(gDevice, bytes, length, options);
    foreign(nullptr, 0, 0, 0, 0, nullptr, nullptr, nullptr);
    return handle;
}

extern "C" id native_bridge_metal_buffer_over_foreign(const struct switch_foreign_memory *memory, void *bytes,
                                                      NSUInteger length, NSUInteger options)
{
    NmbBuffer *handle = memory ? bufferOverForeign(memory, bytes, length, options) : nullptr;
    if (!handle) {
        NBLOG(@"native-metal: buffer over foreign memory refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    buffer.wrapped = bytes;
    return (id<MTLBuffer>)buffer;
}

static std::atomic<switch_surface_importer> gSurfaceImporter{nullptr};

extern "C" void native_bridge_metal_set_surface_importer(switch_surface_importer importer)
{
    gSurfaceImporter.store(importer);
}

// The layouts surface imports were answered, by surface and mapping: what an import the runtime joins to a live one of the same
// bytes (no BoImport, so no answer of its own) was bound as. Bounded: past the cap it starts again, and a join it no longer knows
// takes no layout (its textures that would need one are refused).
static std::unordered_map<std::string, surface_layout::Import> gSurfaceLayouts;
static constexpr size_t kSurfaceLayouts = 4096;

static std::string surfaceLayoutKey(IOSurfaceRef surface, void *base, size_t bytes)
{
    char key[64];
    snprintf(key, sizeof key, "%u/%lx/%zx", IOSurfaceGetID(surface), (unsigned long)(uintptr_t)base, bytes);
    return key;
}

// The surface's import, the layout asked by `ask`'s shape (the raw GM20B path's import in the render server: it asked; another
// process: this import asks at its BoImport, NVK binding a block-linear surface under the kind its texels are written in).
static NmbBuffer *importSurface(IOSurfaceRef surface, void *base, size_t bytes, const surface_layout::Shape &ask,
                                surface_layout::Import &layout)
{
    const surface_layout::Nvk &nvk = surface_layout::nvk(nvkSymbol);
    const switch_surface_importer importer = gSurfaceImporter.load();
    struct switch_foreign_memory memory = {};
    if (importer && importer(IOSurfaceGetID(surface), surface, base, bytes, &memory)) {
        layout.handle = memory.handle;
        if (memory.kind == surface_layout::kKind) {
            layout.layout = surface_layout::kTiled;
        } else {  // pitch, or a surface the raw path took as rows without asking: never decided block-linear from here
            layout.asked = ask;
            layout.asked.flags &= ~surface_layout::kWrites;
            layout.layout =
                nvk.layout ? nvk.layout(memory.handle, ask.width, ask.height, ask.bpp, ask.rowBytes, layout.asked.flags)
                           : surface_layout::kPitch;
        }
        NmbBuffer *handle = bufferOverForeign(&memory, base, bytes, MTLResourceStorageModeShared);
        memory.drop(memory.lease);
        return handle;
    }
    noteImportSurface(IOSurfaceGetID(surface), base, bytes);
    if (nvk.importShape)
        nvk.importShape(ask.width, ask.height, ask.bpp, ask.rowBytes, ask.flags);
    NmbBuffer *handle = nmb_buffer_new_no_copy(gDevice, base, bytes, MTLResourceStorageModeShared);
    layout.layout = nvk.importLayout ? nvk.importLayout(&layout.handle) : surface_layout::kPitch;
    layout.asked = ask;
    noteImportSurface(0, nullptr, 0);
    const std::string key = surfaceLayoutKey(surface, base, bytes);
    if (layout.handle) {
        if (gSurfaceLayouts.size() >= kSurfaceLayouts)
            gSurfaceLayouts.clear();
        gSurfaceLayouts[key] = layout;
    } else if (handle) {
        const auto known = gSurfaceLayouts.find(key);
        layout = known != gSurfaceLayouts.end() ? known->second : surface_layout::Import{};
    }
    return handle;
}

static bool usageWrites(MTLTextureUsage usage)
{
    return usage == MTLTextureUsageUnknown || (usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite));
}

static NBBuffer *surfaceBuffer(IOSurfaceRef surface, void *base, size_t bytes, const surface_layout::Shape &ask)
{
    struct Key {
        uint64_t identity, address, bytes;
    };

    const Key value = {IOSurfaceGetID(surface), (uintptr_t)base, bytes};
    NSValue *key = value.identity ? [NSValue valueWithBytes:&value objCType:@encode(Key)] : nil;
    std::lock_guard<std::mutex> lock(gSurfaceImportLock);
    if (!gSurfaceImports)
        gSurfaceImports = [NSMapTable strongToWeakObjectsMapTable];
    NBBuffer *buffer = key ? [gSurfaceImports objectForKey:key] : nil;
    if (buffer) {
        SWITCH_METAL_DIAGNOSTIC(gSurfaceImportHits++);
        return buffer;
    }
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    surface_layout::Import layout;
    NmbBuffer *handle = importSurface(surface, base, bytes, ask, layout);
    SWITCH_METAL_DIAGNOSTIC(gTimeSurfaceImport.add(began));
    if (!handle) {
        NBLOG(@"native-metal: surface import refused: %@", lastError());
        return nil;
    }
    buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = MTLResourceStorageModeShared;
    buffer.surfaceOwner = (IOSurfaceRef)CFRetain(surface);
    buffer.layout = layout;
    if (key) {
        // Bound metadata without retaining old frames. Evicting an association is
        // harmless: its owners still retain the actual import until their own end.
        if (gSurfaceImports.count >= kSurfaceImportKeys) {
            for (NSValue *old in gSurfaceImports.keyEnumerator.allObjects)
                if (![gSurfaceImports objectForKey:old])
                    [gSurfaceImports removeObjectForKey:old];
            if (gSurfaceImports.count >= kSurfaceImportKeys)
                [gSurfaceImports removeObjectForKey:gSurfaceImports.keyEnumerator.nextObject];
        }
        [gSurfaceImports setObject:buffer forKey:key];
    }
    return buffer;
}

// RG pairs at `from` (rows `fromRow` apart, images `fromImage` apart) widened to RGBA texels at `to` (rows `w` * 4 apart, images
// `h` rows apart): (r, g, 0, 255). narrowRG: the other way.
static void widenRG(const uint8_t *from, size_t fromRow, size_t fromImage, uint8_t *to, size_t w, size_t h, size_t d)
{
    for (size_t z = 0; z < d; z++)
        for (size_t y = 0; y < h; y++) {
            const uint8_t *src = from + z * fromImage + y * fromRow;
            uint8_t *dst = to + ((z * h + y) * w) * 4;
            for (size_t x = 0; x < w; x++) {
                dst[4 * x] = src[2 * x];
                dst[4 * x + 1] = src[2 * x + 1];
                dst[4 * x + 2] = 0;
                dst[4 * x + 3] = 255;
            }
        }
}

static void narrowRG(const uint8_t *from, size_t w, size_t h, size_t d, uint8_t *to, size_t toRow, size_t toImage)
{
    for (size_t z = 0; z < d; z++)
        for (size_t y = 0; y < h; y++) {
            const uint8_t *src = from + ((z * h + y) * w) * 4;
            uint8_t *dst = to + z * toImage + y * toRow;
            for (size_t x = 0; x < w; x++) {
                dst[2 * x] = src[4 * x];
                dst[2 * x + 1] = src[4 * x + 1];
            }
        }
}

// Whether a new texture of `d` is held as RGBA8Unorm_sRGB (gHoldRG8SrgbAsRGBA8): RG8Unorm_sRGB, single-sample, sampled and copied
// only (a pass, a shader write or a view of another size would see the wider texels; those keep the runtime's own image).
static bool heldAsRGBA8(MTLTextureDescriptor *d)
{
    return gHoldRG8SrgbAsRGBA8 && d.pixelFormat == (MTLPixelFormat)31 /* RG8Unorm_sRGB */ && d.sampleCount <= 1 &&
           !(d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite)) &&
           d.textureType != MTLTextureType2DMultisample && d.textureType != MTLTextureType2DMultisampleArray;
}

// A view's format of an RG8 texture held as RGBA8: its RGBA8 twin (RG8Unorm -> RGBA8Unorm, RG8Unorm_sRGB -> RGBA8Unorm_sRGB), or
// Invalid for another.
static MTLPixelFormat wideViewFormat(MTLPixelFormat format)
{
    switch (format) {
    case MTLPixelFormatRG8Unorm:
        return MTLPixelFormatRGBA8Unorm;
    case (MTLPixelFormat)31 /* RG8Unorm_sRGB */:
        return MTLPixelFormatRGBA8Unorm_sRGB;
    default:
        return MTLPixelFormatInvalid;
    }
}

static bool textureDescriptor(MTLTextureDescriptor *d, NmbTextureDescriptor *out)
{
    if (!d)
        return false;
    *out = {(uint64_t)d.textureType,
            (uint64_t)d.pixelFormat,
            d.width,
            d.height,
            d.depth,
            d.mipmapLevelCount,
            d.sampleCount,
            d.arrayLength,
            (uint64_t)d.usage,
            (uint64_t)d.resourceOptions};
    return true;
}

@implementation NBTexture

- (void)dealloc
{
    nmb_texture_release(_handle);
    if (_surface)
        CFRelease(_surface);
}

- (MTLTextureType)textureType
{
    return (MTLTextureType)_descriptor.type;
}

- (MTLPixelFormat)pixelFormat
{
    return (MTLPixelFormat)_descriptor.format;
}

- (NSUInteger)width
{
    return _descriptor.width;
}

- (NSUInteger)height
{
    return _descriptor.height;
}

- (NSUInteger)depth
{
    return _descriptor.depth;
}

- (NSUInteger)mipmapLevelCount
{
    return _descriptor.levels;
}

- (NSUInteger)sampleCount
{
    return _descriptor.samples;
}

- (NSUInteger)arrayLength
{
    return _descriptor.array_length;
}

- (MTLTextureUsage)usage
{
    return (MTLTextureUsage)_descriptor.usage;
}

- (MTLResourceOptions)resourceOptions
{
    return (MTLResourceOptions)_descriptor.options;
}

- (MTLStorageMode)storageMode
{
    return (MTLStorageMode)((_descriptor.options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return (MTLCPUCacheMode)((_descriptor.options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}

- (BOOL)isFramebufferOnly
{
    return NO;
}

- (IOSurfaceRef)iosurface
{
    return _surface;
}

- (NSUInteger)iosurfacePlane
{
    return 0;
}

- (id<MTLBuffer>)buffer
{
    return _surface ? nil : (id<MTLBuffer>)_buffer;
}

// The host import whose pages hold this texture's texels (a surface's, or a no-copy buffer's), for the lazy readback invalidate.
- (NBBuffer *)hostImport
{
    return (_surface || (_buffer && _buffer.wrapped)) ? _buffer : nil;
}

- (NSUInteger)bufferOffset
{
    return _offset;
}

- (NSUInteger)bufferBytesPerRow
{
    return _rowBytes;
}

- (id<MTLTexture>)parentTexture
{
    return _parent;
}

- (NSUInteger)rotation
{
    return 0;
}

// Nothing here is compressed (NVK on GM20B has no compression) or a drawable; views take no swizzle (identity).
- (MTLTextureCompressionType)compressionType
{
    return MTLTextureCompressionTypeLossless;
}

- (BOOL)isCompressed
{
    return NO;
}

- (BOOL)isDrawable
{
    return NO;
}

- (MTLTextureSwizzleChannels)swizzle
{
    return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                         MTLTextureSwizzleAlpha);
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const uint64_t answer = nmb_texture_set_purgeable_state(_handle, state);
    if (!answer)
        NBLOG(@"native-metal: texture setPurgeableState refused: %@", lastError());
    return (MTLPurgeableState)answer;
}
NB_RESOURCE_SPI(texture)

- (id<MTLTexture>)view:(NmbTexture *)handle
{
    if (!handle) {
        NBLOG(@"native-metal: texture view refused: %@", lastError());
        return nil;
    }
    NBTexture *view = [NBTexture new];
    view.handle = handle;
    NmbTextureDescriptor descriptor;
    nmb_texture_describe(handle, &descriptor);
    view.descriptor = descriptor;
    view.parent = (id<MTLTexture>)self;
    view.buffer = _buffer;
    view.offset = _offset;
    view.rowBytes = _rowBytes;
    if (_surface)
        view.surface = (IOSurfaceRef)CFRetain(_surface);
    return (id<MTLTexture>)view;
}

// A view of an RG8 texture held as RGBA8 is its RGBA8 twin's (wideViewFormat), reporting the format asked for.
- (id<MTLTexture>)wideView:(NmbTexture *)handle format:(MTLPixelFormat)format
{
    NBTexture *view = (NBTexture *)[self view:handle];
    if (view) {
        NmbTextureDescriptor descriptor = view.descriptor;
        descriptor.format = format;
        view.descriptor = descriptor;
        view.rgWide = true;
    }
    return (id<MTLTexture>)view;
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
    if (_rgWide) {
        const MTLPixelFormat wide = wideViewFormat(format);
        if (wide == MTLPixelFormatInvalid) {
            NBLOG(@"native-metal: texture view refused: an RG8Unorm_sRGB texture held as RGBA8 viewed as %lu",
                  (unsigned long)format);
            return nil;
        }
        return [self wideView:nmb_texture_new_view(_handle, wide, 0, nullptr, nullptr, true) format:format];
    }
    return [self view:nmb_texture_new_view(_handle, format, 0, nullptr, nullptr, true)];
}

- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
{
    const uint64_t l[2] = {levels.location, levels.length}, s[2] = {slices.location, slices.length};
    if (_rgWide) {
        const MTLPixelFormat wide = wideViewFormat(format);
        if (wide == MTLPixelFormatInvalid) {
            NBLOG(@"native-metal: texture view refused: an RG8Unorm_sRGB texture held as RGBA8 viewed as %lu",
                  (unsigned long)format);
            return nil;
        }
        return [self wideView:nmb_texture_new_view(_handle, wide, type, l, s, false) format:format];
    }
    return [self view:nmb_texture_new_view(_handle, format, type, l, s, false)];
}

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    NBApiTime timed;
    const uint64_t region[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    uint64_t length = (r.size.depth > 1 ? image * (r.size.depth - 1) : 0) + row * r.size.height;
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    std::vector<uint8_t> wide;
    if (_rgWide && bytes) {  // the RG pairs widened to the RGBA8 texels the texture holds
        const size_t w = r.size.width, h = r.size.height, d = std::max<size_t>(r.size.depth, 1);
        wide.resize(w * h * d * 4);
        widenRG((const uint8_t *)bytes, row, image, wide.data(), w, h, d);
        bytes = wide.data();
        row = w * 4;
        image = w * 4 * h;
        length = wide.size();
    }
    const bool replaced = nmb_texture_replace(_handle, region, level, slice, bytes, row, image, length);
    SWITCH_METAL_DIAGNOSTIC(gTimeTexelCopy.add(began));
    if (!replaced)
        NBLOG(@"native-metal: replaceRegion refused: %@", lastError());
}

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
{
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row * r.size.height];
}

- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    const uint64_t region[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    const uint64_t length = (r.size.depth > 1 ? image * (r.size.depth - 1) : 0) + row * r.size.height;
    [[self hostImport] invalidateForCpu];  // a read back of what the GPU wrote this texture's surface/buffer pages
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    bool read;
    if (_rgWide && bytes) {  // the RGBA8 texels narrowed to the RG pairs the program reads
        const size_t w = r.size.width, h = r.size.height, d = std::max<size_t>(r.size.depth, 1);
        std::vector<uint8_t> wide(w * h * d * 4);
        read = nmb_texture_get_bytes(_handle, wide.data(), w * 4, w * 4 * h, wide.size(), region, level, slice);
        if (read)
            narrowRG(wide.data(), w, h, d, (uint8_t *)bytes, row, image);
    } else {
        read = nmb_texture_get_bytes(_handle, bytes, row, image, length, region, level, slice);
    }
    SWITCH_METAL_DIAGNOSTIC(gTimeTexelCopy.add(began));
    if (!read)
        NBLOG(@"native-metal: getBytes refused: %@", lastError());
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
    [self getBytes:bytes bytesPerRow:row bytesPerImage:row * r.size.height fromRegion:r mipmapLevel:level slice:0];
}

@end

// The host import whose pages hold `owner`'s texels (a surface- or no-copy-buffer-backed texture, or such a buffer), for the lazy
// readback invalidate: nil for an ordinary private/shared resource.
static NBBuffer *hostImportOf(id owner)
{
    if ([owner isKindOfClass:[NBTexture class]])
        return [(NBTexture *)owner hostImport];
    if ([owner isKindOfClass:[NBBuffer class]] && ([(NBBuffer *)owner surfaceOwner] || [(NBBuffer *)owner wrapped]))
        return (NBBuffer *)owner;
    return nil;
}

static NBTexture *textureOnBuffer(NBBuffer *buffer, MTLTextureDescriptor *d, NSUInteger offset, NSUInteger row,
                                  bool surface)
{
    NmbTextureDescriptor descriptor;
    if (!buffer || !textureDescriptor(d, &descriptor))
        return nil;
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    NmbTexture *handle = nmb_texture_new_on_buffer(buffer.handle, &descriptor, offset, row, surface);
    SWITCH_METAL_DIAGNOSTIC(if (surface) gTimeSurfaceTexture.add(began));
    if (!handle) {
        NBLOG(@"native-metal: texture over buffer refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.buffer = buffer;
    texture.offset = offset;
    texture.rowBytes = row;
    return texture;
}

// The device's optimal image of `d` over `buffer`'s memory from `offset`: texels another driver of the process, or the kernel's
// block-linear surface layout, lays out as the device does (SurfaceLayout.h).
static NBTexture *tiledTextureOnBuffer(NBBuffer *buffer, MTLTextureDescriptor *d, NSUInteger offset)
{
    NmbTextureDescriptor descriptor;
    if (!buffer || !textureDescriptor(d, &descriptor))
        return nil;
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    NmbTexture *handle = nmb_texture_new_tiled_in_place(buffer.handle, &descriptor, offset);
    SWITCH_METAL_DIAGNOSTIC(gTimeSurfaceTexture.add(began));
    if (!handle) {
        NBLOG(@"native-metal: tiled texture over buffer refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.buffer = buffer;
    texture.offset = offset;
    return texture;
}

// A surface use refused because its layout does not fit (a texture of another shape or type over a block-linear surface, a
// buffer or a planar texture over one, no answer): once per surface's shape, use and answer.
static void refuseLayout(IOSurfaceRef surface, const char *use, uint32_t layout)
{
    static NSMutableSet *said = [NSMutableSet set];
    NSString *why = [NSString stringWithFormat:@"%s over a %zux%zu surface of %zu-byte texels: layout %u", use,
                                               IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
                                               IOSurfaceGetBytesPerElement(surface), layout];
    @synchronized(said) {
        if ([said containsObject:why])
            return;
        [said addObject:why];
    }
    NBLOG(@"native-metal: refused for its surface's layout: %@", why);
}

// The shape a surface's own texels (its first plane's) ask, sampled
static surface_layout::Shape surfaceShape(IOSurfaceRef surface)
{
    if (!IOSurfaceGetPlaneCount(surface))
        return {uint32_t(IOSurfaceGetWidth(surface)), uint32_t(IOSurfaceGetHeight(surface)),
                uint32_t(IOSurfaceGetBytesPerElement(surface)), uint32_t(IOSurfaceGetBytesPerRow(surface)), 0};
    return {uint32_t(IOSurfaceGetWidthOfPlane(surface, 0)), uint32_t(IOSurfaceGetHeightOfPlane(surface, 0)),
            uint32_t(IOSurfaceGetBytesPerElementOfPlane(surface, 0)),
            uint32_t(IOSurfaceGetBytesPerRowOfPlane(surface, 0)), 0};
}

// A texture over plane `plane` of a planar surface, made by the runtime over the surface's whole allocation with the surface's facts
// (its OSType, plane 0's size, plane 1's place, its kIOSurfaceYCbCrMatrix): Metal's two-plane formats over plane 0 and plain
// formats over either plane, as the runtime holds them (`new_texture_with_iosurface_plane`):
//  - 'b3a8' as RGB10A8_2P_XR10 (550/551): WebKit's deep-color tiles (without them every web page draws black);
//  - '_RB4' as RGB8_444_2P (542): the widget and poster snapshots Chrono and PosterBoard hand the render server (without them no
//    widget draws and Core Animation logs "Failed to allocate MTLTexture from IOSurface" for each);
//  - '420f'/'420v' (and the other CoreVideo YCbCr surfaces) as YCBCR8_420_2P (500), its sRGB twin (520) or RGB8_420_2P (540): decoded
//    photos and video frames, converted at each encoder that samples them (ycbcr.rs; M2V_YCBCR_MEMO skips unchanged ones);
//  - R8Unorm/RG8Unorm and kin over plane 0 or 1 (the decoders' and Core Image's per-plane views).
// The runtime refuses what it does not hold exactly (logged once per reason; the texture is then nil).
static NBTexture *planarSurfaceTexture(MTLTextureDescriptor *d, IOSurfaceRef surface, NSUInteger plane)
{
    const size_t planes = IOSurfaceGetPlaneCount(surface);
    if (planes < 1 || planes > 2 || plane >= planes)
        return nil;
    const uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(surface);
    const uintptr_t at = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface, plane);
    const uintptr_t chroma = planes > 1 ? (uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface, 1) : 0;
    const size_t page = vm_page_size, allocation = IOSurfaceGetAllocSize(surface);
    if (!base || (base & (page - 1)) || at < base || (planes > 1 && chroma < base) || !allocation ||
        allocation > SIZE_MAX - (page - 1))
        return nil;
    NBBuffer *buffer =
        surfaceBuffer(surface, (void *)base, (allocation + page - 1) & ~(page - 1), surfaceShape(surface));
    NmbTextureDescriptor descriptor;
    if (!buffer || !textureDescriptor(d, &descriptor))
        return nil;
    if (buffer.layout.layout != surface_layout::kPitch) {
        refuseLayout(surface, "a planar texture", buffer.layout.layout);
        return nil;
    }
    // kIOSurfaceYCbCrMatrix: the CoreVideo attachment's string ("ITU_R_709_2", ...), which the converted formats read.
    char matrix[32] = {};
    if (CFTypeRef value = IOSurfaceCopyValue(surface, CFSTR("IOSurfaceYCbCrMatrix"))) {
        if (CFGetTypeID(value) == CFStringGetTypeID())
            CFStringGetCString((CFStringRef)value, matrix, sizeof matrix, kCFStringEncodingUTF8);
        CFRelease(value);
    }
    const NSUInteger offset = at - base, row = IOSurfaceGetBytesPerRowOfPlane(surface, plane);
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    NmbTexture *handle = nmb_texture_new_on_surface_planes(
        buffer.handle, &descriptor, offset, row, IOSurfaceGetPixelFormat(surface), plane,
        IOSurfaceGetWidthOfPlane(surface, 0), IOSurfaceGetHeightOfPlane(surface, 0), planes > 1 ? chroma - base : 0,
        planes > 1 ? IOSurfaceGetBytesPerRowOfPlane(surface, 1) : 0, matrix[0] ? matrix : nullptr);
    SWITCH_METAL_DIAGNOSTIC(gTimeSurfaceTexture.add(began));
    if (!handle) {
        // once per format, OSType, plane and reason: Core Animation asks again every frame
        static NSMutableSet *said = [NSMutableSet set];
        const uint32_t fourcc = IOSurfaceGetPixelFormat(surface);
        NSString *why = [NSString stringWithFormat:@"%lu %c%c%c%c plane %lu: %@", (unsigned long)d.pixelFormat,
                                                   (char)(fourcc >> 24), (char)(fourcc >> 16), (char)(fourcc >> 8),
                                                   (char)fourcc, (unsigned long)plane, lastError()];
        bool first = false;
        @synchronized(said) {
            if (![said containsObject:why]) {
                [said addObject:why];
                first = true;
            }
        }
        if (first)
            NBLOG(@"native-metal: planar surface texture refused (format, fourcc, plane): %@", why);
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.buffer = buffer;
    texture.offset = offset;
    texture.rowBytes = row;
    texture.surface = (IOSurfaceRef)CFRetain(surface);
    if (usageWrites(d.usage))
        textureGpuWrites(texture);
    return texture;
}
@implementation NBBuffer (Textures)

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)row
{
    return (id<MTLTexture>)textureOnBuffer(self, d, offset, row, false);
}

- (id<MTLTexture>)newLinearTextureWithDescriptor:(MTLTextureDescriptor *)d
                                          offset:(NSUInteger)offset
                                     bytesPerRow:(NSUInteger)row
                                   bytesPerImage:(NSUInteger)image
{
    NmbTextureDescriptor descriptor;
    if (!textureDescriptor(d, &descriptor))
        return nil;
    NmbTexture *handle = nmb_texture_new_linear(self.handle, &descriptor, offset, row, image);
    if (!handle) {
        NBLOG(@"native-metal: linear texture refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.buffer = self;
    texture.offset = offset;
    texture.rowBytes = row;
    return (id<MTLTexture>)texture;
}

@end

// The raw GM20B path's m2v view of memory it shares with it (drivers/TegraGM20BMetal/raw/runtime/RawMetal.mm): a texture over
// `buffer`'s bytes rendered and sampled in place, as an IOSurface's texture is (m2v's texture over an IOSurface plane), never
// through a shadow image the raw path would not see.
extern "C" id native_bridge_metal_texture_in_place(id buffer, id descriptor, NSUInteger offset, NSUInteger row)
{
    if (![buffer isKindOfClass:[NBBuffer class]] || ![descriptor isKindOfClass:[MTLTextureDescriptor class]])
        return nil;
    return textureOnBuffer((NBBuffer *)buffer, (MTLTextureDescriptor *)descriptor, offset, row, true);
}

extern "C" id native_bridge_metal_texture_tiled_in_place(id buffer, id descriptor, NSUInteger offset)
{
    if (![buffer isKindOfClass:[NBBuffer class]] || ![descriptor isKindOfClass:[MTLTextureDescriptor class]])
        return nil;
    return tiledTextureOnBuffer((NBBuffer *)buffer, (MTLTextureDescriptor *)descriptor, offset);
}

// ---- libraries, functions, states ----
@interface NBLibrary : NBObject
@property (nonatomic) NmbLibrary *handle;
@property (nonatomic, strong)
    id storage;  // keeps the metallib's bytes: its functions' bitcode, which stitching sends (functionBitcode)
@property (nonatomic) const uint8_t *bytes;
@property (nonatomic) size_t length;
@property (nonatomic)
    std::string journalId;  // the journal's lib=: the file's path, or data:<SHA-256 of the bytes> (libraryJournalId)
@end
@interface NBFunction : NBObject
@property (nonatomic) NmbFunction *handle;
@property (nonatomic, copy) NSString *name;  // Metal's: the specialized name it was made under, else libraryName
@property (nonatomic, copy)
    NSString *libraryName;  // the library function it was made from, its AIR's name (name unless set)
@property (nonatomic) MTLFunctionType functionType;
@property (nonatomic)
    std::string constantsText;           // name:type:hex of each constant value it was specialized with (the journal's)
@property (nonatomic) bool specialized;  // made by newFunctionWithName:constantValues:, even with no values
@property (nonatomic, strong) NBLibrary *library;  // the library it came from, whose bytes hold its bitcode
@end
@interface NBArgumentEncoder : NBObject
@property (nonatomic) NmbArgumentEncoder *handle;
@property (nonatomic, strong) id<MTLBuffer> argumentBuffer;  // the buffer its writes go to, kept while it is set
+ (id<MTLArgumentEncoder>)with:(NmbArgumentEncoder *)handle;
@end

// A journal token of `text`: space, '%' and '|' percent-encoded (the journal is split at spaces and " | ").
static std::string journalToken(const char *text)
{
    std::string out;
    for (const char *c = text ? text : ""; *c; c++) {
        char hex[4];
        snprintf(hex, sizeof hex, "%%%02X", (unsigned char)*c);
        out += *c == ' ' || *c == '%' || *c == '|' ? std::string(hex) : std::string(1, *c);
    }
    return out.empty() ? "-" : out;
}

// The journal's name of a library made from `bytes`: data:<their SHA-256>, its bytes kept once as <cache>/libs/<SHA-256>.metallib,
// beside the journal, so the host can make what the journal names again (precompile/harvest.py collects them). Written beside its
// name and renamed; a library already kept is not written again.
#if SWITCH_FRAME_TIMELINE
static std::string libraryJournalId(const void *bytes, size_t length)
{
    if (!journalOn() || !bytes || !length)
        return std::string();
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes, (CC_LONG)length, digest);
    char sha[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(sha + 2 * i, 3, "%02x", digest[i]);
    const std::string dir = gJournalPath.substr(0, gJournalPath.rfind('/')) + "/libs",
                      path = dir + "/" + sha + ".metallib";
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        mkdir(dir.c_str(), 0700);
        const std::string temporary = path + "." + std::to_string(getpid()) + ".tmp";
        if (FILE *f = fopen(temporary.c_str(), "wb")) {
            const bool written = fwrite(bytes, 1, length, f) == length;
            if (fclose(f) == 0 && written)
                rename(temporary.c_str(), path.c_str());
        }
        unlink(temporary.c_str());
    }
    return std::string("data:") + sha;
}

// A function as the journal's S, R and C lines name it: its library function, lib=<its library>, as=<the specialized name it was
// made under>, then its constants; "spec" for one specialized with no constant values (newFunctionWithName:constantValues:, which is
// not newFunctionWithName:).
static std::string stageText(NBFunction *f)
{
    if (!f)
        return "- ";
    std::string text = f.libraryName.UTF8String ?: "?";
    if (f.library && !f.library.journalId.empty())
        text += " lib=" + f.library.journalId;
    if (![f.name isEqualToString:f.libraryName])
        text += " as=" + std::string(f.name.UTF8String ?: "?");
    if (f.specialized && f.constantsText.empty())
        text += " spec";
    return text + " " + f.constantsText;
}
#endif

// The function constants a function declares (MTLFunctionConstant's readable properties), for functionConstantsDictionary.
@interface NBFunctionConstant : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) MTLDataType type;
@property (nonatomic) NSUInteger index;
@property (nonatomic) BOOL required;
@end
@implementation NBFunctionConstant
@end
// A function's stage-input attribute (MTLVertexAttribute's and MTLAttribute's readable properties), for vertexAttributes and
// stageInputAttributes.
@interface NBAttribute : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic) NSUInteger attributeIndex;
@property (nonatomic) MTLDataType attributeType;
@property (nonatomic, getter=isActive) BOOL active;
@property (nonatomic, getter=isPatchData) BOOL patchData;
@property (nonatomic, getter=isPatchControlPointData) BOOL patchControlPointData;
@end
@implementation NBAttribute
@end

static NSArray *functionAttributes(const NmbFunction *f, bool stageInput)
{
    const int64_t count = nmb_function_attribute_count(f, stageInput);
    if (count < 0)
        return nil;
    NSMutableArray *out = [NSMutableArray arrayWithCapacity:NSUInteger(count)];
    for (uint32_t i = 0; i < uint32_t(count); i++) {
        char name[256];
        uint64_t info[5] = {};
        if (!nmb_function_attribute(f, stageInput, i, name, sizeof name, info))
            continue;
        NBAttribute *a = [NBAttribute new];
        a.name = [NSString stringWithUTF8String:name];
        a.attributeIndex = info[0];
        a.attributeType = (MTLDataType)info[1];
        a.active = info[2] != 0;
        a.patchData = info[3] != 0;
        a.patchControlPointData = info[4] != 0;
        [out addObject:a];
    }
    return out;
}

@implementation NBFunction {
    std::once_flag _tessRead;
    switch_metal::FunctionTess _tess;
}

- (void)dealloc
{
    nmb_function_release(_handle);
}

- (NSString *)libraryName
{
    return _libraryName ?: _name;
}

// Its library's function list entry's TESS tag (the library's bytes are the AIR slice Metal reads)
- (switch_metal::FunctionTess)tess
{
    std::call_once(_tessRead, [self] {
        switch_metal::metallibFunctionTess(_library.bytes, _library.length, self.libraryName.UTF8String ?: "", _tess);
    });
    return _tess;
}

- (MTLPatchType)patchType
{
    return MTLPatchType([self tess].patchType);
}

- (NSInteger)patchControlPointCount
{
    return [self tess].controlPoints;
}

// MPS reads its kernels' constants (kMPSDeviceCaps, MPSMaxBatchSize, ...) here before specializing them.
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary
{
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    for (uint32_t i = 0, n = nmb_function_constant_count(_handle); i < n; i++) {
        char name[256];
        uint64_t info[3] = {};
        if (!nmb_function_constant(_handle, i, name, sizeof name, info))
            continue;
        NBFunctionConstant *c = [NBFunctionConstant new];
        c.name = [NSString stringWithUTF8String:name];
        c.type = (MTLDataType)info[0];
        c.index = info[1];
        c.required = info[2] != 0;
        out[c.name] = c;
    }
    return (NSDictionary<NSString *, MTLFunctionConstant *> *)out;
}

- (MTLFunctionOptions)options
{
    return MTLFunctionOptionNone;
}

- (NSArray<MTLVertexAttribute *> *)vertexAttributes
{
    return (NSArray<MTLVertexAttribute *> *)functionAttributes(_handle, false);
}

- (NSArray<MTLAttribute *> *)stageInputAttributes
{
    return (NSArray<MTLAttribute *> *)functionAttributes(_handle, true);
}

// The layout of the argument buffer the function takes at the index, as its AIR lays it out (the runtime's decode). No reflection
// object is made: the reflection form answers nil there, as Metal does when it was not asked for.
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
{
    return [NBArgumentEncoder with:nmb_function_new_argument_encoder(_handle, (uint32_t)index)];
}

- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
                                                 reflection:(MTLAutoreleasedArgument *)reflection
{
    if (reflection)
        *reflection = nil;
    return [self newArgumentEncoderWithBufferIndex:index];
}

@end
// The target's own accessors of MTLFunctionConstantValues (as the offline frontend reads them); their ABI is checked before any call.
@interface MTLFunctionConstantValues (NBOriginalConstantAccess)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
@end
@protocol NBOriginalConstantValue
- (NSUInteger)dataType;
- (void *)data;
- (NSUInteger)index;
- (NSString *)name;
@end

static bool methodABI(id object, SEL selector, const char *encoding)
{
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    return method && !strcmp(method_getTypeEncoding(method), encoding);
}

// Bytes of a function constant of MTLDataType `type` (scalars and 2/4-wide vectors; 3-wide vectors are not qualified).
static uint64_t constantBytes(uint64_t type)
{
    switch (type) {
    case 3:
    case 29:
    case 33:
        return 4;
    case 4:
    case 30:
    case 34:
        return 8;
    case 6:
    case 32:
    case 36:
        return 16;
    case 16:
    case 37:
    case 41:
        return 2;
    case 17:
    case 38:
    case 42:
        return 4;
    case 19:
    case 40:
    case 44:
        return 8;
    case 45:
    case 49:
    case 53:
        return 1;
    case 46:
    case 50:
    case 54:
        return 2;
    case 48:
    case 52:
    case 56:
        return 4;
    case 81:
    case 85:
        return 8;
    case 82:
    case 86:
        return 16;
    case 84:
    case 88:
        return 32;
    default:
        return 0;
    }
}

#if SWITCH_FRAME_TIMELINE
static std::string quartzCoreCallers();
#endif
static bool systemCompiler();
@implementation NBLibrary {
    std::once_flag _externsRead;
    NSArray<NSString *> *_externs;
    std::once_flag _entriesRead;
    NSSet<NSString *> *_entries;
}

- (void)dealloc
{
    nmb_library_release(_handle);
}

// Every entry the runtime finds a function by, extern or not
- (NSSet<NSString *> *)entries
{
    std::call_once(_entriesRead, [&] {
        NSMutableSet<NSString *> *names = [NSMutableSet set];
        for (uint32_t i = 0, n = nmb_library_function_count(_handle); i < n; i++) {
            const size_t length = nmb_library_function_name(_handle, i, nullptr, 0);
            std::vector<char> name(length + 1);
            if (nmb_library_function_name(_handle, i, name.data(), name.size()) != length)
                continue;
            if (NSString *entry = [[NSString alloc] initWithBytes:name.data()
                                                           length:length
                                                         encoding:NSUTF8StringEncoding])
                [names addObject:entry];
        }
        _entries = [names copy];
    });
    return _entries;
}

// Metal's answer to a name the library lacks, before any compile: Core Image asks each of its kernel libraries in turn for a
// kernel and takes the first that makes it (CI::PrecompiledKernels::new_specialzied_function: ci_stdlib, then ci_filters)
static id functionNotFound(NSString *name, NSError **error)
{
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorFunctionNotFound
                                 userInfo:@{
                                     NSLocalizedDescriptionKey :
                                         [NSString stringWithFormat:@"Function %@ was not found in the library", name]
                                 }];
    return nil;
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    return [self newFunctionWithName:name specializedName:nil constantValues:values error:error];
}

// `specialized`: MTLFunctionDescriptor.specializedName, which Metal's function answers as its name and a pipeline links it by
// (RenderBox makes each icon effect custom_fn so)
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                       specializedName:(NSString *)specialized
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    if (name && ![self.entries containsObject:name])
        return functionNotFound(name, error);
    // No values are no constants set (Core Image asks its stitched kernels so), where Core Image's kernels are compiled.
    if (!values && systemCompiler())
        values = [MTLFunctionConstantValues new];
    if (!name || !values || !methodABI(values, @selector(newIndexedConstantArray), "@16@0:8") ||
        !methodABI(values, @selector(newNamedConstantArray), "@16@0:8")) {
        if (error)
            *error = nbError(@"unqualified function constant values");
        return nil;
    }
    MTLFunctionConstantValues *snapshot = [values copy];
    NSArray *indexed = [snapshot newIndexedConstantArray], *named = [snapshot newNamedConstantArray];
    std::vector<NmbConstant> constants;
    for (unsigned group = 0; group < 2; group++)
        for (id value in (group ? named : indexed)) {
            if (!methodABI(value, @selector(dataType), "Q16@0:8") || !methodABI(value, @selector(data), "^v16@0:8") ||
                !methodABI(value, group ? @selector(name) : @selector(index), group ? "@16@0:8" : "Q16@0:8")) {
                if (error)
                    *error = nbError(@"unqualified constant value ABI");
                return nil;
            }
            id<NBOriginalConstantValue> v = value;
            const uint64_t bytes = constantBytes(v.dataType);
            if (!bytes || !v.data) {
                if (error)
                    *error = nbError(
                        [NSString stringWithFormat:@"constant type %lu is not qualified", (unsigned long)v.dataType]);
                return nil;
            }
            constants.push_back({v.dataType, group ? 0 : v.index, group ? v.name.UTF8String : nullptr, v.data, bytes});
        }
#if SWITCH_FRAME_TIMELINE
    std::string text;
    for (const NmbConstant &c : constants) {
        char head[32];
        if (!c.name)
            snprintf(head, sizeof head, "#%llu", (unsigned long long)c.index);
        text += text.empty() ? "" : " ";
        text += c.name ? c.name : head;
        snprintf(head, sizeof head, ":%llu:", (unsigned long long)c.type);
        text += head;
        for (uint64_t i = 0; i < c.length; i++) {
            snprintf(head, sizeof head, "%02x", ((const uint8_t *)c.bytes)[i]);
            text += head;
        }
    }
#endif
    SWITCH_METAL_DIAGNOSTIC(const uint64_t functionBegan = nowUs());
    if (!specialized.length)
        specialized = nil;
    NmbFunction *handle = nmb_function_new_specialized(_handle, name.UTF8String, specialized.UTF8String,
                                                       constants.data(), (uint32_t)constants.size());
    SWITCH_METAL_DIAGNOSTIC(gTimeFunction.add(functionBegan));
    // One S line per specialization (int-155: every QuartzCore pipeline was made on its draw, and a refused generic function would leave its
    // generic pipeline unmade without a word): the function, its constants, the time, the refusal and QuartzCore's callers.
#if SWITCH_FRAME_TIMELINE
    if (journalOn()) {
        char head[96];
        snprintf(head, sizeof head, "S t=%llu us=%llu ok=%d ",
                 (unsigned long long)((functionBegan - gJournalBegan) / 1000),
                 (unsigned long long)(nowUs() - functionBegan), handle ? 1 : 0);
        journal(head + (_journalId.empty() ? std::string() : "lib=" + _journalId + " ") +
                "name=" + std::string(name.UTF8String ?: "?") + " k=" + text +
                (specialized ? " as=" + std::string(specialized.UTF8String) : std::string()) +
                (handle ? std::string() : " err=" + std::string(lastError().UTF8String ?: "?")) + " | " +
                quartzCoreCallers());
    }
#endif
    if (!handle) {
        noteProblem([NSString stringWithFormat:@"specialized function %@: %@", name, lastError()]);
        if (error)
            *error = nbError([NSString stringWithFormat:@"specialized function: %@", lastError()]);
        return nil;
    }
    NBFunction *function = [NBFunction new];
    function.handle = handle;
    function.name = specialized ?: name;
    function.libraryName = name;
    function.functionType = (MTLFunctionType)nmb_function_type(handle);
    SWITCH_METAL_DIAGNOSTIC(function.constantsText = text);
    function.specialized = true;
    function.library = self;
    if (error)
        *error = nil;
    return (id<MTLFunction>)function;
}

- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error = nil;
    id<MTLFunction> function = [self newFunctionWithName:name constantValues:values error:&error];
    handler(function, error);
}

- (MTLLibraryType)type
{
    return MTLLibraryTypeExecutable;
}

// The descriptor's name, specialized name and constants; its options ask for compile-time work the runtime does when the function is
// made.
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d error:(NSError **)error
{
    if (!d.name) {
        if (error)
            *error = nbError(@"a function name is required");
        return nil;
    }
    if (d.constantValues || d.specializedName.length)
        return [self newFunctionWithName:d.name
                         specializedName:d.specializedName
                          constantValues:d.constantValues ?: [MTLFunctionConstantValues new]
                                   error:error];
    if (![self.entries containsObject:d.name])
        return functionNotFound(d.name, error);
    id<MTLFunction> function = [self newFunctionWithName:d.name];
    if (!function) {
        if (error)
            *error = nbError([NSString stringWithFormat:@"function %@: %@", d.name, lastError()]);
        return nil;
    }
    if (error)
        *error = nil;
    return function;
}

// A pipeline library holds an Apple GPU's compiled functions, none for this device (newPipelineLibraryWithFilePath: refuses
// each), so the function is made from this library as without one.
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                       pipelineLibrary:(id)pipelineLibrary
                                 error:(NSError **)error
{
    (void)pipelineLibrary;
    MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
    d.name = name;
    d.constantValues = values;
    return [self newFunctionWithDescriptor:d error:error];
}

- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)d
                completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error = nil;
    id<MTLFunction> function = [self newFunctionWithDescriptor:d error:&error];
    handler(function, error);
}

// m2v lists every entry of the library; Metal leaves its extern functions out (externFunctionNames)
- (NSArray<NSString *> *)functionNames
{
    NSSet<NSString *> *externs = [NSSet setWithArray:[self metallibExterns]];
    NSMutableArray *names = [NSMutableArray array];
    for (uint32_t i = 0, n = nmb_library_function_count(_handle); i < n; i++) {
        const size_t length = nmb_library_function_name(_handle, i, nullptr, 0);
        std::vector<char> name(length + 1);
        if (nmb_library_function_name(_handle, i, name.data(), name.size()) != length)
            return nil;
        NSString *entry = [[NSString alloc] initWithBytes:name.data() length:length encoding:NSUTF8StringEncoding];
        if (!entry)
            return nil;
        if (![externs containsObject:entry])
            [names addObject:entry];
    }
    return names;
}

// Core Image counts its own kernels (ci_stdlib, ci_filters: CI::DAGPrecompiledKernels) from these and finds a kernel's library by
// them; with none it makes every graph of them from source rather than stitching or linking it
// (newLibraryWithImageFilterFunctionsSPI:).
- (NSArray<NSString *> *)externFunctionNames
{
    return [self metallibExterns];
}

// From the library's own bytes. functionNames reads these rather than externFunctionNames, which the QEMU plugin answers from the
// Mac's library over its wire.
- (NSArray<NSString *> *)metallibExterns
{
    std::call_once(_externsRead, [&] {
        NSMutableArray<NSString *> *names = [NSMutableArray array];
        for (const std::string &name : switch_metal::metallibExternNames(_bytes, _length))
            if (NSString *entry = [[NSString alloc] initWithBytes:name.data()
                                                           length:name.size()
                                                         encoding:NSUTF8StringEncoding])
                [names addObject:entry];
        _externs = [names copy];
    });
    return _externs;
}

// As Metal's: the function of any name the library has, extern or not
- (id<MTLFunction>)newExternFunctionWithName:(NSString *)name
{
    return [self newFunctionWithName:name];
}

- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    if (!name || ![self.entries containsObject:name])
        return nil;
    SWITCH_METAL_DIAGNOSTIC(const uint64_t functionBegan = nowUs());
    NmbFunction *handle = nmb_function_new(_handle, name.UTF8String);
    SWITCH_METAL_DIAGNOSTIC(gTimeFunction.add(functionBegan));
    if (!handle)
        return nil;
    NBFunction *function = [NBFunction new];
    function.handle = handle;
    function.name = name;
    function.functionType = (MTLFunctionType)nmb_function_type(handle);
    function.library = self;
    return (id<MTLFunction>)function;
}

@end

// A function's wrapped bitcode in its library's metallib (the entry NAMEd as the function): what a stitching request carries for each
// function, as Metal's own MTLLibraryBuilder sends it. Nil when the library kept no bytes or does not list the function.
static NSData *functionBitcode(NBFunction *function)
{
    NBLibrary *library = function.library;
    const uint8_t *file = library.bytes;
    const size_t length = library.length;
    const std::string wanted = function.libraryName.UTF8String ?: "";
    if (!file || wanted.empty() || length < switch_metal::kMetallibHeaderBytes)
        return nil;
    uint64_t bitcodeAt, bitcodeSize;
    memcpy(&bitcodeAt, file + 72, 8);
    memcpy(&bitcodeSize, file + 80, 8);
    if (bitcodeAt > length || bitcodeSize > length - bitcodeAt)
        return nil;
    NSData *bitcode = nil;
    switch_metal::forEachMetallibEntry(file, length, [&](const switch_metal::MetallibEntry &entry) {
        if (entry.name != wanted)
            return true;
        const uint64_t offset = entry.bitcodeOffset, bytes = entry.bitcodeBytes;
        if (entry.placed && offset <= bitcodeSize && bytes <= bitcodeSize - offset)
            bitcode = [NSData dataWithBytes:file + bitcodeAt + offset length:(NSUInteger)bytes];
        return false;
    });
    return bitcode;
}

// A dispatch's threadgroup size (and, for dispatchThreads, the grid's remainder) picks the VkPipeline it runs: the local size is
// a specialization. The first dispatch of each such shape a state takes is journaled as a D line naming the state as its C line
// does, so the shader seed can make those pipelines ahead of time too.
@interface NBComputePipeline : NBObject
@property (nonatomic) NmbComputePipeline *handle;
@property (nonatomic) std::string journalState;
- (void)journalDispatch:(MTLSize)grid
                  group:(MTLSize)group
                threads:(bool)threads
                 memory:(const std::string &)memory
               textures:(const std::string &)textures
               samplers:(const std::string &)samplers;
@end
@implementation NBComputePipeline {
    std::mutex _dispatchLock;
    std::vector<std::pair<std::array<uint32_t, 7>, std::string>> _dispatched;
}

- (void)dealloc
{
    nmb_compute_pipeline_release(_handle);
}

- (void)journalDispatch:(MTLSize)grid
                  group:(MTLSize)group
                threads:(bool)threads
                 memory:(const std::string &)memory
               textures:(const std::string &)textures
               samplers:(const std::string &)samplers
{
    if (!journalOn() || _journalState.empty())
        return;
    const uint32_t g[3] = {(uint32_t)group.width, (uint32_t)group.height, (uint32_t)group.depth};
    const uint64_t n[3] = {grid.width, grid.height, grid.depth};
    std::array<uint32_t, 7> shape = {g[0], g[1], g[2], 0, 0, 0, threads};
    for (int i = 0; threads && i < 3; i++)
        shape[3 + i] = g[i] ? (uint32_t)(n[i] % g[i]) : 0;
    {
        std::lock_guard<std::mutex> hold(_dispatchLock);
        const auto seen = std::make_pair(shape, memory + "|" + textures + "|" + samplers);
        if (_dispatched.size() >= 16 || std::find(_dispatched.begin(), _dispatched.end(), seen) != _dispatched.end())
            return;
        _dispatched.push_back(seen);
    }
    char head[128];
    snprintf(head, sizeof head, "D ok=1 tg=%u,%u,%u edge=%u,%u,%u threads=%u", shape[0], shape[1], shape[2], shape[3],
             shape[4], shape[5], shape[6]);
    journal(head + (memory.empty() ? std::string() : " tgmem=" + memory) +
            (textures.empty() ? std::string() : " tex=" + textures) +
            (samplers.empty() ? std::string() : " smp=" + samplers) + " | " + _journalState);
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return nmb_compute_pipeline_max_threads(_handle);
}

- (NSUInteger)threadExecutionWidth
{
    return nmb_compute_pipeline_execution_width(_handle);
}

- (NSUInteger)staticThreadgroupMemoryLength
{
    return nmb_compute_pipeline_static_threadgroup_memory(_handle);
}

@end

@interface NBNativeComputeArtifact : NBComputePipeline {
@public
    NmbComputeArtifact *_artifact;
    NmbComputeArtifactInfo _info;
    native_artifact_cache::Key _cacheKey;
    bool _cacheKeyValid;
}
@end
@implementation NBNativeComputeArtifact

- (void)dealloc
{
    nmb_compute_artifact_release(_artifact);
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return _info.max_threads;
}

- (NSUInteger)threadExecutionWidth
{
    return _info.execution_width;
}

- (NSUInteger)staticThreadgroupMemoryLength
{
    return _info.static_memory;
}

@end

static char computeArtifactKey;

static dispatch_queue_t persistQueue()
{
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue =
            dispatch_queue_create("switch.native-analysis.persist",
                                  dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
    return queue;
}

static void saveNativeArtifact(NBNativeComputeArtifact *artifact)
{
    if (gNativeArtifactStorage.cache.empty())
        return;
    if (gNativeArtifactWrites.fetch_add(1, std::memory_order_relaxed) >= 64) {
        gNativeArtifactWrites.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    const std::string directory = gNativeArtifactStorage.cache;
    dispatch_async(persistQueue(), ^{
        @autoreleasepool {
            NativeComputeArtifactStorage::save(artifact->_artifact, artifact->_cacheKey, directory);
        }
        gNativeArtifactWrites.fetch_sub(1, std::memory_order_relaxed);
    });
}

extern "C" bool native_bridge_metal_compute_save(id object, const char *directory, char *name, size_t name_size)
{
    if (![object isKindOfClass:[NBNativeComputeArtifact class]] || !directory || !*directory || !name)
        return false;
    auto *artifact = (NBNativeComputeArtifact *)object;
    if (!artifact->_cacheKeyValid)
        return false;
    const std::string filename = native_artifact_cache::name(artifact->_cacheKey);
    if (name_size <= filename.size() ||
        !NativeComputeArtifactStorage::save(artifact->_artifact, artifact->_cacheKey, directory))
        return false;
    memcpy(name, filename.c_str(), filename.size() + 1);
    return true;
}

extern "C" void native_bridge_metal_journal_dispatch(id state, MTLSize grid, MTLSize group, bool threads,
                                                     const char *memory, const char *textures, const char *samplers)
{
    if (journalOn() && [state isKindOfClass:[NBComputePipeline class]])
        [(NBComputePipeline *)state journalDispatch:grid
                                              group:group
                                            threads:threads
                                             memory:memory
                                           textures:textures
                                           samplers:samplers];
}

@interface NBRenderPipeline : NBObject
@property (nonatomic) NmbRenderPipeline *handle;
@property (nonatomic) std::shared_ptr<nbasync::Making> making;  // being made on a worker (AsyncPipelines.h)
@end
@implementation NBRenderPipeline

// A state made on a worker draws with the handle of the state the synchronous path made there, once made.
- (NmbRenderPipeline *)handle
{
    if (!_making)
        return _handle;
    std::lock_guard<std::mutex> g(_making->lock);
    return _making->state == nbasync::Making::Made ? [(NBRenderPipeline *)_making->result handle] : nullptr;
}

- (void)dealloc
{
    nmb_render_pipeline_release(_handle);
}

// The threadgroup size a tile dispatch of this pipeline could use: the device's limit (no tile shaders are offered).
- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    uint64_t s[3] = {};
    if (!nmb_device_max_threads_per_threadgroup(gDevice, s))
        return 0;
    return s[0];
}

@end
// A worker's own creation takes the synchronous path (AsyncPipelines.h).
static thread_local bool tAsyncWorker;

static bool archivesOurs(NSArray *list);

// Whether newRenderPipelineStateWithDescriptor: makes `d` on a worker (the policy's, AsyncPipelines.h), unless its archives
// must answer at the call (FailOnBinaryArchiveMiss refuses there) or a function or archive is another device's (refused at
// the call). Listed archives alone are read on the worker, where a miss compiles as any state does: a framework's metallib
// (QuartzCore's, CoreUI's) loads as an archive holding nothing for this device.
static bool asyncRenderPipelineWanted(MTLRenderPipelineDescriptor *d, MTLPipelineOption options)
{
    if (tAsyncWorker || !nbasync::policy().on || !d)
        return false;
    if (options & MTLPipelineOptionFailOnBinaryArchiveMiss)
        return false;
    if (d.binaryArchives.count && !archivesOurs(d.binaryArchives))
        return false;
    if (![(id)d.vertexFunction isKindOfClass:[NBFunction class]])
        return false;
    for (MTLLinkedFunctions *l : {d.vertexLinkedFunctions, d.fragmentLinkedFunctions}) {
        for (id f in l.functions)
            if (![f isKindOfClass:[NBFunction class]])
                return false;
        for (id f in l.privateFunctions)
            if (![f isKindOfClass:[NBFunction class]])
                return false;
    }
    return !d.fragmentFunction || [(id)d.fragmentFunction isKindOfClass:[NBFunction class]];
}

// A state of `d` made on a worker by the synchronous path (the call returns at once; AsyncPipelines.h), or at its first
// set when `lazy`.
static id<MTLRenderPipelineState> asyncRenderPipeline(id device, MTLRenderPipelineDescriptor *d,
                                                      MTLPipelineOption options, bool lazy = false)
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    auto making = std::make_shared<nbasync::Making>();
    making->label = std::string(copy.label.UTF8String ?: "") + " " + (copy.vertexFunction.name.UTF8String ?: "-") +
                    "+" + (copy.fragmentFunction.name.UTF8String ?: "-");
    making->work = [device, copy, options](NSString **failure) -> id {
        tAsyncWorker = true;
        NSError *error = nil;
        id state = [(id<MTLDevice>)device newRenderPipelineStateWithDescriptor:copy
                                                                       options:options
                                                                    reflection:nil
                                                                         error:&error];
        tAsyncWorker = false;
        if (!state)
            *failure = error.localizedDescription;
        return state;
    };
    NBRenderPipeline *state = [NBRenderPipeline new];
    state.making = making;
    if (lazy)
        nbasync::startLazy(making);
    else
        nbasync::start(making);
    return (id<MTLRenderPipelineState>)state;
}

extern "C" NSString *native_bridge_metal_function_library_name(id function)
{
    return [function isKindOfClass:[NBFunction class]] ? ((NBFunction *)function).libraryName : nil;
}

extern "C" id native_bridge_metal_function_metallib(id function, const void **bytes, size_t *length)
{
    if (![function isKindOfClass:[NBFunction class]])
        return nil;
    NBLibrary *library = ((NBFunction *)function).library;
    if (!library.bytes || !library.length)
        return nil;
    *bytes = library.bytes;
    *length = library.length;
    return library;
}

extern "C" void *native_bridge_metal_nvk_symbol(const char *name)
{
    return nvkSymbol(name);
}

extern "C" int native_bridge_metal_texture_memory(id texture, struct switch_nvk_image_memory *out)
{
    using Memory = int (*)(uint64_t, struct switch_nvk_image_memory *);
    static const Memory memory = reinterpret_cast<Memory>(nvkSymbol("switch_nvk_image_memory"));
    if (!memory)
        return -1;
    const uint64_t image =
        [texture isKindOfClass:[NBTexture class]] ? nmb_texture_vk_image(((NBTexture *)texture).handle) : 0;
    return image && out ? memory(image, out) : -2;
}

// TegraGM20BMetal: the m2v twin of a state its raw path draws, made at its first set (AsyncPipelines.h startLazy); made
// here when this process makes its states on the asking thread.
extern "C" id native_bridge_metal_render_pipeline_lazy(id device, id descriptor, NSUInteger options)
{
    MTLRenderPipelineDescriptor *d = descriptor;
    if (!asyncRenderPipelineWanted(d, (MTLPipelineOption)options))
        return [(id<MTLDevice>)device newRenderPipelineStateWithDescriptor:d
                                                                   options:(MTLPipelineOption)options
                                                                reflection:nil
                                                                     error:nil];
    return asyncRenderPipeline(device, d, (MTLPipelineOption)options, true);
}

// The raw scheduler subscribes before compatibility replay; no compile occurs on its submission thread.
extern "C" int native_bridge_metal_render_pipeline_ready(id pipeline, void (^observer)(void))
{
    if (![pipeline isKindOfClass:[NBRenderPipeline class]])
        return -1;
    NBRenderPipeline *state = pipeline;
    const auto making = state.making;
    if (!making)
        return state.handle ? 1 : -1;
    std::function<void()> notify;
    if (observer)
        notify = [observer] {
            observer();
        };
    return nbasync::observe(making, std::move(notify));
}

// A committed raw command buffer waits for this state: observed as _ready does, and made on `queue` at once
extern "C" int native_bridge_metal_render_pipeline_urge(id pipeline, void (^observer)(void), dispatch_queue_t queue)
{
    if (![pipeline isKindOfClass:[NBRenderPipeline class]])
        return -1;
    NBRenderPipeline *state = pipeline;
    const auto making = state.making;
    if (!making)
        return state.handle ? 1 : -1;
    std::function<void()> notify;
    if (observer)
        notify = [observer] {
            observer();
        };
    const int status = nbasync::claim(making, std::move(notify));
    if (!status)
        dispatch_async(queue, ^{
            nbasync::urge(making);
        });
    return status;
}

// `call` on a global queue of the caller's class (the completion-handler API, which calls back there; not the workers, whose
// creations would hold it), or here with NMB_ASYNC_PSO_API=0.
static void asyncCall(void (^call)(void))
{
    if (!nbasync::policy().api)
        return call();
    const qos_class_t mine = qos_class_self();
    dispatch_async(dispatch_get_global_queue(mine == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_USER_INITIATED : mine, 0), ^{
        @autoreleasepool {
            tAsyncWorker = true;
            call();
            tAsyncWorker = false;
        }
    });
}

@interface NBSampler : NBObject
@property (nonatomic) NmbSampler *handle;
@property (nonatomic, copy) MTLSamplerDescriptor *descriptor;
@property (nonatomic, strong) NSMutableDictionary<NSValue *, id<MTLSamplerState>> *clamped;
@end
@implementation NBSampler

- (void)dealloc
{
    nmb_sampler_release(_handle);
}

- (NSUInteger)pixelFormat
{
    return MTLPixelFormatInvalid;
}  // SPI: a sampler built from a plain descriptor names no format

@end

// Argument encoders write each argument as the 8 bytes a program could write itself (a buffer's GPU address plus offset, any other
// object's resource ID), straight into the argument buffer's mapping, so encoded and hand-written argument buffers read alike. A setter
// the runtime refuses (Metal's validation errors: a Private buffer, an index of another type) writes nothing and is logged.
@implementation NBArgumentEncoder

+ (id<MTLArgumentEncoder>)with:(NmbArgumentEncoder *)handle
{
    if (!handle) {
        NBLOG(@"native-metal: argument encoder refused: %@", lastError());
        return nil;
    }
    NBArgumentEncoder *encoder = [NBArgumentEncoder new];
    encoder.handle = handle;
    return (id<MTLArgumentEncoder>)encoder;
}

- (void)dealloc
{
    nmb_argument_encoder_release(_handle);
}

- (NSUInteger)encodedLength
{
    return nmb_argument_encoder_length(_handle);
}

- (NSUInteger)alignment
{
    return nmb_argument_encoder_alignment(_handle);
}

- (void)check:(bool)ok what:(const char *)what
{
    if (!ok)
        NBLOG(@"native-metal: argument encoder %s refused: %@", what, lastError());
}

- (void)setArgumentBuffer:(id<MTLBuffer>)b startOffset:(NSUInteger)o arrayElement:(NSUInteger)element
{
    if (b && ![(id)b isKindOfClass:[NBBuffer class]]) {
        NBLOG(@"native-metal: foreign argument buffer");
        return;
    }
    self.argumentBuffer = b;
    [self check:nmb_argument_encoder_set_argument_buffer(_handle, b ? [(NBBuffer *)b handle] : nullptr, o, element)
           what:"setArgumentBuffer"];
}

- (void)setArgumentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o
{
    [self setArgumentBuffer:b startOffset:o arrayElement:0];
}

- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    if (b && ![(id)b isKindOfClass:[NBBuffer class]]) {
        NBLOG(@"native-metal: foreign buffer in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_buffer(_handle, b ? [(NBBuffer *)b handle] : nullptr, o, (uint32_t)i)
           what:"setBuffer"];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setBuffer:b[k] offset:o[k] atIndex:r.location + k];
}

- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    if (t && ![(id)t isKindOfClass:[NBTexture class]]) {
        NBLOG(@"native-metal: foreign texture in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_texture(_handle, t ? [(NBTexture *)t handle] : nullptr, (uint32_t)i)
           what:"setTexture"];
}

- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setTexture:t[k] atIndex:r.location + k];
}

- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    if (s && ![(id)s isKindOfClass:[NBSampler class]]) {
        NBLOG(@"native-metal: foreign sampler in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_sampler(_handle, s ? [(NBSampler *)s handle] : nullptr, (uint32_t)i)
           what:"setSamplerState"];
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setSamplerState:s[k] atIndex:r.location + k];
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)p atIndex:(NSUInteger)i
{
    if (p && ![(id)p isKindOfClass:[NBComputePipeline class]]) {
        NBLOG(@"native-metal: foreign pipeline in an argument buffer");
        return;
    }
    [self check:nmb_argument_encoder_set_compute_pipeline(_handle, p ? [(NBComputePipeline *)p handle] : nullptr,
                                                          (uint32_t)i)
           what:"setComputePipelineState"];
}

- (void *)constantDataAtIndex:(NSUInteger)i
{
    void *at = nmb_argument_encoder_constant_data(_handle, (uint32_t)i);
    if (!at)
        NBLOG(@"native-metal: constantDataAtIndex: %@", lastError());
    return at;
}

- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)i
{
    return [NBArgumentEncoder with:nmb_argument_encoder_new_nested(_handle, (uint32_t)i)];
}

@end
@interface NBDepthStencil : NBObject
@property (nonatomic) NmbDepthStencil *handle;
@end
@implementation NBDepthStencil

- (void)dealloc
{
    nmb_depth_stencil_release(_handle);
}

@end

// ---- events and fences ----
@interface NBEvent : NBObject
@property (nonatomic) NmbEvent *handle;
@end
static void notified(void *context, uint64_t value);
// A plain event answers a shared event's value, set and notifications too, which Metal gives it none of: the raw GM20B
// path (RawMetal.mm) signals and waits for both kinds through them, so a command buffer with a plain event stays raw.
@implementation NBEvent

- (void)dealloc
{
    nmb_event_release(_handle);
}

- (uint64_t)signaledValue
{
    return nmb_shared_event_value(self.handle);
}

- (void)setSignaledValue:(uint64_t)value
{
    nmb_shared_event_set_value(self.handle, value);
}

// The block runs on the listener's dispatch queue (Metal's contract); the bridge's listener thread only forwards it there.
- (void)notifyListener:(MTLSharedEventListener *)listener
               atValue:(uint64_t)value
                 block:(MTLSharedEventNotificationBlock)block
{
    if (!block)
        return;
    dispatch_queue_t queue = listener.dispatchQueue ?: dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
    NSArray *job = @[ self, queue, [block copy] ];
    if (!nmb_shared_event_notify(self.handle, value, notified, (__bridge_retained void *)job)) {
        (void)(__bridge_transfer NSArray *)(__bridge void *)job;
        NBLOG(@"native-metal: notifyListener refused: %@", lastError());
    }
}

@end
@interface NBSharedEvent : NBEvent
@end
@implementation NBSharedEvent

- (BOOL)waitUntilSignaledValue:(uint64_t)value timeoutMS:(uint64_t)milliseconds
{
    return nmb_shared_event_wait(self.handle, value, milliseconds);
}

// A handle for another process needs a kernel-backed event this bridge does not have yet: refused, never a handle that could not work.
- (MTLSharedEventHandle *)newSharedEventHandle
{
    if (gMissing)
        gMissing("NBSharedEvent", "newSharedEventHandle (cross-process)");
    return nil;
}

@end

static void notified(void *context, uint64_t value)
{
    NSArray *job = (__bridge_transfer NSArray *)context;
    id<MTLSharedEvent> event = job[0];
    MTLSharedEventNotificationBlock block = job[2];
    dispatch_async(job[1], ^{
        block(event, value);
    });
}

@interface NBFence : NBObject
@property (nonatomic) NmbFence *handle;
@end
@implementation NBFence

- (void)dealloc
{
    nmb_fence_release(_handle);
}

@end

// ---- command buffers and encoders ----
@interface NBHeap : NBObject
@property (nonatomic) NmbHeap *handle;
@property (nonatomic) MTLResourceOptions options;
@property (nonatomic) MTLHeapType heapType;
@end
@interface NBQueue : NBObject {
@public
    uint32_t gpuPriority, backgroundGpuPriority;  // what it asked for (QueuePriority.h), counted in gQueuePriorities
}
@property (nonatomic) NmbQueue *handle;
@property (atomic, strong)
    dispatch_queue_t completionQueue;  // SPI: where command buffers' handlers run (nil: the runtime's thread)
@end

// The process's queues' GPU priorities; the GPU driver gets the most urgent of each (method 18) whenever it changes. Core
// Animation sets a queue's once and never looks again, so one the driver could not take yet (no NVK device) is sent
// again at the next queue made or command buffer committed (gQueuePriorityPending).
static std::mutex gQueuePriorityLock;
static switch_metal::QueuePriorities gQueuePriorities;
static uint32_t gAppliedPriority = switch_metal::QueuePriorities::kDefault,
                gAppliedBackground = switch_metal::QueuePriorities::kDefaultBackground;
static std::atomic<bool> gQueuePriorityPending{false};

using ApplyPriority = bool (*)(uint32_t, uint32_t);

static ApplyPriority priorityCall()
{
    static const ApplyPriority apply = reinterpret_cast<ApplyPriority>(nvkSymbol("switch_nvk_gpu_priority"));
    return apply;
}

// gQueuePriorityLock held
static void applyQueuePriorities()
{
    const uint32_t priority = gQueuePriorities.priority(), background = gQueuePriorities.backgroundPriority();

    if (priority == gAppliedPriority && background == gAppliedBackground) {
        gQueuePriorityPending.store(false, std::memory_order_relaxed);
        return;
    }
    const bool applied = priorityCall()(priority, background);
    if (applied) {
        gAppliedPriority = priority;
        gAppliedBackground = background;
    }
    gQueuePriorityPending.store(!applied, std::memory_order_relaxed);
}

static void applyPendingQueuePriorities()
{
    if (!gQueuePriorityPending.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(gQueuePriorityLock);
    applyQueuePriorities();
}

static void addQueuePriority(NBQueue *queue)
{
    std::lock_guard<std::mutex> lock(gQueuePriorityLock);
    queue->gpuPriority = switch_metal::QueuePriorities::kDefault;
    queue->backgroundGpuPriority = switch_metal::QueuePriorities::kDefaultBackground;
    gQueuePriorities.add(queue->gpuPriority, queue->backgroundGpuPriority);
    if (priorityCall())
        applyQueuePriorities();
}

// A queue's priority (or its background one): kept, and sent to the driver now or (pending) later. NO only without the
// driver call (not the GM20B's NVK) or for a value Metal does not have.
static bool setQueuePriority(NBQueue *queue, NSUInteger priority, bool background)
{
    if (priority > switch_metal::QueuePriorities::kMax || !priorityCall())
        return false;
    std::lock_guard<std::mutex> lock(gQueuePriorityLock);
    gQueuePriorities.remove(queue->gpuPriority, queue->backgroundGpuPriority);
    if (background)
        queue->backgroundGpuPriority = uint32_t(priority);
    else
        queue->gpuPriority = uint32_t(priority);
    gQueuePriorities.add(queue->gpuPriority, queue->backgroundGpuPriority);
    applyQueuePriorities();
    return true;
}

// A render encoder's attachments as the next one compares them (joinable passes, the rates line): texture, level, slice, depth
// plane, load and store action of each color, the depth and the stencil.
struct PassTarget {
    __unsafe_unretained id texture;
    NSUInteger level, slice, plane;
    MTLLoadAction load;
    MTLStoreAction store;
};

@interface NBCommandBuffer : NBObject {
@public
#if SWITCH_FRAME_TIMELINE
    EncodedWork encodedWork;  // the command buffer's single encoder thread, not global counter deltas
    uint64_t createdUs;
    PassTarget lastPass[10];  // the previous render encoder's, while it is the last encoder opened (lastIsRender)
    bool lastIsRender, lastHadVisibility;
    NSUInteger lastWidth, lastHeight;
#endif
    // The shared event values it waits for (encodeWaitForEvent), its events kept alive in waitedEvents: heldBehindEvent.
    std::vector<std::pair<NmbEvent *, uint64_t>> eventWaits;
#if SWITCH_FRAME_TIMELINE
    // the timeline's fields for an m2v process: ret = scheduled, fence = the runtime's completion, done = the handlers run
    switch_frame::Stamps frame;
#endif
}
@property (nonatomic, strong) NSMutableArray *waitedEvents;
@property (nonatomic) NmbCommandBuffer *handle;
@property (nonatomic, strong) NBQueue *queue;
@property (atomic, copy) NSString *encodeFailure;
@property (atomic) BOOL committed, failedLocally;
#if SWITCH_FRAME_TIMELINE
@property (nonatomic) uint64_t committedUs;
@property (nonatomic) uint32_t commitIndex;
#endif
@property (nonatomic, strong) NSMutableArray *scheduledHandlers, *completedHandlers;
// Runtime handles retain GPU storage, but only the Objective-C owner retains
// a no-copy deallocator, its memory owner and texture/heap ancestry.
@property (nonatomic, strong) NSMutableSet *resourceOwners;
@property (nonatomic, strong) dispatch_group_t handlers;
@property (nonatomic, strong) dispatch_group_t schedulingHandlers;
@property (nonatomic) BOOL schedulingDelivered, completedDelivered;
// SPI: the dictionary MPS and Core Image keep per command buffer (MPS's ~85 call sites), and a process-unique ID for tools.
@property (nonatomic, strong, readonly) NSMutableDictionary *userDictionary;
@property (nonatomic, readonly) uint64_t globalTraceObjectID;
- (void)refuse:(NSString *)what;
- (void)storeStandIn:(NBTwoPlaneStandIn *)standIn;
@end
@interface NBEncoder : NBObject
@property (nonatomic) NmbEncoder *handle;
@property (nonatomic, strong) NBCommandBuffer *commandBuffer;
@end

static void waitForResource(id resource)
{
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    NSArray *pending;
    @synchronized(gInFlight) {
        pending = gInFlight.allObjects;
    }
    for (NBCommandBuffer *commandBuffer in pending) {
        int dependency = -1;
        if ([resource isKindOfClass:[NBBuffer class]])
            dependency = nmb_command_buffer_uses_buffer(commandBuffer.handle, [(NBBuffer *)resource handle]);
        else if ([resource isKindOfClass:[NBTexture class]])
            dependency = nmb_command_buffer_uses_texture(commandBuffer.handle, [(NBTexture *)resource handle]);
        SWITCH_METAL_DIAGNOSTIC(gResourceChecks++);
        // An unknown result retains the old conservative wait. False means no matching storage
        // (or GPU work already completed), not a fabricated completion for a pending dependency.
        if (dependency != 0)
            [(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];
        else
            SWITCH_METAL_DIAGNOSTIC(gResourceSkipped++);
    }
    SWITCH_METAL_DIAGNOSTIC(gTimeResourceWait.add(began));
}

@implementation NBEncoder

NB_RESIDENCY_SETS(use)

- (void)dealloc
{
    if (_handle)
        nmb_encoder_discard(_handle);
}

- (bool)check:(bool)ok what:(const char *)what
{
    // A refused encoder's calls (no handle) fail here too: the command buffer keeps its first reason, so nothing is formatted.
    if (!ok && !_commandBuffer.encodeFailure)
        [_commandBuffer refuse:[NSString stringWithFormat:@"%s: %@", what, lastError()]];
    return ok;
}

- (void)endEncoding
{
    NBApiTime timed;
    NmbEncoder *handle = _handle;
    _handle = nullptr;
    if (!handle) {
        [_commandBuffer refuse:@"endEncoding twice"];
        return;
    }
    [self check:nmb_encoder_end(handle) what:"endEncoding"];
}

- (void)insertDebugSignpost:(NSString *)s
{
    (void)s;
}

- (void)pushDebugGroup:(NSString *)s
{
    (void)s;
}

- (void)popDebugGroup
{}

// Stage 0 compute, 1 vertex, 2 fragment (the bridge's numbering).
- (void)buffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    if (buffer && ![(id)buffer isKindOfClass:[NBBuffer class]]) {
        [_commandBuffer refuse:@"foreign buffer"];
        return;
    }
    if (buffer)
        [_commandBuffer.resourceOwners addObject:buffer];
    [self check:nmb_encoder_set_buffer(_handle, stage, buffer ? [(NBBuffer *)buffer handle] : nullptr, offset,
                                       (uint32_t)index)
           what:"setBuffer"];
}

- (void)texture:(id<MTLTexture>)texture index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    if (texture && ![(id)texture isKindOfClass:[NBTexture class]]) {
        [_commandBuffer refuse:@"foreign texture"];
        return;
    }
    if (texture)
        [_commandBuffer.resourceOwners addObject:texture];
    [self check:nmb_encoder_set_texture(_handle, stage, texture ? [(NBTexture *)texture handle] : nullptr,
                                        (uint32_t)index)
           what:"setTexture"];
}

- (void)sampler:(id<MTLSamplerState>)sampler index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    if (sampler && ![(id)sampler isKindOfClass:[NBSampler class]]) {
        [_commandBuffer refuse:@"foreign sampler"];
        return;
    }
    [self check:nmb_encoder_set_sampler(_handle, stage, sampler ? [(NBSampler *)sampler handle] : nullptr,
                                        (uint32_t)index)
           what:"setSamplerState"];
}

- (void)bytes:(const void *)bytes length:(NSUInteger)length index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    [self check:nmb_encoder_set_bytes(_handle, stage, bytes, length, (uint32_t)index) what:"setBytes"];
}

- (void)fence:(id<MTLFence>)fence stages:(uint64_t)stages update:(bool)update
{
    if (![(id)fence isKindOfClass:[NBFence class]]) {
        [_commandBuffer refuse:@"foreign fence"];
        return;
    }
    NmbFence *handle = [(NBFence *)fence handle];
    [self check:update ? nmb_encoder_update_fence(_handle, handle, stages)
                       : nmb_encoder_wait_fence(_handle, handle, stages)
           what:update ? "updateFence" : "waitForFence"];
}

- (void)updateFence:(id<MTLFence>)fence
{
    [self fence:fence stages:0 update:true];
}

- (void)waitForFence:(id<MTLFence>)fence
{
    [self fence:fence stages:0 update:false];
}

- (void)offset:(NSUInteger)offset index:(NSUInteger)index stage:(uint32_t)stage
{
    NBApiTime timed;
    [self check:nmb_encoder_set_buffer_offset(_handle, stage, offset, (uint32_t)index) what:"setBufferOffset"];
}

// Resources reached through memory (argument buffers, GPU addresses): split into the bridge's buffers and textures, each kept alive by the
// command buffer like a bound one; false, with the command buffer refused, for a foreign object.
- (bool)resources:(const id __unsafe_unretained[])list
            count:(NSUInteger)count
          buffers:(std::vector<NmbBuffer *> &)buffers
         textures:(std::vector<NmbTexture *> &)textures
{
    for (NSUInteger i = 0; i < count; i++) {
        id r = list[i];
        if (!r)
            continue;
        if ([r isKindOfClass:[NBBuffer class]])
            buffers.push_back([(NBBuffer *)r handle]);
        else if ([r isKindOfClass:[NBTexture class]])
            textures.push_back([(NBTexture *)r handle]);
        else {
            [_commandBuffer
                refuse:[NSString stringWithFormat:@"foreign resource %s", class_getName(object_getClass(r))]];
            return false;
        }
        [_commandBuffer.resourceOwners addObject:r];
    }
    return true;
}

- (void)use:(const id __unsafe_unretained[])list
      count:(NSUInteger)count
      usage:(MTLResourceUsage)usage
     stages:(uint64_t)stages
{
    NBApiTime timed;
    std::vector<NmbBuffer *> buffers;
    std::vector<NmbTexture *> textures;
    if (![self resources:list count:count buffers:buffers textures:textures])
        return;
    [self check:nmb_encoder_use_resources(_handle, buffers.data(), buffers.size(), textures.data(), textures.size(),
                                          usage, stages)
           what:"useResources"];
}

- (void)heaps:(const id __unsafe_unretained[])list count:(NSUInteger)count stages:(uint64_t)stages
{
    std::vector<NmbHeap *> heaps;
    for (NSUInteger i = 0; i < count; i++) {
        if (![list[i] isKindOfClass:[NBHeap class]]) {
            [_commandBuffer refuse:@"missing or foreign heap"];
            return;
        }
        heaps.push_back([(NBHeap *)list[i] handle]);
        [_commandBuffer.resourceOwners addObject:list[i]];
    }
    [self check:nmb_encoder_use_heaps(_handle, heaps.data(), heaps.size(), stages) what:"useHeaps"];
}

@end

@interface NBComputeEncoder : NBEncoder
@property (nonatomic) MTLDispatchType kind;
@property (nonatomic, strong) NBComputePipeline *state;
@end
@implementation NBComputeEncoder {
    // setThreadgroupMemoryLength:'s lengths by index, rounded up to 16 as Metal takes them: a dispatch's D line names
    // them (m2v compiles each into the kernel's program)
    std::map<uint32_t, uint32_t> _threadgroupMemory;
    // the textures bound, by index: m2v's program and push words follow their formats (texel_format), so a D line names them
    NSMutableDictionary<NSNumber *, id<MTLTexture>> *_textures;
    NSMutableDictionary<NSNumber *, id<MTLSamplerState>> *_samplers;  // and the samplers they are read through
}

// "<index>:<min>:<mag>:<mip>:<s>:<t>:<r>:<lodMin bits>:<lodMax bits>:<anisotropy>:<compare>:<border>:<normalized>:<average>,..."
// of the samplers bound, the floats' bits in hex (raw/runtime/raw_compute_key.h computeSamplers)
- (std::string)samplersText
{
    std::string text;
    for (NSNumber *index in [_samplers.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        id s = _samplers[index];
        if (![s isKindOfClass:[NBSampler class]])
            continue;
        MTLSamplerDescriptor *d = ((NBSampler *)s).descriptor;
        const float lo = d.lodMinClamp, hi = d.lodMaxClamp;
        uint32_t loBits, hiBits;
        memcpy(&loBits, &lo, 4);
        memcpy(&hiBits, &hi, 4);
        char one[192];
        snprintf(one, sizeof one, "%s%u:%lu:%lu:%lu:%lu:%lu:%lu:%x:%x:%lu:%lu:%lu:%d:%d", text.empty() ? "" : ",",
                 index.unsignedIntValue, (unsigned long)d.minFilter, (unsigned long)d.magFilter,
                 (unsigned long)d.mipFilter, (unsigned long)d.sAddressMode, (unsigned long)d.tAddressMode,
                 (unsigned long)d.rAddressMode, loBits, hiBits, (unsigned long)d.maxAnisotropy,
                 (unsigned long)d.compareFunction, (unsigned long)d.borderColor, d.normalizedCoordinates ? 1 : 0,
                 d.lodAverage ? 1 : 0);
        text += one;
    }
    return text;
}

// "<index>:<pixelFormat>:<textureType>:<mipmapLevelCount>:<arrayLength>:<swizzle, 4 hex bytes>,..." of the
// textures bound (the raw pack's key, raw/runtime/raw_compute_key.h computeTextures)
- (std::string)texturesText
{
    std::string text;
    for (NSNumber *index in [_textures.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        id<MTLTexture> t = _textures[index];
        if ([(id)t isKindOfClass:[NSNull class]])
            continue;
        const MTLTextureSwizzleChannels w = t.swizzle;
        char one[96];
        snprintf(one, sizeof one, "%s%u:%lu:%lu:%lu:%lu:%02x%02x%02x%02x", text.empty() ? "" : ",",
                 index.unsignedIntValue, (unsigned long)t.pixelFormat, (unsigned long)t.textureType,
                 (unsigned long)t.mipmapLevelCount, (unsigned long)t.arrayLength, w.red, w.green, w.blue, w.alpha);
        text += one;
    }
    return text;
}

// "<index>:<length>,..." of the lengths set (the raw pack's key, raw/runtime/raw_compute_key.h)
- (std::string)threadgroupMemoryText
{
    std::string text;
    for (const auto &[index, length] : _threadgroupMemory)
        if (length)
            text += (text.empty() ? "" : ",") + std::to_string(index) + ":" + std::to_string(length);
    return text;
}

- (MTLDispatchType)dispatchType
{
    return _kind;
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
    NBApiTime timed;
    if (![(id)state isKindOfClass:[NBComputePipeline class]]) {
        [self.commandBuffer refuse:@"foreign compute pipeline"];
        return;
    }
    [self check:nmb_compute_set_pipeline(self.handle, [(NBComputePipeline *)state handle])
           what:"setComputePipelineState"];
    self.state = (NBComputePipeline *)state;
}

- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self buffer:b offset:o index:i stage:0];
}

- (void)setBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self offset:o index:i stage:0];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self buffer:b[k] offset:o[k] index:r.location + k stage:0];
}

- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self bytes:p length:n index:i stage:0];
}

- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    [self texture:t index:i stage:0];
    if (journalOn()) {
        if (!_textures)
            _textures = [NSMutableDictionary dictionary];
        _textures[@(i)] = t ?: (id<MTLTexture>)[NSNull null];
    }
}

- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setTexture:t[k] atIndex:r.location + k];
}

- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    [self sampler:s index:i stage:0];
    if (journalOn()) {
        if (!_samplers)
            _samplers = [NSMutableDictionary dictionary];
        _samplers[@(i)] = s ?: (id<MTLSamplerState>)[NSNull null];
    }
}

- (void)setThreadgroupMemoryLength:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self check:nmb_compute_set_threadgroup_memory(self.handle, (uint32_t)n, (uint32_t)i)
           what:"setThreadgroupMemoryLength"];
    if (journalOn())
        _threadgroupMemory[(uint32_t)i] = (uint32_t)((n + 15) & ~NSUInteger(15));
}

- (void)dispatch:(MTLSize)size group:(MTLSize)group threads:(bool)threads
{
    NBApiTime timed;
    SWITCH_METAL_DIAGNOSTIC(gDispatches++);
    SWITCH_METAL_DIAGNOSTIC(self.commandBuffer->encodedWork.dispatches++);
    const uint64_t s[3] = {size.width, size.height, size.depth}, g[3] = {group.width, group.height, group.depth};
    [self check:nmb_compute_dispatch(self.handle, s, g, threads) what:"dispatch"];
    if (journalOn())
        [self.state journalDispatch:size
                              group:group
                            threads:threads
                             memory:[self threadgroupMemoryText]
                           textures:[self texturesText]
                           samplers:[self samplersText]];
}

- (void)dispatchThreads:(MTLSize)t threadsPerThreadgroup:(MTLSize)g
{
    [self dispatch:t group:g threads:true];
}

- (void)dispatchThreadgroups:(MTLSize)t threadsPerThreadgroup:(MTLSize)g
{
    [self dispatch:t group:g threads:false];
}

// The arguments are read when the dispatch runs (after the work before it completed): MPS's and MPSGraph's sizes computed by a kernel.
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b
                          indirectBufferOffset:(NSUInteger)o
                         threadsPerThreadgroup:(MTLSize)g
{
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign indirect buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    SWITCH_METAL_DIAGNOSTIC(gDispatches++);
    SWITCH_METAL_DIAGNOSTIC(self.commandBuffer->encodedWork.dispatches++);
    const uint64_t per[3] = {g.width, g.height, g.depth};
    [self check:nmb_compute_dispatch_indirect(self.handle, [(NBBuffer *)b handle], o, per)
           what:"dispatchThreadgroupsWithIndirectBuffer"];
    if (journalOn())
        [self.state journalDispatch:MTLSizeMake(0, 0, 0)
                              group:g
                            threads:false
                             memory:[self threadgroupMemoryText]
                           textures:[self texturesText]
                           samplers:[self samplersText]];
}

// SPI (MTLDispatchThreadsIndirectArguments: the grid, then the threadgroup).
- (void)dispatchThreadsWithIndirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o
{
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign indirect buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    SWITCH_METAL_DIAGNOSTIC(gDispatches++);
    SWITCH_METAL_DIAGNOSTIC(self.commandBuffer->encodedWork.dispatches++);
    [self check:nmb_compute_dispatch_indirect(self.handle, [(NBBuffer *)b handle], o, nullptr)
           what:"dispatchThreadsWithIndirectBuffer"];
}

// What a dispatch reaches through an argument buffer or a GPU address must be made resident, or its reads miss (read 0) in the runtime.
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u
{
    const id __unsafe_unretained list[1] = {r};
    [self use:list count:1 usage:u stages:0];
}

- (void)useResources:(const id<MTLResource> __unsafe_unretained[])r count:(NSUInteger)n usage:(MTLResourceUsage)u
{
    [self use:r count:n usage:u stages:0];
}

- (void)useHeap:(id<MTLHeap>)h
{
    const id __unsafe_unretained list[1] = {h};
    [self heaps:list count:1 stages:0];
}

- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained[])h count:(NSUInteger)n
{
    [self heaps:h count:n stages:0];
}

// A serial encoder orders its dispatches anyway; a concurrent one (MPSGraph's) depends on these.
- (void)memoryBarrierWithScope:(MTLBarrierScope)s
{
    [self check:nmb_compute_memory_barrier(self.handle, s) what:"memoryBarrierWithScope"];
}

- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained[])r count:(NSUInteger)n
{
    std::vector<NmbBuffer *> buffers;
    std::vector<NmbTexture *> textures;
    if (![self resources:r count:n buffers:buffers textures:textures])
        return;
    [self check:nmb_compute_memory_barrier_resources(self.handle, buffers.data(), buffers.size(), textures.data(),
                                                     textures.size())
           what:"memoryBarrierWithResources"];
}

- (void)setStageInRegion:(MTLRegion)r
{
    const uint64_t region[6] = {r.origin.x, r.origin.y, r.origin.z, r.size.width, r.size.height, r.size.depth};
    [self check:nmb_compute_set_stage_in_region(self.handle, region, nullptr, 0) what:"setStageInRegion"];
}

- (void)setStageInRegionWithIndirectBuffer:(id<MTLBuffer>)b indirectBufferOffset:(NSUInteger)o
{
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign stage-in buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    [self check:nmb_compute_set_stage_in_region(self.handle, nullptr, [(NBBuffer *)b handle], o)
           what:"setStageInRegionWithIndirectBuffer"];
}

// Imageblocks are an Apple4 feature this device does not claim; the runtime keeps Metal's sizing rules for a kernel that has one.
- (void)setImageblockWidth:(NSUInteger)w height:(NSUInteger)h
{
    [self check:nmb_compute_set_imageblock_size(self.handle, w, h) what:"setImageblockWidth"];
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setSamplerState:s[k] atIndex:r.location + k];
}

- (void)setSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i
{
    if (s && ![(id)s isKindOfClass:[NBSampler class]]) {
        [self.commandBuffer refuse:@"foreign sampler"];
        return;
    }
    [self check:nmb_compute_set_sampler_lod(self.handle, s ? [(NBSampler *)s handle] : nullptr, lo, hi, (uint32_t)i)
           what:"setSamplerState:lodMinClamp:lodMaxClamp:"];
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
            lodMinClamps:(const float[])lo
            lodMaxClamps:(const float[])hi
               withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self setSamplerState:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] atIndex:r.location + k];
}

@end

@interface NBRenderEncoder : NBEncoder {
    MTLCullMode _cull;
    MTLWinding _winding;
    MTLTriangleFillMode _fill;
}
@property (nonatomic) bool skipDraws;  // the state set last is being made (or failed): its draws are skipped
@property (nonatomic, strong) NSArray<NBTwoPlaneStandIn *> *storeBacks;  // stored into their planes at endEncoding
@end
@implementation NBRenderEncoder

// The pass's two-plane attachments go back into their planes once it ends (rendersThroughStandIn).
- (void)endEncoding
{
    [super endEncoding];
    NSArray<NBTwoPlaneStandIn *> *stores = _storeBacks;
    _storeBacks = nil;
    for (NBTwoPlaneStandIn *standIn in stores)
        [self.commandBuffer storeStandIn:standIn];
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state
{
    NBApiTime timed;
    if (![(id)state isKindOfClass:[NBRenderPipeline class]]) {
        [self.commandBuffer refuse:@"foreign render pipeline"];
        return;
    }
    // Native queued work reaches this point only once ready. Direct bridge clients wait for a complete state.
    _skipDraws = false;
    if (const std::shared_ptr<nbasync::Making> making = ((NBRenderPipeline *)state).making;
        making && !nbasync::ready(making)) {
        _skipDraws = true;
        [self.commandBuffer refuse:@"render pipeline compilation failed or timed out"];
        return;
    }
    [self check:nmb_render_set_pipeline(self.handle, [(NBRenderPipeline *)state handle]) what:"setRenderPipelineState"];
}

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self buffer:b offset:o index:i stage:1];
}

- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self buffer:b offset:o index:i stage:2];
}

- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self offset:o index:i stage:1];
}

- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i
{
    [self offset:o index:i stage:2];
}

- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self buffer:b[k] offset:o[k] index:r.location + k stage:1];
}

- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained[])b
                   offsets:(const NSUInteger[])o
                 withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self buffer:b[k] offset:o[k] index:r.location + k stage:2];
}

- (void)setVertexBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self bytes:p length:n index:i stage:1];
}

- (void)setFragmentBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    [self bytes:p length:n index:i stage:2];
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    [self texture:t index:i stage:1];
}

- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    [self texture:t index:i stage:2];
}

- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained[])t withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self texture:t[k] index:r.location + k stage:2];
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    [self sampler:s index:i stage:1];
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
    [self sampler:s index:i stage:2];
}

// A bind's LOD clamps replace the state's (Metal's rule, metal2vulkan's for compute): the state of its descriptor with those clamps,
// made once per state and clamps
static id<MTLSamplerState> clampedSampler(id<MTLSamplerState> s, float lodMin, float lodMax)
{
    if (![(id)s isKindOfClass:[NBSampler class]])
        return s;
    NBSampler *base = (NBSampler *)s;
    const float clamps[2] = {lodMin, lodMax};
    NSValue *key = [NSValue valueWithBytes:clamps objCType:@encode(float[2])];
    @synchronized(base) {
        if (!base.clamped)
            base.clamped = [NSMutableDictionary dictionary];
        if (id<MTLSamplerState> made = base.clamped[key])
            return made;
        MTLSamplerDescriptor *d = [base.descriptor copy];
        d.lodMinClamp = lodMin;
        d.lodMaxClamp = lodMax;
        id<MTLSamplerState> made = [(id<MTLDevice>)gDeviceObject newSamplerStateWithDescriptor:d];
        if (made)
            base.clamped[key] = made;
        return made;
    }
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s
                  lodMinClamp:(float)lodMin
                  lodMaxClamp:(float)lodMax
                      atIndex:(NSUInteger)i
{
    [self sampler:clampedSampler(s, lodMin, lodMax) index:i stage:1];
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s
                    lodMinClamp:(float)lodMin
                    lodMaxClamp:(float)lodMax
                        atIndex:(NSUInteger)i
{
    [self sampler:clampedSampler(s, lodMin, lodMax) index:i stage:2];
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])s
                    lodMinClamps:(const float[])lodMin
                    lodMaxClamps:(const float[])lodMax
                       withRange:(NSRange)r
{
    for (NSUInteger k = 0; k < r.length; k++)
        [self sampler:clampedSampler(s[k], lodMin[k], lodMax[k]) index:r.location + k stage:2];
}

- (void)setDepthStencilState:(id<MTLDepthStencilState>)state
{
    NBApiTime timed;
    if (state && ![(id)state isKindOfClass:[NBDepthStencil class]]) {
        [self.commandBuffer refuse:@"foreign depth/stencil state"];
        return;
    }
    [self check:nmb_render_set_depth_stencil(self.handle, state ? [(NBDepthStencil *)state handle] : nullptr)
           what:"setDepthStencilState"];
}

- (void)setViewport:(MTLViewport)v
{
    NBApiTime timed;
    const double values[6] = {v.originX, v.originY, v.width, v.height, v.znear, v.zfar};
    [self check:nmb_render_set_viewport(self.handle, values) what:"setViewport"];
}

- (void)setScissorRect:(MTLScissorRect)r
{
    NBApiTime timed;
    const uint64_t values[4] = {r.x, r.y, r.width, r.height};
    [self check:nmb_render_set_scissor(self.handle, values) what:"setScissorRect"];
}

// The runtime takes cull mode, winding and fill mode together; this encoder keeps Metal's defaults (zero) until each is set.
- (void)raster
{
    NBApiTime timed;
    [self check:nmb_render_set_raster(self.handle, _cull, _winding, _fill) what:"raster state"];
}

- (void)setCullMode:(MTLCullMode)m
{
    _cull = m;
    [self raster];
}

- (void)setFrontFacingWinding:(MTLWinding)w
{
    _winding = w;
    [self raster];
}

- (void)setTriangleFillMode:(MTLTriangleFillMode)f
{
    _fill = f;
    [self raster];
}

- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp
{
    [self check:nmb_render_set_depth_bias(self.handle, bias, slope, clamp) what:"setDepthBias"];
}

// Metal's default is Clip until set. Clamp needs the device's depthClamp: refused there, the draws keep clipping (one line), the
// closest picture the device draws, rather than a failed command buffer.
- (void)setDepthClipMode:(MTLDepthClipMode)mode
{
    NBApiTime timed;
    if (!self.handle || nmb_render_set_depth_clip_mode(self.handle, mode))
        return;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        NBLOG(@"native-metal: setDepthClipMode:%lu refused, draws keep the mode before: %@", (unsigned long)mode,
              lastError());
}

- (void)setLineWidth:(float)width
{
    [self check:nmb_render_set_line_width(self.handle, width) what:"setLineWidth"];
}

- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset
{
    [self check:nmb_render_set_visibility_mode(self.handle, mode, offset) what:"setVisibilityResultMode"];
}

- (void)setFragmentTexture:(id<MTLTexture>)t
            atTextureIndex:(NSUInteger)ti
              samplerState:(id<MTLSamplerState>)s
            atSamplerIndex:(NSUInteger)si
{
    [self texture:t index:ti stage:2];
    [self sampler:s index:si stage:2];
}

// Tile shaders and imageblock tile memory (Apple GPU families 4 and up) are not offered: supportsTileShaders answers no, and a call that
// reaches one anyway fails its command buffer.
- (NSUInteger)tileWidth
{
    return 0;
}

- (NSUInteger)tileHeight
{
    return 0;
}

- (void)setTileTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
    (void)t;
    (void)i;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}

- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i
{
    (void)p;
    (void)n;
    (void)i;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}

- (void)dispatchThreadsPerTile:(MTLSize)size
{
    (void)size;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}

- (void)dispatchThreadsPerTile:(MTLSize)size inRegion:(MTLRegion)region
{
    (void)size;
    (void)region;
    [self.commandBuffer refuse:@"tile shaders are not supported"];
}

- (void)setStencilReferenceValue:(uint32_t)v
{
    [self check:nmb_render_set_stencil_reference(self.handle, v, v) what:"setStencilReferenceValue"];
}

- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b
{
    [self check:nmb_render_set_stencil_reference(self.handle, f, b) what:"setStencilReferenceValues"];
}

- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
    const float c[4] = {r, g, b, a};
    [self check:nmb_render_set_blend_color(self.handle, c) what:"setBlendColor"];
}

- (void)drawPrimitives:(MTLPrimitiveType)t
           vertexStart:(NSUInteger)s
           vertexCount:(NSUInteger)n
         instanceCount:(NSUInteger)i
          baseInstance:(NSUInteger)b
{
    NBApiTime timed;
    if (_skipDraws) {  // its state is still being made
        SWITCH_METAL_DIAGNOSTIC(nbasync::counters().skippedDraws++);
        return;
    }
    SWITCH_METAL_DIAGNOSTIC(gDraws++);
    SWITCH_METAL_DIAGNOSTIC(self.commandBuffer->encodedWork.draws++);
    [self check:nmb_render_draw(self.handle, t, (uint32_t)s, (uint32_t)n, (uint32_t)i, (uint32_t)b)
           what:"drawPrimitives"];
}

- (void)drawPrimitives:(MTLPrimitiveType)t
           vertexStart:(NSUInteger)s
           vertexCount:(NSUInteger)n
         instanceCount:(NSUInteger)i
{
    [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:i baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
{
    [self drawPrimitives:t vertexStart:s vertexCount:n instanceCount:1 baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)t
                   indexCount:(NSUInteger)n
                    indexType:(MTLIndexType)k
                  indexBuffer:(id<MTLBuffer>)b
            indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i
                   baseVertex:(NSInteger)v
                 baseInstance:(NSUInteger)bi
{
    NBApiTime timed;
    if (_skipDraws) {  // its state is still being made
        SWITCH_METAL_DIAGNOSTIC(nbasync::counters().skippedDraws++);
        return;
    }
    if (![(id)b isKindOfClass:[NBBuffer class]]) {
        [self.commandBuffer refuse:@"foreign index buffer"];
        return;
    }
    [self.commandBuffer.resourceOwners addObject:b];
    SWITCH_METAL_DIAGNOSTIC(gDraws++);
    SWITCH_METAL_DIAGNOSTIC(self.commandBuffer->encodedWork.draws++);
    [self check:nmb_render_draw_indexed(self.handle, t, (uint32_t)n, k, [(NBBuffer *)b handle], o, (uint32_t)i,
                                        (int32_t)v, (uint32_t)bi)
           what:"drawIndexedPrimitives"];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)t
                   indexCount:(NSUInteger)n
                    indexType:(MTLIndexType)k
                  indexBuffer:(id<MTLBuffer>)b
            indexBufferOffset:(NSUInteger)o
                instanceCount:(NSUInteger)i
{
    [self drawIndexedPrimitives:t
                     indexCount:n
                      indexType:k
                    indexBuffer:b
              indexBufferOffset:o
                  instanceCount:i
                     baseVertex:0
                   baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)t
                   indexCount:(NSUInteger)n
                    indexType:(MTLIndexType)k
                  indexBuffer:(id<MTLBuffer>)b
            indexBufferOffset:(NSUInteger)o
{
    [self drawIndexedPrimitives:t
                     indexCount:n
                      indexType:k
                    indexBuffer:b
              indexBufferOffset:o
                  instanceCount:1
                     baseVertex:0
                   baseInstance:0];
}

- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)s
{
    const id __unsafe_unretained list[1] = {r};
    [self use:list count:1 usage:u stages:s];
}

- (void)useResources:(const id<MTLResource> __unsafe_unretained[])r
               count:(NSUInteger)n
               usage:(MTLResourceUsage)u
              stages:(MTLRenderStages)s
{
    [self use:r count:n usage:u stages:s];
}

- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u
{
    [self useResource:r usage:u stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

- (void)useHeap:(id<MTLHeap>)h stages:(MTLRenderStages)s
{
    const id __unsafe_unretained list[1] = {h};
    [self heaps:list count:1 stages:s];
}

- (void)useHeap:(id<MTLHeap>)h
{
    [self useHeap:h stages:MTLRenderStageVertex | MTLRenderStageFragment];
}

- (void)updateFence:(id<MTLFence>)fence afterStages:(MTLRenderStages)stages
{
    [self fence:fence stages:stages update:true];
}

- (void)waitForFence:(id<MTLFence>)fence beforeStages:(MTLRenderStages)stages
{
    [self fence:fence stages:stages update:false];
}

@end

@interface NBBlitEncoder : NBEncoder
@end
@implementation NBBlitEncoder
// The arguments are passed as a plain array: an NSArray literal raises on a nil element, and a nil resource (a refused allocation) must fail the
// command buffer like any invalid argument, not abort the host process.
#define NB_OBJECTS(...) ((__unsafe_unretained id[]){__VA_ARGS__})

- (bool)buffers:(__unsafe_unretained id *)objects count:(size_t)count
{
    for (size_t i = 0; i < count; i++)
        if (![objects[i] isKindOfClass:[NBBuffer class]]) {
            [self.commandBuffer refuse:@"missing or foreign blit buffer"];
            return false;
        }
    for (size_t i = 0; i < count; i++)
        [self.commandBuffer.resourceOwners addObject:objects[i]];
    return true;
}

- (bool)textures:(__unsafe_unretained id *)objects count:(size_t)count
{
    for (size_t i = 0; i < count; i++) {
        id o = objects[i];
        if (![o isKindOfClass:[NBTexture class]]) {
            [self.commandBuffer refuse:@"missing or foreign blit texture"];
            return false;
        }
        [self.commandBuffer.resourceOwners addObject:o];
    }
    return true;
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
          sourceOffset:(NSUInteger)so
              toBuffer:(id<MTLBuffer>)d
     destinationOffset:(NSUInteger)dO
                  size:(NSUInteger)n
{
    NBApiTime timed;
    if (![self buffers:NB_OBJECTS(s, d) count:2])
        return;
    [self check:nmb_blit_copy_buffer(self.handle, [(NBBuffer *)s handle], so, [(NBBuffer *)d handle], dO, n)
           what:"copyFromBuffer:toBuffer:"];
}

- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v
{
    if (![self buffers:NB_OBJECTS(b) count:1])
        return;
    [self check:nmb_blit_fill_buffer(self.handle, [(NBBuffer *)b handle], r.location, r.location + r.length, v)
           what:"fillBuffer"];
}

- (void)copyFromTexture:(id<MTLTexture>)s
            sourceSlice:(NSUInteger)ss
            sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so
             sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)ds
       destinationLevel:(NSUInteger)dl
      destinationOrigin:(MTLOrigin)dO
{
    NBApiTime timed;
    if (![self textures:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])
        return;
    if ([(NBTexture *)s rgWide] != [(NBTexture *)d rgWide]) {  // one is an RG8 texture held as RGBA8 (heldAsRGBA8)
        [self.commandBuffer refuse:@"copyFromTexture:toTexture: between an RG8 texture held as RGBA8 and another"];
        return;
    }
    textureGpuWrites((NBTexture *)d);
    const uint64_t a[3] = {so.x, so.y, so.z}, size[3] = {z.width, z.height, z.depth}, b[3] = {dO.x, dO.y, dO.z};
    [self check:nmb_blit_copy_texture(self.handle, [(NBTexture *)s handle], ss, sl, a, size, [(NBTexture *)d handle],
                                      ds, dl, b)
           what:"copyFromTexture:toTexture:"];
}

- (void)copyFromTexture:(id<MTLTexture>)s toTexture:(id<MTLTexture>)d
{
    if (![self textures:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])
        return;
    if ([(NBTexture *)s rgWide] != [(NBTexture *)d rgWide]) {  // one is an RG8 texture held as RGBA8 (heldAsRGBA8)
        [self.commandBuffer refuse:@"copyFromTexture:toTexture: between an RG8 texture held as RGBA8 and another"];
        return;
    }
    const NSUInteger levels = MIN(s.mipmapLevelCount, d.mipmapLevelCount), slices = MIN(s.arrayLength, d.arrayLength);
    for (NSUInteger slice = 0; slice < slices; slice++)
        for (NSUInteger level = 0; level < levels; level++) {
            MTLSize size = MTLSizeMake(MAX(s.width >> level, (NSUInteger)1), MAX(s.height >> level, (NSUInteger)1),
                                       MAX(s.depth >> level, (NSUInteger)1));
            [self copyFromTexture:s
                      sourceSlice:slice
                      sourceLevel:level
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:size
                        toTexture:d
                 destinationSlice:slice
                 destinationLevel:level
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
}

- (void)copyFromBuffer:(id<MTLBuffer>)s
           sourceOffset:(NSUInteger)o
      sourceBytesPerRow:(NSUInteger)row
    sourceBytesPerImage:(NSUInteger)image
             sourceSize:(MTLSize)z
              toTexture:(id<MTLTexture>)d
       destinationSlice:(NSUInteger)slice
       destinationLevel:(NSUInteger)level
      destinationOrigin:(MTLOrigin)dO
{
    NBApiTime timed;
    if (![self buffers:NB_OBJECTS(s) count:1] || ![self textures:NB_OBJECTS(d) count:1])
        return;
    textureGpuWrites((NBTexture *)d);
    const uint64_t size[3] = {z.width, z.height, z.depth}, b[3] = {dO.x, dO.y, dO.z};
    if ([(NBTexture *)d rgWide]) {
        // An RG8 texture held as RGBA8 (heldAsRGBA8): the buffer's RG pairs widened into a staging buffer now, which the copy
        // moves. A buffer the CPU cannot read (Private) is refused.
        const uint8_t *from = (const uint8_t *)[(id<MTLBuffer>)s contents];
        if (!from) {
            [self.commandBuffer
                refuse:@"copyFromBuffer:toTexture: a Private buffer into an RG8Unorm_sRGB texture held as RGBA8"];
            return;
        }
        const size_t w = z.width, h = z.height, depth = std::max<size_t>(z.depth, 1);
        id<MTLBuffer> staging = [(id<MTLDevice>)gDeviceObject newBufferWithLength:std::max<size_t>(w * h * depth * 4, 4)
                                                                          options:MTLResourceStorageModeShared];
        if (!staging) {
            [self.commandBuffer
                refuse:@"copyFromBuffer:toTexture: no staging buffer for an RG8Unorm_sRGB texture held as RGBA8"];
            return;
        }
        widenRG(from + o, row, image, (uint8_t *)staging.contents, w, h, depth);
        [self.commandBuffer.resourceOwners addObject:staging];
        [self check:nmb_blit_buffer_to_texture(self.handle, [(NBBuffer *)staging handle], 0, w * 4, w * 4 * h, size,
                                               [(NBTexture *)d handle], slice, level, b)
               what:"copyFromBuffer:toTexture:"];
        return;
    }
    [self check:nmb_blit_buffer_to_texture(self.handle, [(NBBuffer *)s handle], o, row, image, size,
                                           [(NBTexture *)d handle], slice, level, b)
           what:"copyFromBuffer:toTexture:"];
}

- (void)copyFromTexture:(id<MTLTexture>)s
                 sourceSlice:(NSUInteger)slice
                 sourceLevel:(NSUInteger)level
                sourceOrigin:(MTLOrigin)so
                  sourceSize:(MTLSize)z
                    toBuffer:(id<MTLBuffer>)d
           destinationOffset:(NSUInteger)o
      destinationBytesPerRow:(NSUInteger)row
    destinationBytesPerImage:(NSUInteger)image
{
    NBApiTime timed;
    if (![self textures:NB_OBJECTS(s) count:1] || ![self buffers:NB_OBJECTS(d) count:1])
        return;
    if ([(NBTexture *)s rgWide]) {
        [self.commandBuffer refuse:@"copyFromTexture:toBuffer: from an RG8Unorm_sRGB texture held as RGBA8"];
        return;
    }
    const uint64_t a[3] = {so.x, so.y, so.z}, size[3] = {z.width, z.height, z.depth};
    [self check:nmb_blit_texture_to_buffer(self.handle, [(NBTexture *)s handle], slice, level, a, size,
                                           [(NBBuffer *)d handle], o, row, image)
           what:"copyFromTexture:toBuffer:"];
}

- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
    if (![self textures:NB_OBJECTS(t) count:1])
        return;
    textureGpuWrites((NBTexture *)t);
    [self check:nmb_blit_generate_mipmaps(self.handle, [(NBTexture *)t handle]) what:"generateMipmapsForTexture"];
}

- (void)synchronizeResource:(id<MTLResource>)r
{
    (void)r;
}

@end

// A two-plane render target's stand-in and the textures over its planes the store writes (rendersThroughStandIn).
@interface NBTwoPlaneStandIn : NSObject
// RGBA16Float: the passes' attachment in the texture's place.
@property (nonatomic, strong) id<MTLTexture> texture;
// R8Unorm over plane 0 (green), RG8Unorm over plane 1 (blue, red).
@property (nonatomic, strong) id<MTLTexture> luma, chroma;
@end
@implementation NBTwoPlaneStandIn
@end
// The load and store functions' states (TwoPlaneStandIn.metal), made once, synchronously: the store must draw in the encoder that
// needs it, which a state still being made on a worker would skip (AsyncPipelines.h). Nil, logged once, where either is refused.
static id<MTLRenderPipelineState> gStandInLoad, gStandInStore;

static bool standInStates()
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        id<MTLDevice> device = (id<MTLDevice>)gDeviceObject;
        dispatch_data_t data = dispatch_data_create(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary, nullptr,
                                                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
        const bool was = tAsyncWorker;
        tAsyncWorker = true;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.label = @"two-plane stand-in load";
        d.vertexFunction = [library newFunctionWithName:@"two_plane_cover"];
        d.fragmentFunction = [library newFunctionWithName:@"two_plane_load"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        gStandInLoad = library ? [device newRenderPipelineStateWithDescriptor:d error:&error] : nil;
        d.label = @"two-plane stand-in store";
        d.fragmentFunction = [library newFunctionWithName:@"two_plane_store"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm;
        d.colorAttachments[1].pixelFormat = MTLPixelFormatRG8Unorm;
        gStandInStore = gStandInLoad ? [device newRenderPipelineStateWithDescriptor:d error:&error] : nil;
        tAsyncWorker = was;
        if (!gStandInLoad || !gStandInStore)
            NBREFUSE("standin.states", 0, @"two-plane render targets refused: their load/store states: %@",
                     error.localizedDescription);
    });
    return gStandInLoad && gStandInStore;
}

// The stand-in of `texture` where a pass renders it through one (rendersThroughStandIn; a surface texture's level 0), made at its
// first pass and kept with the texture; nil for any other texture, or where the stand-in cannot be made (logged once; the
// runtime then refuses the pass).
static NBTwoPlaneStandIn *standInOf(id texture)
{
    if (![texture isKindOfClass:[NBTexture class]])
        return nil;
    NBTexture *planes = (NBTexture *)texture;
    if (!rendersThroughStandIn(planes.descriptor.format) || !planes.surface)
        return nil;
    @synchronized(planes) {
        if (planes.standIn)
            return planes.standIn;
        if (!standInStates())
            return nil;
        id<MTLDevice> device = (id<MTLDevice>)gDeviceObject;
        const NSUInteger width = planes.descriptor.width, height = planes.descriptor.height;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                     width:width
                                                                                    height:height
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        NBTwoPlaneStandIn *standIn = [NBTwoPlaneStandIn new];
        standIn.texture = [device newTextureWithDescriptor:d];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageRenderTarget;
        d.pixelFormat = MTLPixelFormatR8Unorm;
        standIn.luma = [device newTextureWithDescriptor:d iosurface:planes.surface plane:0];
        d.pixelFormat = MTLPixelFormatRG8Unorm;
        standIn.chroma = [device newTextureWithDescriptor:d iosurface:planes.surface plane:1];
        if (!standIn.texture || !standIn.luma || !standIn.chroma) {
            NBREFUSE("standin.texture", 0,
                     @"two-plane render target refused: no stand-in for a %lux%lu texture (%d%d%d)",
                     (unsigned long)width, (unsigned long)height, standIn.texture != nil, standIn.luma != nil,
                     standIn.chroma != nil);
            return nil;
        }
        planes.standIn = standIn;
        return standIn;
    }
}

static void completed(void *context);
static void scheduled(void *context);
@implementation NBCommandBuffer

NB_RESIDENCY_SETS(use)

// The kernel charges GPU time to the task that submits the work: there is no other task to name.
- (void)setResponsibleTaskIDs:(const uint32_t *)taskIDs count:(uint32_t)count
{
    (void)taskIDs;
    (void)count;
}

// The planes' codes into `standIn` before a pass that loads it: one draw reading the texture (Metal's tile of the attachment).
- (void)loadStandIn:(NBTwoPlaneStandIn *)standIn from:(id<MTLTexture>)planes
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = standIn.texture;
    d.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [(id<MTLCommandBuffer>)self renderCommandEncoderWithDescriptor:d];
    [encoder setRenderPipelineState:gStandInLoad];
    [encoder setFragmentTexture:planes atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

// `standIn` into its texture's planes once a pass that stores it ended: every texel's codes (Metal's store of the attachment).
- (void)storeStandIn:(NBTwoPlaneStandIn *)standIn
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = standIn.luma;
    d.colorAttachments[1].texture = standIn.chroma;
    for (unsigned i = 0; i < 2; i++) {
        d.colorAttachments[i].loadAction = MTLLoadActionDontCare;
        d.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> encoder = [(id<MTLCommandBuffer>)self renderCommandEncoderWithDescriptor:d];
    [encoder setRenderPipelineState:gStandInStore];
    [encoder setFragmentTexture:standIn.texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

- (instancetype)init
{
    static std::atomic<uint64_t> traceIDs{0};
    if ((self = [super init])) {
        _userDictionary = [NSMutableDictionary dictionary];
        _globalTraceObjectID = ++traceIDs;
        _scheduledHandlers = [NSMutableArray array];
        _completedHandlers = [NSMutableArray array];
        _resourceOwners = [NSMutableSet set];
        _handlers = dispatch_group_create();
        _schedulingHandlers = dispatch_group_create();
    }
    if (self)
        SWITCH_METAL_DIAGNOSTIC(createdUs = nowUs());
    return self;
}

- (void)dealloc
{
    nmb_command_buffer_release(_handle);
}

- (id<MTLCommandQueue>)commandQueue
{
    return (id<MTLCommandQueue>)_queue;
}

- (BOOL)retainedReferences
{
    return YES;
}

// Protected content is not supported (the runtime's AppleOnlyCall::SetProtectionOptions takes Metal's default only): any other option fails
// the command buffer.
- (uint64_t)protectionOptions
{
    return 0;
}

- (void)setProtectionOptions:(uint64_t)options
{
    if (options)
        [self refuse:[NSString stringWithFormat:@"protection options 0x%llx", options]];
}

- (void)refuse:(NSString *)what
{
    if (!self.encodeFailure) {
        self.encodeFailure = what;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        SWITCH_METAL_DIAGNOSTIC(gProblems++);
        noteProblem(what);
        os_log_error(OS_LOG_DEFAULT, "native-metal: command buffer refused: %{public}@", what);
#else
        NBLOG(@"native-metal: command buffer refused: %@", what);
#endif
    }
}

- (MTLCommandBufferStatus)status
{
    if (self.failedLocally)
        return MTLCommandBufferStatusError;
    return (MTLCommandBufferStatus)nmb_command_buffer_status(_handle);
}

- (NSError *)error
{
    if (self.failedLocally)
        return nbError(self.encodeFailure);
    if (self.status != MTLCommandBufferStatusError)
        return nil;
    char text[512] = {};
    nmb_command_buffer_error(_handle, text, sizeof text);
    const uint64_t code = nmb_command_buffer_error_code(_handle);
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain
                               code:code ? (NSInteger)code : MTLCommandBufferErrorInternal
                           userInfo:@{NSLocalizedDescriptionKey : [NSString stringWithUTF8String:text]}];
}

// Debug groups and cache hints name work for tools and the driver's caches; they change nothing the GPU computes here.
- (void)pushDebugGroup:(NSString *)name
{
    (void)name;
}

- (void)popDebugGroup
{}

- (void)encodeCacheHintTag:(uint64_t)tag resourceGroups:(const id __unsafe_unretained[])groups count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}

- (void)encodeCacheHintFinalize:(uint64_t)tag
                 resourceGroups:(const id __unsafe_unretained[])groups
                          count:(NSUInteger)count
{
    (void)tag;
    (void)groups;
    (void)count;
}

// The kernel driver's own submission times are not recorded: 0 is Metal's answer where they are unavailable.
- (CFTimeInterval)kernelStartTime
{
    return 0;
}

- (CFTimeInterval)kernelEndTime
{
    return 0;
}

- (BOOL)commitAndWaitUntilSubmitted
{
    [(id<MTLCommandBuffer>)self commit];
    [self waitUntilScheduled];
    return !self.failedLocally;
}

- (CFTimeInterval)GPUStartTime
{
    return nmb_command_buffer_gpu_time(_handle, false);
}

- (CFTimeInterval)GPUEndTime
{
    return nmb_command_buffer_gpu_time(_handle, true);
}

// Metal never answers an encoder request with nil, and clients rely on it: RenderBox's DirectRenderCommandEncoder::reset caches
// the encoder's IMPs through methodForSelector: and calls them, so a nil encoder is a jump to 0 (a SIGSEGV at pc 0 from
// RB::RenderQueue::encode). A refused request answers an encoder without a handle (refusedEncoder:): each call on it refuses again (the command buffer logs its first reason only) and the command buffer
// completes with an error, as a Metal command buffer that failed to encode does.
- (id)refusedEncoder:(NBEncoder *)encoder
{
    encoder.commandBuffer = self;
    return encoder;
}

- (id)encoder:(NBEncoder *)encoder handle:(NmbEncoder *)handle what:(NSString *)what
{
    if (self.committed) {
        [self refuse:[what stringByAppendingString:@" after commit"]];
        return [self refusedEncoder:encoder];
    }
    if (!handle) {
        [self refuse:[NSString stringWithFormat:@"%@: %@", what, lastError()]];
        return [self refusedEncoder:encoder];
    }
    encoder.handle = handle;
    encoder.commandBuffer = self;
    return encoder;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}

// A concurrent encoder leaves its dispatches' order to its memory barriers, which the runtime records.
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
    NBApiTime timed;
    SWITCH_METAL_DIAGNOSTIC(lastIsRender = false);
    NBComputeEncoder *encoder = [NBComputeEncoder new];
    encoder.kind = type;
    return [self encoder:encoder
                  handle:self.committed ? nullptr : nmb_compute_encoder_new_with_dispatch_type(_handle, type)
                    what:@"computeCommandEncoder"];
}

// The pass descriptor's counter sample buffers are not offered (no counter sets): one that names any fails the command buffer.
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d
{
    for (NSUInteger i = 0; i < 4; i++)
        if (d.sampleBufferAttachments[i].sampleBuffer) {
            [self refuse:@"compute pass counter samples"];
            return [self refusedEncoder:[NBComputeEncoder new]];
        }
    return [self computeCommandEncoderWithDispatchType:d.dispatchType];
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    NBApiTime timed;
    SWITCH_METAL_DIAGNOSTIC(lastIsRender = false);
    SWITCH_METAL_DIAGNOSTIC(gBlits++);
    SWITCH_METAL_DIAGNOSTIC(encodedWork.blits++);
    return [self encoder:[NBBlitEncoder new]
                  handle:self.committed ? nullptr : nmb_blit_encoder_new(_handle)
                    what:@"blitCommandEncoder"];
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    NBApiTime timed;
    NmbRenderPassDescriptor pass = {};
#if SWITCH_FRAME_TIMELINE
    {
        id<MTLTexture> first = d.colorAttachments[0].texture;
        if (first) {
            const uint64_t pixels = (uint64_t)first.width * first.height;
            gPasses++;
            gPassPixels += pixels;
            encodedWork.passes++;
            encodedWork.pixels += pixels;
            uint64_t seen = gBiggestPass.load();
            while (pixels > seen && !gBiggestPass.compare_exchange_weak(seen, pixels)) {
            }
            if (pixels >= 2000000)
                gFullScreenPasses++;
        }
    }
#endif
    // Two-plane attachments render into their stand-ins (rendersThroughStandIn): loaded from the planes first where the pass loads
    // them, stored back into them when the encoder ends where it stores them.
    NBTwoPlaneStandIn *standIns[8] = {};
    NSMutableArray<NBTwoPlaneStandIn *> *storeBacks = nil;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (!c.texture || c.level || c.slice || c.depthPlane || c.resolveTexture ||
            !(standIns[i] = standInOf(c.texture)))
            continue;
        // A DontCare load keeps the planes' codes in every 32x32 tile no draw touches (an M4 under macOS 27.0.1, raw/test/
        // standinoracle.mm), and the store writes every texel: the stand-in holds the codes there too
        if (c.loadAction == MTLLoadActionLoad ||
            (c.loadAction == MTLLoadActionDontCare && c.storeAction != MTLStoreActionDontCare))
            [self loadStandIn:standIns[i] from:c.texture];
        if (c.storeAction != MTLStoreActionDontCare) {
            if (!storeBacks)
                storeBacks = [NSMutableArray array];
            [storeBacks addObject:standIns[i]];
        }
    }
    auto attachment = [&](MTLRenderPassAttachmentDescriptor *a, NmbPassAttachment &out) -> bool {
        if (!a.texture)
            return true;
        if (![(id)a.texture isKindOfClass:[NBTexture class]])
            return false;
        NBTexture *texture = (NBTexture *)a.texture;
        [_resourceOwners addObject:texture];
        textureGpuWrites(texture);
        out.texture = texture.handle;
        out.level = a.level;
        out.slice = a.slice;
        out.depth_plane = a.depthPlane;
        out.load = a.loadAction;
        out.store = a.storeAction;
        // The MSAA resolve target (WebGL's antialiased drawing buffer, ANGLE's depth resolve): m2v resolves into it where the
        // store action resolves (without it a color resolve writes nothing and a depth or stencil one refuses the pass).
        if (id<MTLTexture> resolve = a.resolveTexture) {
            if (![(id)resolve isKindOfClass:[NBTexture class]])
                return false;
            [_resourceOwners addObject:resolve];
            textureGpuWrites((NBTexture *)resolve);
            out.resolve_texture = [(NBTexture *)resolve handle];
            out.resolve_level = a.resolveLevel;
            out.resolve_slice = a.resolveSlice;
            out.resolve_depth_plane = a.resolveDepthPlane;
        }
        return true;
    };
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        if (!attachment(c, pass.colors[i])) {
            [self refuse:@"foreign color attachment"];
            return [self refusedEncoder:[NBRenderEncoder new]];
        }
        if (standIns[i]) {
            [_resourceOwners addObject:standIns[i].texture];
            pass.colors[i].texture = [(NBTexture *)standIns[i].texture handle];
        }
        pass.colors[i].clear[0] = c.clearColor.red;
        pass.colors[i].clear[1] = c.clearColor.green;
        pass.colors[i].clear[2] = c.clearColor.blue;
        pass.colors[i].clear[3] = c.clearColor.alpha;
    }
    if (!attachment(d.depthAttachment, pass.depth) || !attachment(d.stencilAttachment, pass.stencil)) {
        [self refuse:@"foreign depth/stencil attachment"];
        return [self refusedEncoder:[NBRenderEncoder new]];
    }
    pass.depth.clear[0] = d.depthAttachment.clearDepth;
    pass.stencil.clear[0] = d.stencilAttachment.clearStencil;
    pass.depth.resolve_filter = d.depthAttachment.depthResolveFilter;
    pass.stencil.resolve_filter = d.stencilAttachment.stencilResolveFilter;
    pass.width = d.renderTargetWidth;
    pass.height = d.renderTargetHeight;
    pass.array_length = d.renderTargetArrayLength;
    pass.samples = d.defaultRasterSampleCount;
    if (d.visibilityResultBuffer) {
        if (![(id)d.visibilityResultBuffer isKindOfClass:[NBBuffer class]]) {
            [self refuse:@"foreign visibility result buffer"];
            return [self refusedEncoder:[NBRenderEncoder new]];
        }
        [_resourceOwners addObject:d.visibilityResultBuffer];
        pass.visibility = [(NBBuffer *)d.visibilityResultBuffer handle];
    }
    SWITCH_METAL_DIAGNOSTIC([self notePass:d]);
    NBRenderEncoder *render = [NBRenderEncoder new];
    render.storeBacks = storeBacks;
    return [self encoder:render
                  handle:self.committed ? nullptr : nmb_render_encoder_new(_handle, &pass)
                    what:@"renderCommandEncoder"];
}

// Count a render encoder that opens right after another of this command buffer on exactly its attachments, loading each it stored
// (DontCare after DontCare for one it discarded), with no visibility results: two passes one pass could hold.
#if SWITCH_FRAME_TIMELINE
- (void)notePass:(MTLRenderPassDescriptor *)d
{
    PassTarget now[10];
    for (unsigned i = 0; i < 10; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < 8    ? (MTLRenderPassAttachmentDescriptor *)d.colorAttachments[i]
                                               : i == 8 ? (MTLRenderPassAttachmentDescriptor *)d.depthAttachment
                                                        : (MTLRenderPassAttachmentDescriptor *)d.stencilAttachment;
        id texture = a.texture;
        now[i] = texture ? PassTarget{texture, a.level, a.slice, a.depthPlane, a.loadAction, a.storeAction}
                         : PassTarget{nil, 0, 0, 0, MTLLoadActionDontCare, MTLStoreActionDontCare};
    }
    const bool visibility = d.visibilityResultBuffer != nil;
    if (lastIsRender && !lastHadVisibility && !visibility && lastWidth == d.renderTargetWidth &&
        lastHeight == d.renderTargetHeight) {
        bool joinable = true;
        for (unsigned i = 0; i < 10 && joinable; i++) {
            const PassTarget &was = lastPass[i], &is = now[i];
            if (was.texture != is.texture) {
                joinable = false;
                break;
            }
            if (!is.texture)
                continue;
            joinable = was.level == is.level && was.slice == is.slice && was.plane == is.plane &&
                       (is.load == MTLLoadActionLoad ||
                        (was.store == MTLStoreActionDontCare && is.load == MTLLoadActionDontCare));
        }
        if (joinable)
            gJoinablePasses++;
    }
    memcpy(lastPass, now, sizeof now);
    lastIsRender = true;
    lastHadVisibility = visibility;
    lastWidth = d.renderTargetWidth;
    lastHeight = d.renderTargetHeight;
}
#endif

// A handler added once its point has passed (didSchedule or finish took the list) runs behind them, as RawMetal's does: added to
// the list, no one would read it again (a present that never happened, a waiter that never woke).
- (void)addScheduledHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    @synchronized(self) {
        now = _schedulingDelivered;
        if (!now)
            [_scheduledHandlers addObject:[block copy]];
    }
    if (now)
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            block((id<MTLCommandBuffer>)self);
        });
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)block
{
    if (!block)
        return;
    bool now;
    @synchronized(self) {
        now = _completedDelivered;
        if (!now)
            [_completedHandlers addObject:[block copy]];
    }
    if (now) {
        dispatch_queue_t queue = _queue.completionQueue;
        dispatch_async(queue ? queue : dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
            block((id<MTLCommandBuffer>)self);
        });
    }
}

// Presentation, as Metal's own -[_MTLCommandBuffer presentDrawable:] (21H461 Metal.framework): a scheduled handler that presents
// the drawable unless the command buffer failed, so a CAMetalLayer frame reaches Core Animation once its work is submitted, in
// commit order.
- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    if (!drawable)
        return;
    [self addScheduledHandler:^(id<MTLCommandBuffer> commandBuffer) {
        if (!commandBuffer.error)
            [drawable present];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime
{
    if (!drawable)
        return;
    [self addScheduledHandler:^(id<MTLCommandBuffer> commandBuffer) {
        if (!commandBuffer.error)
            [drawable presentAtTime:presentationTime];
    }];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration
{
    if (!drawable)
        return;
    [self addScheduledHandler:^(id<MTLCommandBuffer> commandBuffer) {
        if (!commandBuffer.error)
            [drawable presentAfterMinimumDuration:duration];
    }];
}

- (void)enqueue
{}

- (void)event:(id<MTLEvent>)event value:(uint64_t)value signal:(bool)signal
{
    SWITCH_METAL_DIAGNOSTIC(lastIsRender = false);
    if (![(id)event isKindOfClass:[NBEvent class]]) {
        [self refuse:@"foreign event"];
        return;
    }
    if (self.committed) {
        [self refuse:@"event encoded after commit"];
        return;
    }
    NmbEvent *handle = [(NBEvent *)event handle];
    if (!signal && [(id)event isKindOfClass:[NBSharedEvent class]]) {
        if (!_waitedEvents)
            _waitedEvents = [NSMutableArray array];
        [_waitedEvents addObject:event];
        eventWaits.emplace_back(handle, value);
    }
    if (!(signal ? nmb_command_buffer_signal_event(_handle, handle, value)
                 : nmb_command_buffer_wait_event(_handle, handle, value)))
        [self refuse:[NSString
                         stringWithFormat:@"%s: %@", signal ? "encodeSignalEvent" : "encodeWaitForEvent", lastError()]];
}

- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}

- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:false];
}

// IOGPU SPI (MPSGraph): signal once the command buffer is scheduled. Signalled at its place in the command stream instead, which is
// never earlier than scheduling: a waiter only waits longer.
- (void)encodeSignalEventScheduled:(id<MTLEvent>)event value:(uint64_t)value
{
    [self event:event value:value signal:true];
}

// The runtime has submitted the work. Deliver scheduling without waiting for GPU completion.
- (void)didSchedule
{
#if SWITCH_FRAME_TIMELINE
    frame.mark(switch_frame::Returned);
    switch_frame::trace(switch_frame::kScheduled, switch_frame::kPoint, frame.seq);
    {
        const uint64_t took = nowUs() - self.committedUs;
        std::lock_guard<std::mutex> lock(gCommitLock);
        CommitRecord &r = gCommitRing[self.commitIndex % 32];
        if (r.index == self.commitIndex)
            r.scheduledUs = took;
    }
#endif
    NSArray *handlers;
    @synchronized(self) {
        _schedulingDelivered = YES;
        handlers = [_scheduledHandlers copy];
        [_scheduledHandlers removeAllObjects];
    }
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler block in handlers)
            block((id<MTLCommandBuffer>)self);
        dispatch_group_leave(self->_schedulingHandlers);
    };
    // Run on the runtime's scheduled-handler thread, preserving its ordering and own-wait
    // deadlock checks. The queue's completionQueue routes completed handlers only.
    run();
}

// After completion (or a refusal): completed handlers, once.
- (void)finish
{
    // The GPU finished (or a local refusal never submitted). Mark every host import this command buffer used GPU-dirty: a cheap
    // flag store, no cache work here, so the render and completion paths pay nothing. The lines are invalidated lazily, only if
    // and when this process reads the import back (NBBuffer contents / NBTexture getBytes). Over-marking a read-only import is
    // harmless: its one later readback (rare) invalidates once. The framebuffer, which the CPU never reads, costs only the flag.
    if (!self.failedLocally)
        for (id owner in _resourceOwners) {
            NBBuffer *import = hostImportOf(owner);
            if (import)
                [import gpuWrote];
        }
    // Actual GPU completion (or a local refusal that never submitted) is the
    // release point. A retained completed command must not pin every old frame.
    [_resourceOwners removeAllObjects];
    @synchronized(gInFlight) {
        [gInFlight removeObject:self];
        if (!gInFlight.count)
            gMemoryRelease.drained();
    }
    NSArray *completedHandlers;
    @synchronized(self) {
        // A frontend refusal or a failed submission never scheduled. Unblock its waiter without inventing a scheduled event.
        if (!_schedulingDelivered) {
            _schedulingDelivered = YES;
            [_scheduledHandlers removeAllObjects];
            dispatch_group_leave(_schedulingHandlers);
        }
        _completedDelivered = YES;
        completedHandlers = [_completedHandlers copy];
        [_completedHandlers removeAllObjects];
    }
    void (^run)(void) = ^{
        for (MTLCommandBufferHandler block in completedHandlers)
            block((id<MTLCommandBuffer>)self);
#if SWITCH_FRAME_TIMELINE
        if (!self.failedLocally)
            switch_frame::finished(self->frame);
#endif
        dispatch_group_leave(self->_handlers);
    };
    dispatch_queue_t queue = _queue.completionQueue;
    if (queue)
        dispatch_async(queue, run);
    else
        run();
}

// What drives a process's command buffers. One commit every 30 s keeps the stack that committed it (frame pointers, up to 20
// frames below this call), each frame as image+offset (dladdr), and the plugin logs it once beside the rates line. In the shared
// cache, image load address + offset is the unslid address `ipsw dyld a2s` names. Costs one backtrace() per 30 s per process.
#if SWITCH_FRAME_TIMELINE
static std::atomic<uint64_t> gOriginNextUs{0};
#endif
static std::mutex gOriginLock;
static void *gOriginFrames[21];
static int gOriginCount;
static char gOriginThread[32];
static unsigned gOriginQos;

#if SWITCH_FRAME_TIMELINE
__attribute__((noinline)) static void noteCommitOrigin(uint64_t now)
{
    uint64_t next = gOriginNextUs.load(std::memory_order_relaxed);
    if (now < next || !gOriginNextUs.compare_exchange_strong(next, now + 30000000, std::memory_order_relaxed))
        return;
    void *frames[21];
    const int n = backtrace(frames, 21);
    char thread[32] = "";
    pthread_getname_np(pthread_self(), thread, sizeof thread);
    std::lock_guard<std::mutex> lock(gOriginLock);
    gOriginCount = n > 1 ? n - 1 : 0;  // frames[0] is this function
    if (gOriginCount)
        memcpy(gOriginFrames, frames + 1, sizeof(void *) * (size_t)gOriginCount);
    strlcpy(gOriginThread, thread, sizeof gOriginThread);
    gOriginQos = (unsigned)qos_class_self();
}

#endif
extern "C" size_t native_bridge_metal_commit_origin(char *out, size_t size)
{
    void *frames[21];
    int n;
    char thread[32];
    unsigned qos;
    {
        std::lock_guard<std::mutex> lock(gOriginLock);
        if (!gOriginCount || !out || !size)
            return 0;
        n = gOriginCount;
        memcpy(frames, gOriginFrames, sizeof(void *) * (size_t)n);
        strlcpy(thread, gOriginThread, sizeof thread);
        qos = gOriginQos;
        gOriginCount = 0;
    }
    int m = snprintf(out, size, "commit stack (thread '%s' qos 0x%x):", thread, qos);
    size_t at = m > 0 ? std::min<size_t>((size_t)m, size - 1) : 0;
    for (int i = 0; i < n && at + 1 < size; i++) {
        Dl_info info{};
        if (dladdr(frames[i], &info) && info.dli_fname && info.dli_fbase) {
            const char *name = strrchr(info.dli_fname, '/');
            m = snprintf(out + at, size - at, " %s+0x%lx", name ? name + 1 : info.dli_fname,
                         (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase));
        } else {
            m = snprintf(out + at, size - at, " 0x%lx", (unsigned long)(uintptr_t)frames[i]);
        }
        if (m <= 0)
            break;
        at = std::min<size_t>(at + (size_t)m, size - 1);
    }
    return at;
}

- (void)commit
{
#if SWITCH_FRAME_TIMELINE
    static thread_local switch_frame::Since since;
    switch_frame::committed(frame, since,
                            {gTimeSurfaceTexture.count.load(std::memory_order_relaxed),
                             gTimeSurfaceImport.count.load(std::memory_order_relaxed),
                             gSurfaceImportHits.load(std::memory_order_relaxed), 0});
    switch_frame::trace(switch_frame::kM2vCommit, switch_frame::kPoint, frame.seq);
    const uint64_t began = apiTiming() ? mach_absolute_time() : 0;
#endif
    applyPendingQueuePriorities();
    [self commitNow];
#if SWITCH_FRAME_TIMELINE
    if (began) {
        gApiUs += ticksToUs(tApiTicks + mach_absolute_time() - began);
        tApiTicks = 0;
    }
    frame.mark(switch_frame::Back);
#endif
}

- (void)commitNow
{
    if (self.committed) {
        NBLOG(@"native-metal: commit twice");
        return;
    }
    self.committed = YES;
#if SWITCH_FRAME_TIMELINE
    self.committedUs = nowUs();
    noteCommitOrigin(self.committedUs);
    {
        std::lock_guard<std::mutex> lock(gCommitLock);
        // Assign once under the same lock as publication. A concurrent commit cannot reuse this index.
        const uint32_t index = (uint32_t)(gCommits.fetch_add(1) + 1);
        self.commitIndex = index;
        CommitRecord &r = gCommitRing[index % 32];
        r = {};
        r.index = index;
        r.committedUs = self.committedUs;
        r.ageUs = self.committedUs - createdUs;
        r.work = encodedWork;
    }
    gEncodeUs += self.committedUs - createdUs;
    gHistEncode.add(self.committedUs - createdUs);
    if (tLastCommitUs && self.committedUs > tLastCommitUs && self.committedUs - tLastCommitUs < 250000)
        gHistGap.add(self.committedUs - tLastCommitUs);
    tLastCommitUs = self.committedUs;
#endif
    dispatch_group_enter(_handlers);
    dispatch_group_enter(_schedulingHandlers);
    @synchronized(gInFlight) {
        [gInFlight addObject:self];
    }
    if (!self.encodeFailure) {
        // No cache maintenance on the submit path: CPU writes into imports reach the GPU through the import-time clean
        // (BoImport, every import). The kernel's IOSurface-unlock clean does not run: IOSurfaceUnlock is an IOKit trap, and
        // the XNU hook sits on external methods. A CPU redraw into a still-live import is not cleaned.
        // Completion owns this reference; the runtime orders the non-owning scheduled callback before it.
        void *context = (__bridge_retained void *)self;
        bool committed = nmb_command_buffer_on_completed(_handle, completed, context) &&
                         nmb_command_buffer_on_scheduled(_handle, scheduled, context);
        if (committed) {
            SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
            committed = nmb_command_buffer_commit(_handle);
            SWITCH_METAL_DIAGNOSTIC(gTimeSubmit.add(began));
        }
        if (committed)
            return;
        (void)CFBridgingRelease(context);  // refused registration/commit cannot call the registered handlers
        [self refuse:[@"commit: " stringByAppendingString:lastError()]];
    }
    self.failedLocally = YES;
    SWITCH_METAL_DIAGNOSTIC(gCommitFailures++);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        [self finish];
    });
}

// Whether a committed command buffer not yet scheduled waits for a shared event value nothing reached yet (encodeWaitForEvent).
- (bool)waitsUnreached
{
    if (self.schedulingDelivered)
        return false;
    for (const auto &wait : eventWaits)
        if (nmb_shared_event_value(wait.first) < wait.second)
            return true;
    return false;
}

static bool heldBehindEvent()
{
    NSArray *pending;
    @synchronized(gInFlight) {
        pending = gInFlight.allObjects;
    }
    for (NBCommandBuffer *commandBuffer in pending)
        if ([commandBuffer waitsUnreached])
            return true;
    return false;
}

- (void)waitUntilCompleted
{
    if (!self.committed)
        return;
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    // Bounded: the kernel's GPU waits panic within 10 s, so past waitLimitMs this is a hang (a shared event no one
    // signals, a handler that never returns): one line and abort(), a crash report with the stack, not a silent freeze.
    if (!self.failedLocally && nmb_command_buffer_wait_completed_for(_handle, waitLimitMs()) == 1)
        waitTooLong("waitUntilCompleted of command buffer", (__bridge void *)self,
                    eventWaits.empty() ? 0 : eventWaits[0].second);
    if (dispatch_group_wait(_handlers, dispatch_time(DISPATCH_TIME_NOW, (int64_t)waitLimitMs() * NSEC_PER_MSEC)))
        waitTooLong("completed handlers of command buffer", (__bridge void *)self, 0);
    SWITCH_METAL_DIAGNOSTIC(gTimeWaitCompleted.add(began));
}

- (void)waitUntilScheduled
{
    if (!self.committed)
        return;
    SWITCH_METAL_DIAGNOSTIC(const uint64_t began = nowUs());
    if (!self.failedLocally) {
        int scheduled = nmb_command_buffer_wait_scheduled_for(_handle, kHeldGraceMs);
        if (scheduled == 1) {
            // Behind a shared event no one reached yet (its own wait, or an earlier command buffer's that holds it in order):
            // Metal's command buffer is scheduled there, its GPU waiting for the event, so this returns. Core Animation's render
            // thread flushes with waitUntilScheduled every frame and forces the per-surface event of a client that died or was
            // suspended after 5 s (CA::WindowServer::SharedEvent::is_complete_with_timeout): blocked here it never could, and
            // the display would freeze for good.
            if (heldBehindEvent()) {
                SWITCH_METAL_DIAGNOSTIC(gHeldReturns++);
                static std::atomic<bool> said{false};
                if (!said.exchange(true))
                    NBLOG(@"native-metal: waitUntilScheduled of a command buffer behind a shared event not reached "
                          @"yet: returns after %u ms, as Metal's (its GPU waits for the event)",
                          kHeldGraceMs);
                return;
            }
            scheduled = nmb_command_buffer_wait_scheduled_for(_handle, waitLimitMs());
            if (scheduled == 1)
                waitTooLong("waitUntilScheduled of command buffer", (__bridge void *)self, 0);
        }
        if (scheduled < 0) {
            NBLOG(@"native-metal: waitUntilScheduled refused: %@", lastError());
            return;
        }
    }
    if (dispatch_group_wait(_schedulingHandlers,
                            dispatch_time(DISPATCH_TIME_NOW, (int64_t)waitLimitMs() * NSEC_PER_MSEC)))
        waitTooLong("scheduled handlers of command buffer", (__bridge void *)self, 0);
    SWITCH_METAL_DIAGNOSTIC(gTimeWaitScheduled.add(began));
    SWITCH_METAL_DIAGNOSTIC(gHistSched.add(nowUs() - began));
}

@end

static void scheduled(void *context)
{
    [(__bridge NBCommandBuffer *)context didSchedule];
}

// The runtime's completion: the command buffer's own reference, taken at commit, is given back here.
static void completed(void *context)
{
    SWITCH_METAL_DIAGNOSTIC(gCompleted++);
    NBCommandBuffer *commandBuffer = (__bridge_transfer NBCommandBuffer *)context;
#if SWITCH_FRAME_TIMELINE
    commandBuffer->frame.mark(switch_frame::Fence);
    switch_frame::trace(switch_frame::kCompleted, switch_frame::kPoint, commandBuffer->frame.seq);
    {
        const uint64_t took = nowUs() - commandBuffer.committedUs;
        gTimeEncodeToCommit.count++;
        gTimeEncodeToCommit.total += took;
        gHistGpu.add(took);
        uint64_t seen = gTimeEncodeToCommit.slowest.load();
        while (took > seen && !gTimeEncodeToCommit.slowest.compare_exchange_weak(seen, took)) {
        }
        std::lock_guard<std::mutex> lock(gCommitLock);
        CommitRecord &r = gCommitRing[commandBuffer.commitIndex % 32];
        if (r.index == commandBuffer.commitIndex)
            r.callbackUs = took;
    }
#endif
    [commandBuffer finish];
}

@implementation NBQueue

NB_RESIDENCY_SETS(add)
NB_RESIDENCY_SETS(remove)

- (void)dealloc
{
    {
        std::lock_guard<std::mutex> lock(gQueuePriorityLock);
        gQueuePriorities.remove(gpuPriority, backgroundGpuPriority);
        if (priorityCall())
            applyQueuePriorities();
    }
    nmb_queue_release(_handle);
}

// The queue holds its 64 uncompleted command buffers past the bound: one fault naming the queue, its slots (held by uncompleted command buffers, of them not committed), and its oldest
// committed command buffer still in flight with its state, then abort() for the stack, as every other wait past its bound.
[[noreturn]] static void slotWaitTooLong(NBQueue *queue, const uint64_t slots[4])
{
    NBCommandBuffer *oldest = nil;
    NSUInteger inFlight = 0;
    @synchronized(gInFlight) {
        for (NBCommandBuffer *cb in gInFlight) {
            if (cb.queue != queue)
                continue;
            inFlight++;
            if (!oldest || cb.globalTraceObjectID < oldest.globalTraceObjectID)
                oldest = cb;
        }
    }
    char age[64] = "";
#if SWITCH_FRAME_TIMELINE
    if (oldest)
        snprintf(age, sizeof age, " committed %llu ms ago",
                 (unsigned long long)((nowUs() - oldest.committedUs) / 1000));
#endif
    os_log_fault(
        OS_LOG_DEFAULT,
        "native-metal: commandBuffer of queue %p (%{public}s) not made after %llu ms: %llu of %llu slots held by "
        "uncompleted command buffers (%llu not committed), %lu committed in flight; earliest-created #%llu '%{public}s'"
        "%{public}s, scheduled %d, waits for an unreached event %d, failed %d: abort() for the stack",
        queue, queue.label.UTF8String ?: "", (unsigned long long)waitLimitMs(), (unsigned long long)slots[1],
        (unsigned long long)slots[3], (unsigned long long)slots[2], (unsigned long)inFlight,
        oldest ? (unsigned long long)oldest.globalTraceObjectID : 0ull, oldest.label.UTF8String ?: "", age,
        oldest ? (int)oldest.schedulingDelivered : -1, oldest ? (int)[oldest waitsUnreached] : -1,
        oldest ? (int)(oldest.failedLocally || oldest.encodeFailure != nil) : -1);
#if !TARGET_OS_IPHONE
    fprintf(stderr,
            "native-metal: commandBuffer of queue %p not made after %llu ms: %llu of %llu slots held (%llu not "
            "committed)\n",
            (__bridge void *)queue, (unsigned long long)waitLimitMs(), (unsigned long long)slots[1],
            (unsigned long long)slots[3], (unsigned long long)slots[2]);
#endif
    abort();
}

- (id<MTLCommandBuffer>)commandBuffer
{
    NBApiTime timed;
    uint64_t slots[4] = {};
    NmbCommandBuffer *handle = nmb_command_buffer_new_within(_handle, waitLimitMs(), slots);
    if (!handle && slots[0])
        slotWaitTooLong(self, slots);
    if (!handle) {
        NBLOG(@"native-metal: commandBuffer refused: %@", lastError());
        return nil;
    }
    NBCommandBuffer *commandBuffer = [NBCommandBuffer new];
    commandBuffer.handle = handle;
    commandBuffer.queue = self;
    return (id<MTLCommandBuffer>)commandBuffer;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return [self commandBuffer];
}

- (void)setSubmissionQueue:(dispatch_queue_t)queue
{
    (void)queue;
}  // submission happens on the caller's thread here

// setCompletionQueue: is the property's setter.
// The GPU driver runs the process's channels at the most urgent priority its queues asked for (QueuePriority.h).
- (BOOL)setGPUPriority:(NSUInteger)priority
{
    return setQueuePriority(self, priority, false);
}

- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
    return setQueuePriority(self, priority, true);
}

- (NSUInteger)getGPUPriority
{
    std::lock_guard<std::mutex> lock(gQueuePriorityLock);
    return gpuPriority;
}

- (NSUInteger)getBackgroundGPUPriority
{
    std::lock_guard<std::mutex> lock(gQueuePriorityLock);
    return backgroundGpuPriority;
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d
{
    (void)d;
    return [self commandBuffer];
}

@end

// ---- the device ----
template <typename T> static T failWith(NSError **error, NSString *what, T value)
{
    noteProblem([NSString stringWithFormat:@"%@: %@", what, lastError()]);
    if (error)
        *error = nbError([NSString stringWithFormat:@"%@: %@", what, lastError()]);
    return value;
}

// Which of QuartzCore's paths asked for a pipeline (int-153: every one was made synchronously on its render thread and no generic pipeline
// was ever asked for): its return addresses as offsets from its image header (header 0x189429000 in the 21H461 cache, so offset +
// 0x189429000 is the address in that image's disassembly), the global feature flags it ORs into every context's flags, and its
// CA_DISABLE_GENERIC_SHADERS, CA_FORCE_GENERIC_SHADERS and CA_DISABLE_PRECOMPILED_PIPELINES debug options (ca_debug_options +0x55..0x57,
// read only where dladdr places that address in QuartzCore).
// QuartzCore's image is found once in dyld's image list (looked for again only after an image loads): its header, the end of its
// __TEXT and whether its debug options lie in it; each frame is compared with that range, not dladdr'd (a dladdr per frame, up to
// 24 a line, costs about a millisecond per journal line on the thread making the state).
#if SWITCH_FRAME_TIMELINE
struct NBQuartzCoreImage {
    uintptr_t header = 0, end = 0;  // [header, end): QuartzCore's __TEXT, 0 until QuartzCore is loaded
    bool optionsInImage = false;    // whether dladdr places the debug options' address in QuartzCore (below)
};

static NBQuartzCoreImage quartzCoreImage()
{
    static std::mutex lock;
    static NBQuartzCoreImage image;
    static uint32_t imagesSeen = 0;  // the loaded image count at the last look: a look again only after an image loaded
    std::lock_guard<std::mutex> guard(lock);
    if (image.header || _dyld_image_count() == imagesSeen)
        return image;
    imagesSeen = _dyld_image_count();
    for (uint32_t i = 0; i < imagesSeen && !image.header; ++i) {
        // the image dladdr names ".../QuartzCore" (QuartzCore.framework/QuartzCore, or Versions/A/QuartzCore on a Mac)
        const char *name = _dyld_get_image_name(i);
        const char *slash = name ? strrchr(name, '/') : nullptr;
        if (!slash || strcmp(slash, "/QuartzCore") || !strstr(name, "/QuartzCore.framework/"))
            continue;
        const auto *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!mh)
            continue;
        const auto *command = (const struct load_command *)(mh + 1);
        for (uint32_t c = 0; mh->magic == MH_MAGIC_64 && c < mh->ncmds; ++c) {
            const auto *segment = (const struct segment_command_64 *)command;
            if (command->cmd == LC_SEGMENT_64 && !strncmp(segment->segname, "__TEXT", sizeof segment->segname)) {
                image.end = (uintptr_t)mh + segment->vmsize;  // __TEXT begins at the header (dyld cache and file alike)
                break;
            }
            command = (const struct load_command *)((const char *)command + command->cmdsize);
        }
        if (!image.end)
            continue;
        image.header = (uintptr_t)mh;
        const uintptr_t options = image.header + (0x1f491b3c5ull - 0x189429000ull);
        Dl_info at{};
        image.optionsInImage = dladdr((void *)options, &at) && at.dli_fname && strstr(at.dli_fname, "/QuartzCore") &&
                               (uintptr_t)at.dli_fbase == image.header;
    }
    return image;
}

static std::string quartzCoreCallers()
{
    void *frames[24];
    const int n = backtrace(frames, 24);
    std::string out = "qc";
    char item[32];
    unsigned kept = 0;
    uintptr_t header = 0;
    const NBQuartzCoreImage image = quartzCoreImage();
    for (int i = 0; i < n && kept < 8 && image.header; i++) {
        if ((uintptr_t)frames[i] < image.header || (uintptr_t)frames[i] >= image.end)
            continue;
        header = image.header;
        snprintf(item, sizeof item, " %lx", (unsigned long)((uintptr_t)frames[i] - header));
        out += item;
        ++kept;
    }
    // The context's own flags (int-154: the draws never took the generic path, which only bit 18 of MetalContext +0x1a0 or the debug byte
    // can cause, and the debug byte was 0). MetalContext::create_pipeline_state (header +0x143000) saves its caller's x19 at fp-8, and its
    // caller MetalContext::draw keeps `this` in x19: the frame record whose return address is +0x143214 links to that frame. Read with
    // vm_read so a different stack layout reports ctx=? instead of faulting. In the render server only: elsewhere a sandbox may
    // refuse the vm_read_overwrite MIG routine (WebKit's GPU process does), ctx=? then.
    static const bool renderServer = !strcmp(getprogname(), "backboardd");
    if (header && !renderServer)
        out += " ctx=?";
    if (header && renderServer) {
        uintptr_t fp = (uintptr_t)__builtin_frame_address(0), ctx = 0;
        for (unsigned depth = 0; fp && depth < 24; ++depth) {
            uintptr_t record[2] = {};
            vm_size_t got = 0;
            if (vm_read_overwrite(mach_task_self(), fp, sizeof record, (vm_address_t)record, &got) != KERN_SUCCESS ||
                got != sizeof record)
                break;
            if (record[1] - header == 0x143214) {
                uintptr_t saved = 0;
                if (record[0] && vm_read_overwrite(mach_task_self(), record[0] - 8, sizeof saved, (vm_address_t)&saved,
                                                   &got) == KERN_SUCCESS)
                    ctx = saved;
                break;
            }
            if (record[0] <= fp)
                break;
            fp = record[0];
        }
        uint32_t flags[2] = {};
        vm_size_t got = 0;
        if (ctx && vm_read_overwrite(mach_task_self(), ctx + 0x1a0, 4, (vm_address_t)&flags[0], &got) == KERN_SUCCESS &&
            vm_read_overwrite(mach_task_self(), ctx + 0xb3c, 4, (vm_address_t)&flags[1], &got) == KERN_SUCCESS) {
            snprintf(item, sizeof item, " ctx=%x/%x", flags[0], flags[1]);
            out += item;
        } else
            out += " ctx=?";
    }
    static auto global = (uint32_t (*)(void))dlsym(RTLD_DEFAULT, "CARenderGetGlobalFeatureFlags");
    snprintf(item, sizeof item, " global=%x", global ? global() : 0xffffffffu);
    out += item;
    if (header) {
        const uintptr_t options = header + (0x1f491b3c5ull - 0x189429000ull);
        if (image.optionsInImage) {
            const volatile uint8_t *b = (const volatile uint8_t *)options;
            snprintf(item, sizeof item, " dbg=%u%u%u", b[0], b[1], b[2]);
            out += item;
        } else
            out += " dbg=?";
    }
    return out;
}

// What a creation cost beyond its wall time (int-156: a translation the host does in 6 ms took 665 ms on the tablet, whose single core is
// as fast): this thread's CPU time (user and system, microseconds) and the task's page faults, page-ins and context switches. A creation
// whose CPU time is near its wall time computed; one whose faults are many paid the 4 KiB fault path; one far below it waited.
// The thread's times come from proc_pidinfo(PROC_PIDTHREADID64INFO) of this thread (process-info-pidinfo of itself, which every
// sandbox that admits the device admits), not thread_info(mach_thread_self()): WebKit's GPU process profile refuses both the
// thread_self trap and the thread_info MIG routine. The iOS SDK has no libproc.h: its declarations, from sys/proc_info.h.
extern "C" int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);

struct NBThreadInfo {  // struct proc_threadinfo
    uint64_t user_ns, system_ns;
    int32_t cpu_usage, policy, run_state, flags, sleep_time, curpri, priority, maxpriority;
    char name[64];
};

static_assert(sizeof(NBThreadInfo) == 112, "struct proc_threadinfo");

struct NBCost {
    uint64_t user = 0, system = 0;
    int64_t faults = 0, pageins = 0, switches = 0;

    static NBCost now()
    {
        NBCost c;
        constexpr int kThreadId64Info = 15;  // PROC_PIDTHREADID64INFO
        uint64_t thread = 0;
        NBThreadInfo t{};
        if (!pthread_threadid_np(nullptr, &thread) &&
            proc_pidinfo(getpid(), kThreadId64Info, thread, &t, (int)sizeof t) == (int)sizeof t) {
            c.user = t.user_ns / 1000;
            c.system = t.system_ns / 1000;
        }
        task_events_info_data_t e{};
        mach_msg_type_number_t n = TASK_EVENTS_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&e, &n) == KERN_SUCCESS) {
            c.faults = e.faults;
            c.pageins = e.pageins;
            c.switches = e.csw;
        }
        return c;
    }

    std::string since(const NBCost &b) const
    {
        char text[128];
        snprintf(text, sizeof text, " cpu=%llu sys=%llu flt=%lld pin=%lld csw=%lld",
                 (unsigned long long)(user + system - b.user - b.system), (unsigned long long)(system - b.system),
                 (long long)(faults - b.faults), (long long)(pageins - b.pageins), (long long)(switches - b.switches));
        return text;
    }
};

// A render descriptor's linked functions as its R line names them, after its stages: " | <vl|vp|fl|fp> <stage>" for each of the
// vertex and fragment stages' functions and privateFunctions, in that order
static std::string linkedText(MTLRenderPipelineDescriptor *d)
{
    std::string text;
    const std::pair<const char *, NSArray *> lists[] = {{"vl", d.vertexLinkedFunctions.functions},
                                                        {"vp", d.vertexLinkedFunctions.privateFunctions},
                                                        {"fl", d.fragmentLinkedFunctions.functions},
                                                        {"fp", d.fragmentLinkedFunctions.privateFunctions}};
    for (const auto &[tag, functions] : lists)
        for (id f in functions)
            text += std::string(" | ") + tag + " " +
                    stageText([f isKindOfClass:[NBFunction class]] ? (NBFunction *)f : nil);
    return text;
}

// One journal line of a render pipeline creation begun at `began` (microseconds), `refused` its refusal or null. The label is the
// caller's name for the pipeline (QuartzCore's: its spec's encoded name). Built on the creating thread: it names that thread
// and its QuartzCore callers.
static std::string renderPipelineLine(MTLRenderPipelineDescriptor *d, MTLPipelineOption options, uint64_t began,
                                      const char *refused, const std::string &cost)
{
    const uint64_t ended = nowUs();
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    char head[256];
    snprintf(head, sizeof head, "R t=%llu tid=%llu main=%d us=%llu ok=%d samples=%lu topo=%lu ds=%lu/%lu",
             (unsigned long long)((began - gJournalBegan) / 1000), (unsigned long long)tid, pthread_main_np(),
             (unsigned long long)(ended - began), refused ? 0 : 1, (unsigned long)d.rasterSampleCount,
             (unsigned long)d.inputPrimitiveTopology, (unsigned long)d.depthAttachmentPixelFormat,
             (unsigned long)d.stencilAttachmentPixelFormat);
    std::string line = head;
    line += cost;
    if (options)
        line += " opt=" + std::to_string((unsigned long long)options);
    line += " label=";
    line += d.label.length ? d.label.UTF8String : "-";
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid)
            continue;
        snprintf(head, sizeof head, " c%u=%lu:%lu:%d:%lu:%lu:%lu:%lu:%lu:%lu", i, (unsigned long)a.pixelFormat,
                 (unsigned long)a.writeMask, (int)a.blendingEnabled, (unsigned long)a.sourceRGBBlendFactor,
                 (unsigned long)a.destinationRGBBlendFactor, (unsigned long)a.rgbBlendOperation,
                 (unsigned long)a.sourceAlphaBlendFactor, (unsigned long)a.destinationAlphaBlendFactor,
                 (unsigned long)a.alphaBlendOperation);
        line += head;
    }
    if (MTLVertexDescriptor *v = d.vertexDescriptor)
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex < 31 ? a.bufferIndex : 0];
            snprintf(head, sizeof head, " a%u=%lu:%lu:%lu:%lu:%lu:%lu", i, (unsigned long)a.format,
                     (unsigned long)a.offset, (unsigned long)a.bufferIndex, (unsigned long)l.stride,
                     (unsigned long)l.stepFunction, (unsigned long)l.stepRate);
            line += head;
        }
    NBFunction *vf = [(id)d.vertexFunction isKindOfClass:[NBFunction class]] ? (NBFunction *)d.vertexFunction : nil;
    NBFunction *ff = [(id)d.fragmentFunction isKindOfClass:[NBFunction class]] ? (NBFunction *)d.fragmentFunction : nil;
    line += " | v " + stageText(vf);
    line += " | f " + stageText(ff);
    line += linkedText(d);
    if (refused) {
        line += " | refused ";
        line += refused;
    }
    line += " | ";
    line += quartzCoreCallers();
    for (char &c : line)
        if (c == '\n')
            c = ' ';
    return line;
}

static void journalRenderPipeline(MTLRenderPipelineDescriptor *d, MTLPipelineOption options, uint64_t began,
                                  const char *refused, const std::string &cost)
{
    if (journalOn())
        journal(renderPipelineLine(d, options, began, refused, cost));
}

// One journal line of a compute pipeline creation begun at `began`, `refused` its refusal or null: made from the function `f` alone
// (form=f) or from the descriptor `d` (form=d: its fields, the kernel as k, its visible linked functions as l and private ones as
// p), with `options`; the host makes it again the same way (precompile.mm).
// Returns what made the state as the C line names it ("form=... | k ... | "), which its D lines repeat; "" when not journaled.
static std::string journalComputePipeline(MTLComputePipelineDescriptor *d, NBFunction *f, MTLPipelineOption options,
                                          uint64_t began, const char *refused)
{
    if (!journalOn())
        return std::string();
    char head[256];
    snprintf(head, sizeof head, "form=%s opt=%llu", d ? "d" : "f", (unsigned long long)options);
    std::string state = head;
    if (d) {
        char mutability[32] = {};
        for (unsigned i = 0; i < 31; i++)
            mutability[i] = (char)('0' + (d.buffers[i].mutability & 7));
        snprintf(head, sizeof head, " max=%lu tgm=%d icb=%d depth=%lu abf=%d mut=%s",
                 (unsigned long)d.maxTotalThreadsPerThreadgroup, (int)d.threadGroupSizeIsMultipleOfThreadExecutionWidth,
                 (int)d.supportIndirectCommandBuffers, (unsigned long)d.maxCallStackDepth,
                 (int)d.supportAddingBinaryFunctions, mutability);
        state += head;
        state += " label=" + journalToken(d.label.UTF8String);
        f = (NBFunction *)d.computeFunction;
    }
    state += " | k " + stageText(f);
    for (id l in d.linkedFunctions.functions)
        state += " | l " + stageText([l isKindOfClass:[NBFunction class]] ? (NBFunction *)l : nil);
    for (id l in d.linkedFunctions.privateFunctions)
        state += " | p " + stageText([l isKindOfClass:[NBFunction class]] ? (NBFunction *)l : nil);
    for (char &c : state)
        if (c == '\n')
            c = ' ';
    snprintf(head, sizeof head, "C t=%llu us=%llu ok=%d ", (unsigned long long)((began - gJournalBegan) / 1000),
             (unsigned long long)(nowUs() - began), refused ? 0 : 1);
    std::string line = head + state;
    if (refused)
        line += std::string(" | refused ") + refused;
    line += " | " + quartzCoreCallers();
    for (char &c : line)
        if (c == '\n')
            c = ' ';
    journal(line);
    return refused ? std::string() : state + " | ";
}

#endif

extern "C" void native_bridge_metal_journal_render(id descriptor, NSUInteger options)
{
#if SWITCH_FRAME_TIMELINE
    // The creating thread can be Core Animation's render thread: the journal's lock and write(2) run on the persist
    // queue, which keeps these lines in their order.
    if (journalOn()) {
        const std::string line = renderPipelineLine((MTLRenderPipelineDescriptor *)descriptor,
                                                    (MTLPipelineOption)options, nowUs(), nullptr, std::string());
        dispatch_async(persistQueue(), ^{
            journal(line);
        });
    }
#endif
    (void)descriptor;
    (void)options;
}

static bool linkedHandles(NSArray *functions, std::vector<NmbFunction *> &out);

// A stage's visible and private linked functions appended to `linked`; false for what the bridge does not represent (binary
// functions, function groups) or a function of another device.
static bool stageLinked(MTLLinkedFunctions *functions, std::vector<NmbFunction *> &linked, size_t &visible,
                        size_t &hidden, NSError **error)
{
    if (functions.binaryFunctions.count || functions.groups.count) {
        if (error)
            *error = nbError(@"render binary functions and function groups are not implemented");
        return false;
    }
    const size_t before = linked.size();
    if (!linkedHandles(functions.functions, linked)) {
        if (error)
            *error = nbError(@"foreign linked function");
        return false;
    }
    visible = linked.size() - before;
    if (!linkedHandles(functions.privateFunctions, linked)) {
        if (error)
            *error = nbError(@"foreign linked function");
        return false;
    }
    hidden = linked.size() - before - visible;
    return true;
}

// A render pipeline descriptor in the bridge's form; false, with the error, for a function of another device. `linked` holds
// the stages' linked functions' handles that `p` points into: it outlives the creation call.
static bool renderDescriptor(MTLRenderPipelineDescriptor *d, NmbRenderPipelineDescriptor *p,
                             std::vector<NmbFunction *> &linked, NSError **error)
{
    if (![(id)d.vertexFunction isKindOfClass:[NBFunction class]] ||
        (d.fragmentFunction && ![(id)d.fragmentFunction isKindOfClass:[NBFunction class]])) {
        if (error)
            *error = nbError(@"foreign function");
        return false;
    }
    // Metal's functions that call a function by name (IconRendering's custom effects call custom_fn) get it from these
    linked.clear();
    size_t vertexVisible = 0, vertexHidden = 0, fragmentVisible = 0, fragmentHidden = 0;
    if (!stageLinked(d.vertexLinkedFunctions, linked, vertexVisible, vertexHidden, error) ||
        !stageLinked(d.fragmentLinkedFunctions, linked, fragmentVisible, fragmentHidden, error))
        return false;
    p->vertex_linked = linked.data();
    p->vertex_linked_count = vertexVisible;
    p->vertex_private_linked = linked.data() + vertexVisible;
    p->vertex_private_count = vertexHidden;
    p->fragment_linked = linked.data() + vertexVisible + vertexHidden;
    p->fragment_linked_count = fragmentVisible;
    p->fragment_private_linked = linked.data() + vertexVisible + vertexHidden + fragmentVisible;
    p->fragment_private_count = fragmentHidden;
    p->vertex = [(NBFunction *)d.vertexFunction handle];
    p->fragment = d.fragmentFunction ? [(NBFunction *)d.fragmentFunction handle] : nullptr;
    for (unsigned i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[i];
        // A two-plane code format renders into its texture's RGBA16Float stand-in (rendersThroughStandIn).
        const NSUInteger format = rendersThroughStandIn(a.pixelFormat) ? MTLPixelFormatRGBA16Float : a.pixelFormat;
        p->colors[i] = {(uint64_t)format,
                        (uint64_t)a.writeMask,
                        (uint64_t)a.blendingEnabled,
                        (uint64_t)a.sourceRGBBlendFactor,
                        (uint64_t)a.destinationRGBBlendFactor,
                        (uint64_t)a.rgbBlendOperation,
                        (uint64_t)a.sourceAlphaBlendFactor,
                        (uint64_t)a.destinationAlphaBlendFactor,
                        (uint64_t)a.alphaBlendOperation};
    }
    p->depth_format = d.depthAttachmentPixelFormat;
    p->stencil_format = d.stencilAttachmentPixelFormat;
    p->samples = d.rasterSampleCount;
    p->topology = d.inputPrimitiveTopology;
    p->rasterization = d.rasterizationEnabled;
    // Metal's defaults are what the bridge takes without it (m2v TessellationDescriptor::default)
    if (d.maxTessellationFactor != 16 || d.tessellationPartitionMode || d.tessellationFactorFormat ||
        d.tessellationFactorStepFunction || d.tessellationOutputWindingOrder || d.tessellationControlPointIndexType ||
        d.tessellationFactorScaleEnabled) {
        p->tessellation = 1;
        p->max_tessellation_factor = d.maxTessellationFactor;
        p->partition_mode = d.tessellationPartitionMode;
        p->factor_format = d.tessellationFactorFormat;
        p->factor_step_function = d.tessellationFactorStepFunction;
        p->winding = d.tessellationOutputWindingOrder;
        p->control_point_index_type = d.tessellationControlPointIndexType;
        p->factor_scale_enabled = d.tessellationFactorScaleEnabled;
    }
    p->max_amplification = d.maxVertexAmplificationCount;
    if (MTLVertexDescriptor *v = d.vertexDescriptor) {
        for (unsigned i = 0; i < 31; i++) {
            MTLVertexAttributeDescriptor *a = v.attributes[i];
            if (a.format == MTLVertexFormatInvalid)
                continue;
            p->vertex_inputs = 1;
            p->attributes[i] = {(uint64_t)a.format, a.offset, a.bufferIndex};
            if (a.bufferIndex < 31) {
                MTLVertexBufferLayoutDescriptor *l = v.layouts[a.bufferIndex];
                p->layouts[a.bufferIndex] = {l.stride, (uint64_t)l.stepFunction, l.stepRate};
            }
        }
    }
    return true;
}

// ---- heaps ----
// Metal heaps over the runtime's: an automatic heap places each resource, a placement heap takes an offset. Resources made from one keep it
// alive (their `heap`); a heap's resources share its storage mode, CPU cache mode and (here) hazard tracking mode. (Its interface is
// with the encoders', which make heaps resident.)
@implementation NBHeap

- (void)dealloc
{
    nmb_heap_release(_handle);
}

- (NSUInteger)size
{
    return nmb_heap_size(_handle);
}

- (NSUInteger)usedSize
{
    return nmb_heap_used_size(_handle);
}

- (NSUInteger)currentAllocatedSize
{
    return nmb_heap_allocated_size(_handle);
}

- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
    return nmb_heap_max_available_size(_handle, alignment);
}

- (MTLResourceOptions)resourceOptions
{
    return _options;
}

- (MTLStorageMode)storageMode
{
    return (MTLStorageMode)((_options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift);
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return (MTLCPUCacheMode)((_options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift);
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    return (MTLHazardTrackingMode)((_options & MTLResourceHazardTrackingModeMask) >>
                                   MTLResourceHazardTrackingModeShift);
}

- (MTLHeapType)type
{
    return _heapType;
}

- (uint64_t)protectionOptions
{
    return 0;
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    const uint64_t answer = nmb_heap_set_purgeable_state(_handle, state);
    if (!answer)
        NBLOG(@"native-metal: heap setPurgeableState refused: %@", lastError());
    return (MTLPurgeableState)answer;
}

- (id<MTLBuffer>)bufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(uint64_t)offset
{
    NmbBuffer *handle = nmb_heap_new_buffer(_handle, length, options, offset);
    if (!handle) {
        NBLOG(@"native-metal: heap buffer refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    buffer.parentHeap = (id<MTLHeap>)self;
    buffer.heapOffset = offset == UINT64_MAX ? 0 : offset;
    return (id<MTLBuffer>)buffer;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    return [self bufferWithLength:length options:options offset:UINT64_MAX];
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
    return [self bufferWithLength:length options:options offset:offset];
}

- (id<MTLTexture>)textureWithDescriptor:(MTLTextureDescriptor *)d offset:(uint64_t)offset
{
    NmbTextureDescriptor descriptor;
    if (!textureDescriptor(d, &descriptor))
        return nil;
    NmbTexture *handle = nmb_heap_new_texture(_handle, &descriptor, offset);
    if (!handle) {
        NBLOG(@"native-metal: heap texture refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    texture.descriptor = descriptor;
    texture.parentHeap = (id<MTLHeap>)self;
    texture.heapOffset = offset == UINT64_MAX ? 0 : offset;
    return (id<MTLTexture>)texture;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    return [self textureWithDescriptor:d offset:UINT64_MAX];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
    return [self textureWithDescriptor:d offset:offset];
}

@end
// Apple's SPI resource groups name resources for cache hints (encodeCacheHintTag:/Finalize:); a group only keeps its resources alive.
@interface NBResourceGroup : NBObject
@property (nonatomic, strong) NSArray *resources;
@end
@implementation NBResourceGroup
@end

// Apple's texture layout object (MTLTextureLayout, Apple SPI, from newTextureLayoutWithDescriptor:isHeapOrBufferBacked:):
// where a texture's texels lie in memory its caller holds, and the copies between that memory and linear rows. ImageIO keeps
// PosterBoard's wallpaper snapshots in it (ATX files written "pre-twiddled", kCGImagePropertyASTCPreTwiddle; its
// ASTCTwiddler writes and reads them through this object), and Core Animation and PhotosUI ask for it for compressed images.
// No GPU reads that memory here (newTextureWithBytesNoCopy: is refused), so the layout is the plain one: each slice holds
// its levels in turn, each level its images, each image its rows of blocks packed; a copy is a row copy. The watermark
// (which GPU the memory was laid out for; ImageIO refuses a file whose watermark differs) is zero, as in the snapshots
// written while this object was refused (ImageIO then wrote the same packed rows), so those read as they are.
struct NBWatermark {
    uint64_t words[2];
};

static constexpr uint64_t kLayoutMaxLevels = 16;

static uint64_t blocksOf(uint64_t texels, uint64_t block)
{
    return (texels + block - 1) / block;
}

struct PlainLayout {
    uint64_t block[4];  // bytes, width, height, depth
    uint64_t width, height, depth, levels, slices, sliceBytes;
    uint64_t levelOffset[kLayoutMaxLevels];  // within a slice

    // A level's rows of blocks: the bytes of one, how many an image has, and its images (3D: its depth in blocks; else 1).
    uint64_t rowBytes(uint64_t level) const
    {
        return blocksOf(std::max<uint64_t>(1, width >> level), block[1]) * block[0];
    }

    uint64_t rows(uint64_t level) const
    {
        return blocksOf(std::max<uint64_t>(1, height >> level), block[2]);
    }

    uint64_t images(uint64_t level) const
    {
        return blocksOf(std::max<uint64_t>(1, depth >> level), block[3]);
    }

    // copy(offset in the texture's memory, offset in the linear bytes, bytes) for each row of blocks of `region` (texels) of
    // slice/level, the linear rows `rowStride` apart (0: packed) and images `imageStride` apart (0: packed); false for a
    // region outside the level.
    template <typename Copy>
    bool eachRow(const MTLRegion &region, uint64_t slice, uint64_t level, uint64_t rowStride, uint64_t imageStride,
                 Copy copy) const
    {
        const uint64_t x = region.origin.x / block[1], y = region.origin.y / block[2], z = region.origin.z / block[3];
        const uint64_t length = blocksOf(region.size.width, block[1]) * block[0],
                       height = blocksOf(region.size.height, block[2]), count = blocksOf(region.size.depth, block[3]);
        if (slice >= slices || level >= levels || x * block[0] + length > rowBytes(level) || y + height > rows(level) ||
            z + count > images(level))
            return false;
        const uint64_t row = rowBytes(level), image = row * rows(level);
        const uint64_t base = slice * sliceBytes + levelOffset[level] + z * image + y * row + x * block[0];
        if (!rowStride)
            rowStride = length;
        if (!imageStride)
            imageStride = height * rowStride;
        for (uint64_t i = 0; i < count; i++)
            for (uint64_t r = 0; r < height; r++)
                copy(base + i * image + r * row, i * imageStride + r * rowStride, length);
        return true;
    }
};

static void refusedLayoutCopy(NSUInteger slice, NSUInteger level, const MTLRegion &r)
{
    NBLOG(@"native-metal: texture layout copy refused: slice %lu level %lu region (%lu,%lu,%lu) %lux%lux%lu is outside",
          (unsigned long)slice, (unsigned long)level, (unsigned long)r.origin.x, (unsigned long)r.origin.y,
          (unsigned long)r.origin.z, (unsigned long)r.size.width, (unsigned long)r.size.height,
          (unsigned long)r.size.depth);
}

@interface NBTextureLayout : NBObject
- (instancetype)initWithDescriptor:(MTLTextureDescriptor *)d block:(const uint64_t *)block;
@end
@implementation NBTextureLayout {
    PlainLayout _layout;
}

- (instancetype)initWithDescriptor:(MTLTextureDescriptor *)d block:(const uint64_t *)block
{
    if (!(self = [super init]))
        return nil;
    PlainLayout &l = _layout;
    memcpy(l.block, block, sizeof(l.block));
    l.width = d.width;
    l.height = d.height;
    l.depth = d.textureType == MTLTextureType3D ? d.depth : 1;
    l.levels = d.mipmapLevelCount;
    const bool cube = d.textureType == MTLTextureTypeCube || d.textureType == MTLTextureTypeCubeArray;
    l.slices = (d.textureType == MTLTextureType3D ? 1 : d.arrayLength) * (cube ? 6 : 1);
    uint64_t offset = 0;
    for (uint64_t level = 0; level < l.levels; level++) {
        l.levelOffset[level] = offset;
        offset += l.rowBytes(level) * l.rows(level) * l.images(level);
    }
    l.sliceBytes = offset;
    return self;
}

- (NSUInteger)size
{
    return _layout.sliceBytes * _layout.slices;
}

- (NSUInteger)alignment
{
    return 16;
}

- (MTLSize)copyGranularity
{
    return MTLSizeMake(_layout.block[1], _layout.block[2], _layout.block[3]);
}

- (NBWatermark)watermark
{
    return NBWatermark{};
}

- (void)initializeTextureMemory:(void *)memory
{
    (void)memory;
}

- (void)finalizeTextureMemory:(void *)memory
{
    (void)memory;
}

- (void)copyFromLinearBytes:(const void *)bytes
               linearOffset:(NSUInteger)offset
          linearBytesPerRow:(NSUInteger)rowStride
        linearBytesPerImage:(NSUInteger)imageStride
            toTextureMemory:(void *)memory
               textureSlice:(NSUInteger)slice
               textureLevel:(NSUInteger)level
              textureRegion:(MTLRegion)region
{
    const uint8_t *from = static_cast<const uint8_t *>(bytes) + offset;
    uint8_t *to = static_cast<uint8_t *>(memory);
    auto copy = [&](uint64_t at, uint64_t linear, uint64_t length) {
        memcpy(to + at, from + linear, length);
    };
    if (!_layout.eachRow(region, slice, level, rowStride, imageStride, copy))
        refusedLayoutCopy(slice, level, region);
}

- (void)copyFromTextureMemory:(const void *)memory
                 textureSlice:(NSUInteger)slice
                 textureLevel:(NSUInteger)level
                textureRegion:(MTLRegion)region
                toLinearBytes:(void *)bytes
                 linearOffset:(NSUInteger)offset
            linearBytesPerRow:(NSUInteger)rowStride
          linearBytesPerImage:(NSUInteger)imageStride
{
    const uint8_t *from = static_cast<const uint8_t *>(memory);
    uint8_t *to = static_cast<uint8_t *>(bytes) + offset;
    auto copy = [&](uint64_t at, uint64_t linear, uint64_t length) {
        memcpy(to + linear, from + at, length);
    };
    if (!_layout.eachRow(region, slice, level, rowStride, imageStride, copy))
        refusedLayoutCopy(slice, level, region);
}

@end

// Creation and archive population must use the same complete supported descriptor.
static bool linkedHandles(NSArray *functions, std::vector<NmbFunction *> &out)
{
    for (id f in functions) {
        if (![f isKindOfClass:[NBFunction class]])
            return false;
        out.push_back([(NBFunction *)f handle]);
    }
    return true;
}

extern "C" bool native_bridge_metal_render_direct(id descriptor, bool barrier, NSUInteger textureFormat,
                                                  NSUInteger textureSamples, const struct NmbDirectBinds *binds,
                                                  bool (*each)(const struct NmbDirectState *, void *), void *ctx,
                                                  const char **why)
{
    NmbRenderPipelineDescriptor p = {};
    std::vector<NmbFunction *> linked;
    NSError *error = nil;
    if ((!gDevice && !gCompile) || !renderDescriptor((MTLRenderPipelineDescriptor *)descriptor, &p, linked, &error)) {
        *why = gDevice || gCompile ? "a function of another device" : "no device";
        return false;
    }
    const uint32_t samples = uint32_t(textureSamples);
    if (gDevice ? nmb_render_pipeline_direct(gDevice, &p, barrier, textureFormat, samples, binds, each, ctx)
                : nmb_compile_render_direct(gCompile, &p, barrier, textureFormat, samples, binds, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

// `linked` holds the linked functions' handles (functions, then privateFunctions) that `p` points into: it outlives the creation call.
static bool computeDescriptor(MTLComputePipelineDescriptor *d, NmbComputePipelineDescriptor *p,
                              std::vector<NmbFunction *> &linked, NSError **error)
{
    if (!d || ![(id)d.computeFunction isKindOfClass:[NBFunction class]]) {
        if (error)
            *error = nbError(@"foreign compute function");
        return false;
    }
    // Metal materializes an empty stage input descriptor on access: no attribute set is none
    if (MTLStageInputOutputDescriptor *stage = d.stageInputDescriptor) {
        for (NSUInteger index = 0; index < 31; ++index) {
            MTLAttributeDescriptor *a = stage.attributes[index];
            if (a.format == MTLAttributeFormatInvalid)
                continue;
            p->stage_inputs = 1;
            p->stage_attributes[index] = {(uint64_t)a.format, a.offset, a.bufferIndex};
            if (a.bufferIndex < 31) {
                MTLBufferLayoutDescriptor *l = stage.layouts[a.bufferIndex];
                p->stage_layouts[a.bufferIndex] = {l.stride, (uint64_t)l.stepFunction, l.stepRate};
            }
        }
        p->stage_index_type = stage.indexType;
        p->stage_index_buffer = stage.indexBufferIndex;
    }
    // Visible functions the kernel calls by name (MPS's ND-array kernels import their prologue and epilogue, prefixPrimary_f and the like,
    // through these); binary functions, call-site groups and dynamic libraries are not represented yet.
    MTLLinkedFunctions *functions = d.linkedFunctions;
    if (functions.binaryFunctions.count || functions.groups.count || d.preloadedLibraries.count ||
        d.insertLibraries.count) {
        if (error)
            *error = nbError(@"compute binary functions, function groups and dynamic libraries are not implemented");
        return false;
    }
    linked.clear();
    if (!linkedHandles(functions.functions, linked)) {
        if (error)
            *error = nbError(@"foreign linked function");
        return false;
    }
    const size_t visible = linked.size();
    if (!linkedHandles(functions.privateFunctions, linked)) {
        if (error)
            *error = nbError(@"foreign linked function");
        return false;
    }
    p->linked = linked.data();
    p->linked_count = visible;
    p->private_linked = linked.data() + visible;
    p->private_count = linked.size() - visible;
    p->function = [(NBFunction *)d.computeFunction handle];
    p->max_threads = d.maxTotalThreadsPerThreadgroup;
    p->multiple_of_execution_width = d.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    p->indirect = d.supportIndirectCommandBuffers;
    p->max_call_stack_depth = d.maxCallStackDepth;
    p->adding_binary_functions = d.supportAddingBinaryFunctions;
    for (NSUInteger index = 0; index < 31; ++index)
        p->mutability[index] = d.buffers[index].mutability;
    return true;
}

extern "C" id native_bridge_metal_compute_prepare(id descriptor, NSUInteger options, id *reflection, NSError **error)
{
    if (reflection)
        *reflection = nil;
    auto *d = (MTLComputePipelineDescriptor *)descriptor;
    if ((options & ~(MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo)) || d.binaryArchives.count ||
        d.supportIndirectCommandBuffers || d.supportAddingBinaryFunctions)
        return nil;
    NmbComputePipelineDescriptor p = {};
    std::vector<NmbFunction *> linked;
    if (!gDevice || !computeDescriptor(d, &p, linked, error))
        return nil;
#if SWITCH_FRAME_TIMELINE
    const uint64_t began = nowUs();
#endif
    const bool reflected = reflection && options;
    native_artifact_cache::Key key = {};
    const bool keyed = nmb_compute_artifact_key(gDevice, &p, reflected, key.data());
    NmbComputeArtifact *handle = keyed ? gNativeArtifactStorage.load(gDevice, p, reflected, key) : nullptr;
    const bool imported = handle != nullptr;
    if (!handle)
        handle = nmb_compute_artifact_new(gDevice, &p, reflected);
    if (!handle)
        return failWith<id>(error, @"native compute artifact", nil);
    NBNativeComputeArtifact *artifact = [NBNativeComputeArtifact new];
    artifact->_artifact = handle;
    artifact->_cacheKey = key;
    artifact->_cacheKeyValid = keyed;
    if (!nmb_compute_artifact_info(handle, &artifact->_info) || artifact->_info.version != 1)
        return failWith<id>(error, @"native compute metadata", nil);
    if (reflected) {
        *reflection = computeReflection(handle);
        if (!*reflection)
            return failWith<id>(error, @"native compute reflection", nil);
    }
    artifact.label = d.label;
#if SWITCH_FRAME_TIMELINE
    artifact.journalState = journalComputePipeline(d, nil, MTLPipelineOption(options), began, nullptr);
#endif
    objc_setAssociatedObject(d, &computeArtifactKey, artifact, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (keyed && !imported)
        saveNativeArtifact(artifact);
    if (error)
        *error = nil;
    return artifact;
}

extern "C" bool native_bridge_metal_compute_direct(id descriptor, const struct NmbDirectDispatch *dispatch,
                                                   bool (*each)(const struct NmbDirectKernel *, void *), void *ctx,
                                                   const char **why)
{
    NBNativeComputeArtifact *artifact = objc_getAssociatedObject(descriptor, &computeArtifactKey);
    if (artifact) {
        if (nmb_compute_artifact_direct(gDevice, artifact->_artifact, dispatch, each, ctx))
            return true;
        *why = nmb_last_error();
        return false;
    }
    NmbComputePipelineDescriptor p = {};
    std::vector<NmbFunction *> linked;
    NSError *error = nil;
    if ((!gDevice && !gCompile) || !computeDescriptor((MTLComputePipelineDescriptor *)descriptor, &p, linked, &error)) {
        *why =
            gDevice || gCompile ? "a function of another device, or what the frontend does not represent" : "no device";
        return false;
    }
    if (gDevice ? nmb_compute_pipeline_direct(gDevice, &p, dispatch, each, ctx)
                : nmb_compile_compute_direct(gCompile, &p, dispatch, each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

extern "C" bool native_bridge_metal_argument_layouts(id function, NSUInteger index,
                                                     bool (*each)(const struct NmbArgumentLayout *, uint32_t, void *),
                                                     void *ctx, const char **why)
{
    if (![function isKindOfClass:[NBFunction class]] || index > UINT32_MAX) {
        *why = "a function of another device";
        return false;
    }
    if (nmb_function_argument_layouts(((NBFunction *)function).handle, uint32_t(index), each, ctx))
        return true;
    *why = nmb_last_error();
    return false;
}

// ---- binary archives ----
@interface NBBinaryArchive : NBObject
@property (nonatomic) NmbBinaryArchive *handle;
@end
@implementation NBBinaryArchive

- (void)dealloc
{
    nmb_binary_archive_release(_handle);
}

- (NSUInteger)options
{
    return 0;
}

- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error
{
    auto *p = new NmbRenderPipelineDescriptor();
    std::vector<NmbFunction *> linked;
    const bool ok = renderDescriptor(d, p, linked, error) && nmb_binary_archive_add_render(_handle, p);
    delete p;
    if (!ok)
        return failWith<BOOL>(error, @"archive render pipeline", NO);
    if (error)
        *error = nil;
    return YES;
}

- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error
{
    NmbComputePipelineDescriptor p = {};
    std::vector<NmbFunction *> linked;
    if (!computeDescriptor(d, &p, linked, error))
        return NO;
    if (!nmb_binary_archive_add_compute_descriptor(_handle, &p))
        return failWith<BOOL>(error, @"archive compute pipeline", NO);
    if (error)
        *error = nil;
    return YES;
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error
{
    if (!url.isFileURL) {
        if (error)
            *error = nbError(@"a file URL is required");
        return NO;
    }
    if (!nmb_binary_archive_serialize(_handle, url.fileSystemRepresentation))
        return failWith<BOOL>(error, @"archive serialize", NO);
    if (error)
        *error = nil;
    return YES;
}

@end

static bool archivesOurs(NSArray *list)
{
    for (id archive in list)
        if (![archive isKindOfClass:[NBBinaryArchive class]])
            return false;
    return true;
}

static bool pipelineArchives(NSArray *list, std::vector<NmbBinaryArchive *> &archives, NSError **error)
{
    for (id archive in list) {
        if (![archive isKindOfClass:[NBBinaryArchive class]]) {
            if (error)
                *error = nbError(@"foreign binary archive");
            return false;
        }
        archives.push_back([(NBBinaryArchive *)archive handle]);
    }
    return true;
}

// Core Image makes its kernels at run time: 21H461's CoreImage imports newLibraryWithSource:options:error: (CIKL and generated kernels,
// CI::MetalMainProgram) and newLibraryWithDescriptorSPI:error: (its stitched DAGs, CI::StitchableFunctionDAGDescriptor; Core Image
// stitches by default, CI_ENABLE_METAL_DAG and CI_ENABLE_FUNCTION_STITCHING), and CoreUI draws every graphic icon (Settings' rows) through
// it. Metal sends both to MTLCompilerService, which has no target for this device. That service's front end, MTLCompiler.framework (in
// the shared cache: MTLCodeGenServiceCreate, MTLCodeGenServiceBuildRequest), answers the same requests in this process with a metallib
// of AIR, which is then loaded as any library. Request types (MTLCompilerObject::buildRequest's jump table): 13 Metal source, 14 a
// stitching DAG. As MaliMetal's frontend does on the same OS build. The reply, to a block called before the request returns, is a header
// whose u32s at 8 and 12 are its own size and that of the warnings after it, then the metallib. The framework is loaded on the first
// request: the render server, whose QuartzCore never compiles source, never loads it.
static std::string
    gCompileCache;  // <the runtime's cache>/msl: the compiler's replies, kept across respawns (empty: none kept)
// <the shared shader cache seed>/msl: the replies the image ships (Core Image's kernels as the device compiled them; the seed's
// merge.py), read in place when this process's own cache lacks one (empty: no seed)
static std::string gCompileSeed;
#if SWITCH_FRAME_TIMELINE
static std::atomic<uint64_t> gCompiles, gCompilesKept, gCompileRefusals;
#endif

// The compiler was written for MTLCompilerService's threads and recurses deeply; a dispatch worker's 512 KiB stack is too small for it
// (MaliMetal: a stack-protector abort inside libGPUCompilerImplLazy). A request runs on a thread of its own with a 16 MiB stack, the
// caller waiting (the asynchronous forms below run on a queue already).
static void *runBlock(void *block)
{
    @autoreleasepool {
        ((__bridge void (^)(void))block)();
    }
    return nullptr;
}

static void onLargeStack(void (^body)(void))
{
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr)) {
        body();
        return;
    }
    const bool made = !pthread_attr_setstacksize(&attr, 16u << 20) &&
                      !pthread_create(&thread, &attr, runBlock, (__bridge void *)body);
    pthread_attr_destroy(&attr);
    if (made)
        pthread_join(thread, nullptr);
    else
        body();
}

// A reply depends only on the request and the compiler, so a compiled one is kept as <gCompileCache>/<SHA-256 of MTLCompiler's LC_UUID,
// the request type and its bytes>.reply: magic, warning and metallib lengths, the SHA-256 of both, then both. A respawned process (the
// icon service is ended whenever it goes idle) reads it instead of compiling again, and a process that never compiled it reads the
// seed's. Written beside its name and renamed; one that does not check out is compiled again; failures are not kept.
struct NBCompileReplyHeader {
    uint32_t magic, warningBytes;
    uint64_t metallibBytes;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
};

static const uint32_t kNBCompileReplyMagic = 0x3143534e;  // "NSC1"

static std::string compileReplyName(const unsigned char uuid[16], uint32_t type, const std::vector<uint8_t> &request)
{
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, uuid, 16);
    CC_SHA256_Update(&context, &type, sizeof type);
    CC_SHA256_Update(&context, request.data(), (CC_LONG)request.size());
    unsigned char key[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(key, &context);
    char name[2 * CC_SHA256_DIGEST_LENGTH + 8];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(name + 2 * i, 3, "%02x", key[i]);
    return std::string(name) + ".reply";
}

static bool compileReplyRead(const std::string &dir, const std::string &name, NSData **metallib, NSString **warnings)
{
    NSData *file = dir.empty() ? nil
                               : [NSData dataWithContentsOfFile:@((dir + "/" + name).c_str())
                                                        options:NSDataReadingMappedIfSafe
                                                          error:nil];
    NBCompileReplyHeader header;
    if (file.length < sizeof header)
        return false;
    memcpy(&header, file.bytes, sizeof header);
    if (header.magic != kNBCompileReplyMagic || !header.metallibBytes ||
        sizeof header + header.warningBytes + header.metallibBytes != file.length)
        return false;
    const uint8_t *payload = (const uint8_t *)file.bytes + sizeof header;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(payload, (CC_LONG)(header.warningBytes + header.metallibBytes), digest);
    if (memcmp(digest, header.digest, sizeof digest))
        return false;
    *warnings = header.warningBytes
                    ? [[NSString alloc] initWithBytes:payload length:header.warningBytes encoding:NSUTF8StringEncoding]
                    : nil;
    *metallib = [NSData dataWithBytes:payload + header.warningBytes length:(NSUInteger)header.metallibBytes];
    return true;
}

static void compileReplyWrite(const std::string &name, NSData *metallib, NSString *warnings,
                              const unsigned char uuid[16], uint32_t type, const std::vector<uint8_t> &request)
{
    static std::mutex writeLock;
    std::lock_guard<std::mutex> guard(writeLock);
    if (gCompileCache.empty() || !metallib.length)
        return;
    const std::string path = gCompileCache + "/" + name;
    NSData *warned = [warnings dataUsingEncoding:NSUTF8StringEncoding];
    NBCompileReplyHeader header = {kNBCompileReplyMagic, (uint32_t)warned.length, metallib.length, {}};
    NSMutableData *file = [NSMutableData dataWithCapacity:sizeof header + warned.length + metallib.length];
    [file appendBytes:&header length:sizeof header];
    if (warned.length)
        [file appendData:warned];
    [file appendData:metallib];
    uint8_t *bytes = (uint8_t *)file.mutableBytes;
    CC_SHA256(bytes + sizeof header, (CC_LONG)(warned.length + metallib.length),
              ((NBCompileReplyHeader *)bytes)->digest);
    mkdir(gCompileCache.c_str(), 0700);
    const std::string temporary = path + "." + std::to_string(getpid()) + ".tmp";
    if (![file writeToFile:@(temporary.c_str()) atomically:NO] || rename(temporary.c_str(), path.c_str())) {
        unlink(temporary.c_str());
        return;
    }
    static const std::string osBuild = [] {
        char value[128] = {};
        size_t bytes = sizeof value;
        return sysctlbyname("kern.osversion", value, &bytes, nullptr, 0) == 0 && bytes && bytes <= sizeof value &&
                       value[bytes - 1] == 0
                   ? std::string(value)
                   : std::string();
    }();
    switch_compile_reply::write(gCompileCache, name, uuid, type, request.data(), request.size(), file, metallib,
                                osBuild, SWITCH_METAL_KIND);
}

typedef void (^NBCodeGenReply)(int32_t status, const void *reply, size_t length, const char *message);

// A kept reply is read before the code generation service is made: a process whose requests the seed or its own cache holds never
// starts the compiler, only loads its framework for the LC_UUID that names the replies.
static NSData *codeGenRequest(uint32_t type, const std::vector<uint8_t> &request, NSString **warnings,
                              NSString **failure)
{
    static void *service;
    static void *(*create)(const char *);
    static void (*build)(void *, void *, uint32_t, const void *, size_t, NBCodeGenReply);
    static unsigned char compilerUuid[16];
    static dispatch_once_t loaded, created;
    static std::mutex lock;
    dispatch_once(&loaded, ^{
        void *library = dlopen(SWITCH_METAL_COMPILER, RTLD_NOW | RTLD_LOCAL);
        create = library ? (void *(*)(const char *))dlsym(library, "MTLCodeGenServiceCreate") : nullptr;
        build = library ? (void (*)(void *, void *, uint32_t, const void *, size_t, NBCodeGenReply))dlsym(
                              library, "MTLCodeGenServiceBuildRequest")
                        : nullptr;
        Dl_info info;
        if (build && dladdr((const void *)build, &info) && info.dli_fbase) {
            const mach_header_64 *header = (const mach_header_64 *)info.dli_fbase;
            const uint8_t *command = (const uint8_t *)(header + 1);
            for (uint32_t i = 0; i < header->ncmds; i++, command += ((const load_command *)command)->cmdsize)
                if (((const load_command *)command)->cmd == LC_UUID) {
                    memcpy(compilerUuid, ((const uuid_command *)command)->uuid, 16);
                    break;
                }
        }
        if (!create || !build)
            NBLOG(@"native-metal: the system's Metal compiler (MTLCompiler.framework) did not load: %s",
                  library ? "no code generation service" : dlerror());
    });
    if (!create || !build) {
        *failure = @"the system's Metal compiler (MTLCompiler.framework) is not available";
        return nil;
    }
    const std::string name = compileReplyName(compilerUuid, type, request);
    NSData *keptMetallib = nil;
    NSString *keptWarnings = nil;
    if (compileReplyRead(gCompileCache, name, &keptMetallib, &keptWarnings) ||
        compileReplyRead(gCompileSeed, name, &keptMetallib, &keptWarnings)) {
        SWITCH_METAL_DIAGNOSTIC(gCompilesKept++);
        if (!gCompileCache.empty() && switch_compile_reply::first(gCompileCache, name))
            compileReplyWrite(name, keptMetallib, keptWarnings, compilerUuid, type, request);
        *warnings = keptWarnings;
        *failure = nil;
        return keptMetallib;
    }
    dispatch_once(&created, ^{
        service = create("native-metal");
        if (!service)
            NBLOG(@"native-metal: the system's Metal compiler (MTLCompiler.framework) made no code generation service");
    });
    if (!service) {
        *failure = @"the system's Metal compiler (MTLCompiler.framework) is not available";
        return nil;
    }
    __block NSData *metallib = nil;
    __block NSString *warned = nil, *message = nil;
    __block bool called = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        onLargeStack(^{
            build(service, nullptr, type, request.data(), request.size(),
                  ^(int32_t status, const void *reply, size_t length, const char *why) {
                      called = true;
                      uint32_t header = 0, warning = 0;
                      if (reply && length >= 16) {
                          memcpy(&header, (const uint8_t *)reply + 8, 4);
                          memcpy(&warning, (const uint8_t *)reply + 12, 4);
                      }
                      if (status || !reply || length < 16 || (uint64_t)header + warning > length) {
                          message = why && *why
                                        ? @(why)
                                        : [NSString stringWithFormat:@"the Metal compiler failed (status %d)", status];
                          return;
                      }
                      if (warning)
                          warned = [[NSString alloc] initWithBytes:(const uint8_t *)reply + header
                                                            length:warning
                                                          encoding:NSUTF8StringEncoding];
                      metallib = [NSData dataWithBytes:(const uint8_t *)reply + header + warning
                                                length:length - header - warning];
                  });
        });
    }
    SWITCH_METAL_DIAGNOSTIC(gCompiles++);
    if (!called)
        message = @"the Metal compiler did not answer";
    *warnings = warned;
    *failure = message;
    if (message) {
        SWITCH_METAL_DIAGNOSTIC(gCompileRefusals++);
        return nil;
    }
    compileReplyWrite(name, metallib, warned, compilerUuid, type, request);
    return metallib;
}

// Metal source, request 13, as 21H461's front end reads it (buildRequest: u32 source bytes at 0, u32 bytes of an extra block at 4, flags
// at 12; the source from 16, padded to 4 bytes; the extra block; the options NUL-terminated): the source NUL-padded to a multiple of 4
// bytes, no extra block, the options' bytes at 8, no flags.
static NSData *compileMetalSource(NSString *source, NSString *arguments, NSString **warnings, NSString **failure)
{
    NSData *text = [source dataUsingEncoding:NSUTF8StringEncoding];
    const char *options = arguments.UTF8String;
    const uint32_t sourceBytes = (uint32_t)((text.length + 1 + 3) & ~(NSUInteger)3),
                   optionBytes = (uint32_t)strlen(options) + 1;
    std::vector<uint8_t> request(16 + sourceBytes + optionBytes, 0);
    const uint32_t head[4] = {sourceBytes, 0, optionBytes, 0};
    memcpy(request.data(), head, sizeof head);
    memcpy(request.data() + 16, text.bytes, text.length);
    memcpy(request.data() + 16 + sourceBytes, options, optionBytes);
    return codeGenRequest(13, request, warnings, failure);
}

// The front end's arguments for a set of compile options, as Metal passes them: the language version (3.1, this OS's, when none is
// given), fast math (Metal's default), invariance, size optimization and the preprocessor macros; module caches beside the replies.
static NSString *compileArgument(NSString *text)
{
    if ([text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@" \t\"\\'"]].location ==
        NSNotFound)
        return text;
    NSString *escaped = [[text stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
        stringByReplacingOccurrencesOfString:@"\""
                                  withString:@"\\\""];
    return [NSString stringWithFormat:@"\"%@\"", escaped];
}

static NSString *compileArguments(MTLCompileOptions *options)
{
    NSMutableArray<NSString *> *arguments =
        [NSMutableArray arrayWithObjects:@"--driver-mode=metal", @"-x", @"metal", nil];
    const NSUInteger version =
        options && options.languageVersion ? (NSUInteger)options.languageVersion : (NSUInteger)MTLLanguageVersion3_1;
    const unsigned major = (unsigned)(version >> 16), minor = (unsigned)(version & 0xffff);
    [arguments
        addObject:[NSString stringWithFormat:major >= 3 ? @"-std=metal%u.%u" : @"-std=ios-metal%u.%u", major, minor]];
    [arguments addObject:!options || options.fastMathEnabled ? @"-ffast-math" : @"-fno-fast-math"];
    if (options.preserveInvariance)
        [arguments addObject:@"-fpreserve-invariance"];
    if (options.optimizationLevel == MTLLibraryOptimizationLevelSize)
        [arguments addObject:@"-Os"];
    [options.preprocessorMacros enumerateKeysAndObjectsUsingBlock:^(NSString *name, NSObject *value, BOOL *) {
        [arguments addObject:compileArgument([NSString stringWithFormat:@"-D%@=%@", name, value])];
    }];
    NSString *modules = gCompileCache.empty() ? [NSTemporaryDirectory() stringByAppendingPathComponent:@"m2v-msl"]
                                              : @((gCompileCache + "/modules").c_str());
    [arguments addObject:compileArgument([@"-fmodules-cache-path=" stringByAppendingString:modules])];
    return [arguments componentsJoinedByString:@" "];
}

// A refusal of a library request this file does not serve, logged for the first few (which of Core Image's paths a process takes).
static void refuseLibrary(NSString *what)
{
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 8)
        NBLOG(@"native-metal: %@ refused (not implemented)", what);
}

// Whether this process compiles Metal source and stitched libraries (NMB_SYSTEM_COMPILER=1, which the plugin sets in every process but the
// render server: its requests stay refused, so it never loads the compiler or waits for one).
static bool systemCompiler()
{
    static const bool on = [] {
        const char *v = getenv("NMB_SYSTEM_COMPILER");
        return v && strcmp(v, "0");
    }();
    return on;
}

// The target's own _MTLDevice method that writes stitching graphs as the DAG text Metal's compiler reads (the device's base class).
@protocol NBOriginalStitching
- (NSString *)newDagStringWithGraphs:(NSArray<MTLFunctionStitchingGraph *> *)graphs;
@end

@protocol NBImageFilterLinking
- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const switch_metal::ImageFilterFunctionInfo *)info
                                    error:(NSError **)error;
@end

// A library the system's Metal compiler made from a request (a stitched DAG, Core Image's filter-function link), loaded as any library.
static id<MTLLibrary> libraryFromCompilerRequest(id<MTLDevice> device, uint32_t type,
                                                 const std::vector<uint8_t> &request, NSString *what, NSError **error)
{
    NSString *warnings = nil, *failure = nil;
    NSData *metallib = codeGenRequest(type, request, &warnings, &failure);
    if (!metallib) {
        NBLOG(@"native-metal: %@: %@", what, [failure substringToIndex:MIN(failure.length, (NSUInteger)300)]);
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{NSLocalizedDescriptionKey : failure}];
        return nil;
    }
    dispatch_data_t data = dispatch_data_create(metallib.bytes, metallib.length, nullptr, ^{
        (void)metallib;
    });
    return [device newLibraryWithData:data error:error];
}

@protocol NBOriginalLimits
- (void *)limits;
@end
#if TARGET_OS_IPHONE
// The base class's limits table (LimitsTable.h) gets each answer of this device's own methods (installAnswers, NBDeviceMethods), so
// the table says what the methods say; a field the device has no answer for stays the base class's 0.
static void fillLimits(id device)
{
    if (![class_getSuperclass(object_getClass(device)) instancesRespondToSelector:@selector(limits)])
        return;
    uint8_t *table = (uint8_t *)[(id<NBOriginalLimits>)device limits];
    if (!table)
        return;
    fillLimitsFromMethods(device, table, [](const LimitField &field, LimitFill how) {
        if (how == LimitFill::Overflow)
            NBREFUSE("limits.overflow", uintptr_t(field.selector), @"limits: %s does not fit the table",
                     field.selector);
        else if (how == LimitFill::NotRead)
            NBREFUSE("limits.layout", field.offset,
                     @"limits: the base class does not read %s at 0x%x; it stays its own", field.selector,
                     field.offset);
    });
    uint32_t width = 0, height = 0;
    memcpy(&width, table + 0x84, 4);
    memcpy(&height, table + 0x88, 4);
    if (!width || !height)
        NBREFUSE("limits.texture2d", 0,
                 @"limits: no 2D texture size from the device (PhotosUICore refuses every image)");
}
#else
static void fillLimits(id)
{}  // the host's _MTLDevice has its own layout
#endif

// Every allocation of this device is resident from its creation to its release (one GPU address space, no paging), so a
// residency set keeps its allocations for the client and asks nothing of the GPU. _MTLDevice's own
// newResidencySetWithDescriptor: asks the driver's allocResidencySet, which the base class answers nil, and returns nil
// with no error.
API_AVAILABLE(ios(18.0), macos(15.0))
@interface NBResidencySet : NBObject <MTLResidencySet>
@end
@implementation NBResidencySet {
    std::mutex _lock;
    NSMutableArray<id<MTLAllocation>> *_pending;
    NSArray<id<MTLAllocation>> *_committed;
}

@dynamic device, label;

- (instancetype)initWithDescriptor:(MTLResidencySetDescriptor *)descriptor
{
    if ((self = [super init])) {
        self.label = descriptor.label;
        _pending = [NSMutableArray arrayWithCapacity:descriptor.initialCapacity];
        _committed = @[];
    }
    return self;
}

- (uint64_t)allocatedSize
{
    std::lock_guard<std::mutex> hold(_lock);
    uint64_t bytes = 0;
    for (id<MTLAllocation> allocation in _committed)
        bytes += allocation.allocatedSize;
    return bytes;
}

- (void)addAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<std::mutex> hold(_lock);
    if (allocation && ![_pending containsObject:allocation])
        [_pending addObject:allocation];
}

- (void)addAllocations:(const id<MTLAllocation> _Nonnull[])allocations count:(NSUInteger)count
{
    for (NSUInteger i = 0; i < count; i++)
        [self addAllocation:allocations[i]];
}

- (void)removeAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<std::mutex> hold(_lock);
    [_pending removeObject:allocation];
}

- (void)removeAllocations:(const id<MTLAllocation> _Nonnull[])allocations count:(NSUInteger)count
{
    for (NSUInteger i = 0; i < count; i++)
        [self removeAllocation:allocations[i]];
}

- (void)removeAllAllocations
{
    std::lock_guard<std::mutex> hold(_lock);
    [_pending removeAllObjects];
}

- (BOOL)containsAllocation:(id<MTLAllocation>)allocation
{
    std::lock_guard<std::mutex> hold(_lock);
    return [_pending containsObject:allocation];
}

- (NSArray<id<MTLAllocation>> *)allAllocations
{
    std::lock_guard<std::mutex> hold(_lock);
    return [_pending copy];
}

- (NSUInteger)allocationCount
{
    std::lock_guard<std::mutex> hold(_lock);
    return _pending.count;
}

- (void)commit
{
    std::lock_guard<std::mutex> hold(_lock);
    _committed = [_pending copy];
}

- (void)requestResidency
{}

- (void)endResidency
{}

@end

static uint64_t askAnswer(SEL cmd, NSUInteger arg);

@interface NBDeviceMethods : NSObject
@end
@implementation NBDeviceMethods

- (NSString *)name
{
    return gName;
}

- (uint64_t)registryID
{
    return gRegistryID;
}

// The kernel driver's largest buffer object (max_bo_bytes, 256 MiB; CONTRACT-kernel-notes.md) bounds a buffer below NVK's
// maxMemoryAllocationSize (2 GiB): MPS and MPSGraph size their tiles and scratch from this answer.
- (NSUInteger)maxBufferLength
{
    return std::min<uint64_t>(nmb_device_max_buffer_length(gDevice), 256ull << 20);
}

- (BOOL)hasUnifiedMemory
{
    return YES;
}

- (BOOL)isLowPower
{
    return NO;
}

- (BOOL)isRemovable
{
    return NO;
}

- (BOOL)isHeadless
{
    return NO;
}

// Unified memory: all of it is shared, none dedicated (Core Image's CIMetalDeviceGetLimits sizes its caches from these).
- (uint64_t)sharedMemorySize
{
    return NSProcessInfo.processInfo.physicalMemory;
}

- (uint64_t)dedicatedMemorySize
{
    return 0;
}

// A no-copy buffer wraps whole pages (nmb_buffer_new_no_copy), the kernel's 4 KiB: the commpage's page size (no system call; getpagesize()
// is a sysctl).
- (NSUInteger)minBufferNoCopyAlignmentBytes
{
    return (NSUInteger)vm_page_size;
}

// MPS sizes its threadgroup memory from this private total (its MPSDevice reads it unguarded); the runtime has no answer of its own
// (Metal's is Apple's), so it is the public length the runtime enforces, never more than a dispatch can take.
- (NSUInteger)maxComputeThreadgroupMemory
{
    return [(id<MTLDevice>)self maxThreadgroupMemoryLength];
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    uint64_t a[2] = {};
    if (!nmb_device_heap_buffer_size(gDevice, length, options, a))
        NBLOG(@"native-metal: heapBufferSizeAndAlign: %@", lastError());
    return {a[0], a[1]};
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
    NmbTextureDescriptor descriptor;
    uint64_t a[2] = {};
    if (!textureDescriptor(d, &descriptor) || !nmb_device_heap_texture_size(gDevice, &descriptor, a))
        NBLOG(@"native-metal: heapTextureSizeAndAlign: %@", lastError());
    return {a[0], a[1]};
}

// Metal source and stitched libraries are compiled by the system's front end in this process (codeGenRequest, above); the base class
// would send them to Apple's compiler service, which has no target for this device. Core Image's filter-function linking and the DAG form
// stay refused.
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    if (!systemCompiler()) {
        if (error)
            *error = nbError(@"Metal source is not compiled here (AIR libraries only)");
        return nil;
    }
    if (!source) {
        if (error)
            *error = nbError(@"no source");
        return nil;
    }
    if (options && options.libraryType == MTLLibraryTypeDynamic) {
        if (error)
            *error = nbError(@"dynamic libraries are not supported");
        return nil;
    }
    NSString *warnings = nil, *failure = nil;
    NSData *metallib = compileMetalSource(source, compileArguments(options), &warnings, &failure);
    if (!metallib) {
        NBLOG(@"native-metal: newLibraryWithSource: %@",
              [failure substringToIndex:MIN(failure.length, (NSUInteger)300)]);
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorCompileFailure
                                     userInfo:@{NSLocalizedDescriptionKey : failure}];
        return nil;
    }
    dispatch_data_t data = dispatch_data_create(metallib.bytes, metallib.length, nullptr, ^{
        (void)metallib;
    });
    id<MTLLibrary> library = [(id<MTLDevice>)self newLibraryWithData:data error:error];
    // As Metal does: a library compiled with warnings comes with them as a CompileWarning error.
    if (library && warnings.length && error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorCompileWarning
                                 userInfo:@{NSLocalizedDescriptionKey : warnings}];
    return library;
}

- (void)newLibraryWithSource:(NSString *)source
                     options:(MTLCompileOptions *)options
           completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        id<MTLLibrary> library = [(id<MTLDevice>)self newLibraryWithSource:source options:options error:&error];
        if (handler)
            handler(library, error);
    });
}

// A stitched library, request 14, as 21H461's front end reads it (MTLCompilerObject::stitchFunctionDag): 'dag ' and the graphs' DAG
// text NUL-terminated, 'numf' and the u32 count of functions, then for each function 'bitc', its u32 size and its wrapped bitcode. A
// newer front end (macOS 27's, the host instrument's) also reads 'nums', 'ptfm' and 'arid' words after them and aborts without them:
// they are sent as zeros, which 21H461's never reads. Core Image's SPI descriptor (newLibraryWithDescriptorSPI:, MTLStitchedLibraryDescriptorSPI, its graphs with SPI
// texture and builtin nodes) is the same class family; the base class's newDagStringWithGraphs: writes its DAG text.
- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    if (!systemCompiler()) {
        if (error)
            *error = nbError(@"function stitching is not implemented");
        return nil;
    }
    if (![d respondsToSelector:@selector(functionGraphs)] || ![d respondsToSelector:@selector(functions)]) {
        if (error)
            *error = nbError(@"not a stitched library descriptor");
        return nil;
    }
    NSArray<MTLFunctionStitchingGraph *> *graphs = d.functionGraphs;
    NSArray<id<MTLFunction>> *functions = d.functions;
    if (!graphs.count || !functions.count) {
        if (error)
            *error = nbError(@"a stitched library needs graphs and functions");
        return nil;
    }
    if (![self respondsToSelector:@selector(newDagStringWithGraphs:)]) {
        if (error)
            *error = nbError(@"no DAG writer in the base device");
        return nil;
    }
    NSString *dag = [(id<NBOriginalStitching>)self newDagStringWithGraphs:graphs];
    if (!dag.length) {
        if (error)
            *error = nbError(@"the stitching graphs have no DAG text");
        return nil;
    }
    std::vector<uint8_t> request;
    auto word = [&](uint32_t v) {
        const uint8_t *b = (const uint8_t *)&v;
        request.insert(request.end(), b, b + 4);
    };
    const char *text = dag.UTF8String;
    word(0x64616720 /* 'dag ' */);
    request.insert(request.end(), text, text + strlen(text) + 1);
    word(0x6e756d66 /* 'numf' */);
    word((uint32_t)functions.count);
    for (id function in functions) {
        NSData *bitcode = [function isKindOfClass:[NBFunction class]] ? functionBitcode((NBFunction *)function) : nil;
        if (!bitcode) {
            if (error)
                *error =
                    nbError([NSString stringWithFormat:@"stitched function %@ has no bitcode here", [function name]]);
            return nil;
        }
        word(0x62697463 /* 'bitc' */);
        word((uint32_t)bitcode.length);
        const uint8_t *b = (const uint8_t *)bitcode.bytes;
        request.insert(request.end(), b, b + bitcode.length);
    }
    word(0x6e756d73 /* 'nums' */);
    word(0);
    word(0x7074666d /* 'ptfm' */);
    word(0);
    word(0x61726964 /* 'arid' */);
    word(0);
    return libraryFromCompilerRequest((id<MTLDevice>)self, 14, request, @"stitched library", error);
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d
                          destinationBinaryArchive:(id)archive
                                             error:(NSError **)error
{
    (void)archive;
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}

- (void)newLibraryWithStitchedDescriptor:(MTLStitchedLibraryDescriptor *)d
                       completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        id<MTLLibrary> library = [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:&error];
        if (handler)
            handler(library, error);
    });
}

// Core Image's stitched DAGs (CreateStitchedFunctionFromDescriptor); the base class forwards these two to the destinationBinaryArchive:
// form, answered above. Made here so the route does not depend on it.
- (id<MTLLibrary>)newLibraryWithDescriptorSPI:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}

- (id<MTLLibrary>)newLibraryWithStitchedDescriptorSPI:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    return [(id<MTLDevice>)self newLibraryWithStitchedDescriptor:d error:error];
}

- (id<MTLLibrary>)newLibraryWithDAG:(id)dag functions:(NSArray *)functions error:(NSError **)error
{
    (void)dag;
    (void)functions;
    refuseLibrary(@"function stitching (DAG)");
    if (error)
        *error = nbError(@"function stitching (DAG) is not implemented");
    return nil;
}

// Core Image's legacy DAG (CI::LegacyDAGDescriptor, CreateFunctionFromDAG) and the constants of a kernel made with function constants
// from a library's unstitchable function (CIKernelReflection::reflectConstants, which links the function to itself and reads
// ciKernelMain's). _MTLDevice sends the first to the second.
- (id<MTLLibrary>)newLibraryWithImageFilterFunctionsSPI:(NSArray *)functions
                                imageFilterFunctionInfo:(const switch_metal::ImageFilterFunctionInfo *)info
                                                  error:(NSError **)error
{
    return [(id<NBImageFilterLinking>)self newLibraryWithCIFilters:functions imageFilterFunctionInfo:info error:error];
}

- (id<MTLLibrary>)newLibraryWithCIFilters:(NSArray *)functions
                  imageFilterFunctionInfo:(const switch_metal::ImageFilterFunctionInfo *)info
                                    error:(NSError **)error
{
    if (!systemCompiler()) {
        refuseLibrary(@"Core Image filter-function linking");
        if (error)
            *error = nbError(
                @"Core Image filter-function linking needs the system compiler, which this process does not load");
        return nil;
    }
    if (!functions.count || !info) {
        if (error)
            *error = nbError(@"Core Image filter-function linking needs functions and their infos");
        return nil;
    }
    std::vector<switch_metal::ImageFilterFunction> linked(functions.count);
    std::vector<NSData *> bitcodes;
    for (NSUInteger i = 0; i < functions.count; i++) {
        id function = functions[i];
        NSData *bitcode = [function isKindOfClass:[NBFunction class]] ? functionBitcode((NBFunction *)function) : nil;
        if (!bitcode) {
            if (error)
                *error =
                    nbError([NSString stringWithFormat:@"filter function %@ has no bitcode here", [function name]]);
            return nil;
        }
        bitcodes.push_back(bitcode);
        switch_metal::ImageFilterFunction &f = linked[i];
        f.name = [[function name] UTF8String] ?: "";
        f.bitcode = (const uint8_t *)bitcode.bytes;
        f.bitcodeBytes = bitcode.length;
        f.earlyOut = info[i].earlyOut;
        if (info[i].argumentCount && info[i].arguments)
            f.arguments.assign(info[i].arguments, info[i].arguments + info[i].argumentCount);
    }
    const std::vector<uint8_t> request = switch_metal::imageFilterLinkRequest(linked);
    if (request.empty()) {
        if (error)
            *error = nbError(@"the filter functions do not fit one link request");
        return nil;
    }
    return libraryFromCompilerRequest((id<MTLDevice>)self, switch_metal::kImageFilterLinkRequest, request,
                                      @"Core Image filter-function link", error);
}

- (BOOL)supportsDynamicLibraries
{
    return NO;
}

// A pipeline library (RenderBox's default.pipelinelib; TextInputUI, PencilKit, SiriUICore and others have their own) is pipelines.desc, the
// functions and pipelines.<target>: one Apple GPU's compiled pipelines. The base class's MTLPipelineLibraryBuilder::newLibraryWithFile names
// that file from -targetDeviceInfo, which _MTLDevice answers NULL (only a device that does not respond to it gets a stand-in), and reads
// NULL+0x10 at Metal+0x52b64 (from RB::Device::render_pipeline_state). A library holds
// no pipelines for this GPU, so it is refused as Metal refuses one without the device's target file; RenderBox and the others then build
// their pipelines from their own metallib.
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
    // Not a failure the caller waits on or retries: Metal's own answer for a GPU its library has no pipelines of, after which
    // RenderBox, PencilKit and the rest make their states from their metallib at once (once per library and process).
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 8)
        NBLOG(@"native-metal: pipeline library %@: none for this GPU (file not found, as Metal answers; the caller "
              @"compiles its metallib)",
              path.lastPathComponent);
    if (error)
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorFileNotFound
                                 userInfo:@{
                                     NSLocalizedDescriptionKey :
                                         @"a pipeline library holds Apple GPUs' pipelines, none for this device"
                                 }];
    return nil;
}

// A buffer over a surface's pages (Espresso, MPSCore and MPSGraph pass IOSurface-backed tensors this way), the import textures use.
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)surface
{
    if (!surface || IOSurfaceGetPlaneCount(surface))
        return nil;
    void *base = IOSurfaceGetBaseAddress(surface);
    const size_t page = vm_page_size, allocation = IOSurfaceGetAllocSize(surface);
    if (!base || ((uintptr_t)base & (page - 1)) || !allocation || allocation > SIZE_MAX - (page - 1))
        return nil;
    NBBuffer *buffer = surfaceBuffer(surface, base, (allocation + page - 1) & ~(page - 1), surfaceShape(surface));
    if (buffer && buffer.layout.layout != surface_layout::kPitch) {
        refuseLayout(surface, "a buffer", buffer.layout.layout);
        return nil;
    }
    if (buffer)
        declareGpuWrites(surface);
    return (id<MTLBuffer>)buffer;
}

// supportsFamily: and the other queries the runtime answers are installed from it (installAnswers); supportsFamily: also answers the target
// hardware's families (the bridge's TARGET_FAMILIES, journaled with their waived rows as F lines). No legacy feature set is claimed: the
// base implementation reads Apple tables this device does not have.
// The legacy feature sets of the same families (iOS GPU families 1 to 3, every version this OS defines), so a framework that still asks
// the old question (MPS's and Core Image's device checks) gets the answer supportsFamily: gives; the base implementation would read
// Apple tables this device does not have. Family 4 and up (imageblocks, tile shaders, raster order groups) are not claimed.
- (BOOL)supportsFeatureSet:(MTLFeatureSet)set
{
    switch ((NSUInteger)set) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 12:
    case 13:
    case 14:
        return YES;
    default:
        return NO;
    }
}

// The layout Metal gives these descriptors (the runtime's ArgumentBufferLayout); a data type or alignment Metal raises at is refused.
- (id<MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments
{
    std::vector<NmbArgumentDescriptor> records;
    for (MTLArgumentDescriptor *a in arguments)
        records.push_back({(uint64_t)a.dataType, a.index, a.arrayLength, (uint64_t)a.access, (uint64_t)a.textureType,
                           a.constantBlockAlignment});
    return [NBArgumentEncoder
        with:records.empty() ? nullptr : nmb_device_new_argument_encoder(gDevice, records.data(), records.size())];
}

// The original discovery (_MTLAddDevice) runs these four on every device it registers. The target's own versions fill the base class's limits
// and feature tables from a hardware profile (`featureProfile`, `indirectArgumentBufferCapabilities`, a table of Apple GPUs) that this device
// has not got: running them would claim an Apple GPU's capabilities. Here the limits table gets this device's own answers instead (fillLimits,
// also run when the device opens) and the other three are empty, so a feature stays the base class's neutral NO and a limit the device has no
// answer for its 0. Nothing is claimed that nothing measured.
- (void)initLimits
{
    fillLimits(self);
}

- (void)initFeatureQueries
{}

- (void)initGPUFamilySupport
{}

- (void)initWorkarounds
{}

- (NSString *)productName
{
    return gName;
}

- (NSString *)vendorName
{
    char name[64] = {};
    nmb_device_vendor_name(gDevice, name, sizeof name);
    return [NSString stringWithUTF8String:name];
}

- (MTLSize)maxThreadsPerThreadgroup
{
    uint64_t s[3] = {};
    if (!nmb_device_max_threads_per_threadgroup(gDevice, s))
        NBLOG(@"native-metal: maxThreadsPerThreadgroup: %@", lastError());
    return MTLSizeMake(s[0], s[1], s[2]);
}

- (void)doesNotRecognizeSelector:(SEL)selector
{
    if (gMissing)
        gMissing("NativeBridgeMetalDevice", sel_getName(selector));
    noteProblem([NSString stringWithFormat:@"missing device %s", sel_getName(selector)]);
    [super doesNotRecognizeSelector:selector];
}

- (id<MTLEvent>)newEvent
{
    NmbEvent *handle = nmb_event_new(gDevice, false);
    if (!handle)
        return nil;
    NBEvent *event = [NBEvent new];
    event.handle = handle;
    return (id<MTLEvent>)event;
}

// _MTLDevice's own versions of these make Metal's generic _MTLSharedEvent, which this device's command buffers cannot signal
// or wait on. Only an in-process event with no options is this device's to make; QuartzCore takes nil from
// newSharedEventWithOptions: as the absent feature and keeps its own path.
- (id<MTLSharedEvent>)newSharedEventWithOptions:(NSInteger)options
{
    if (!options)
        return [self newSharedEvent];
    NBREFUSE("sharedevent.options", uint64_t(options), @"newSharedEventWithOptions:%ld: no shared event with options",
             (long)options);
    return nil;
}

- (id<MTLSharedEvent>)newSharedEventWithHandle:(MTLSharedEventHandle *)handle
{
    (void)handle;
    NBREFUSE("sharedevent.handle", 0, @"newSharedEventWithHandle:: no cross-process shared events");
    return nil;
}

- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)port
{
    (void)port;
    NBREFUSE("sharedevent.handle", 1, @"newSharedEventWithMachPort:: no cross-process shared events");
    return nil;
}

- (id<MTLResidencySet>)newResidencySetWithDescriptor:(MTLResidencySetDescriptor *)descriptor
                                               error:(NSError **)error API_AVAILABLE(ios(18.0), macos(15.0))
{
    if (error)
        *error = nil;
    return [[NBResidencySet alloc] initWithDescriptor:descriptor];
}

// A texture buffer is a linear texture over its buffer: the runtime places both at the same alignment.
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)format
{
    return (NSUInteger)askAnswer(@selector(minimumLinearTextureAlignmentForPixelFormat:), format);
}

- (NSUInteger)currentAllocatedSize
{
    uint64_t memory[3] = {};
    return gDevice && nmb_device_memory(gDevice, memory) ? memory[0] : 0;
}

// This device's memory is charged to the process that maps it (its jetsam footprint), so what it can still hold without
// paging is what it holds now and what the process may still take.
- (uint64_t)recommendedMaxWorkingSetSize
{
#if TARGET_OS_IPHONE
    return [self currentAllocatedSize] + os_proc_available_memory();
#else
    return NSProcessInfo.processInfo.physicalMemory;
#endif
}

- (id<MTLSharedEvent>)newSharedEvent
{
    NmbEvent *handle = nmb_event_new(gDevice, true);
    if (!handle)
        return nil;
    NBSharedEvent *event = [NBSharedEvent new];
    event.handle = handle;
    return (id<MTLSharedEvent>)event;
}

- (id<MTLFence>)newFence
{
    NmbFence *handle = nmb_fence_new(gDevice);
    if (!handle)
        return nil;
    NBFence *fence = [NBFence new];
    fence.handle = handle;
    return (id<MTLFence>)fence;
}

- (id<MTLCommandQueue>)newCommandQueue
{
    NmbQueue *handle = nmb_queue_new(gDevice);
    if (!handle)
        return nil;
    NBQueue *queue = [NBQueue new];
    queue.handle = handle;
    addQueuePriority(queue);
    return (id<MTLCommandQueue>)queue;
}

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)d
{
    if (!d)
        return nil;
    NmbHeap *handle = nmb_heap_new(gDevice, d.type, d.size, (uint64_t)d.resourceOptions);
    if (!handle) {
        NBLOG(@"native-metal: heap refused: %@", lastError());
        return nil;
    }
    NBHeap *heap = [NBHeap new];
    heap.handle = handle;
    heap.options = d.resourceOptions;
    heap.heapType = d.type;
    return (id<MTLHeap>)heap;
}

- (id)newResourceGroupFromResources:(const id __unsafe_unretained[])resources count:(NSUInteger)count
{
    NBResourceGroup *group = [NBResourceGroup new];
    NSMutableArray *held = [NSMutableArray arrayWithCapacity:count];
    for (NSUInteger i = 0; i < count; i++)
        if (resources[i])
            [held addObject:resources[i]];
    group.resources = held;
    return group;
}

// Not made here: a texture over raw memory in Apple's layout, refused with the reason rather than answered with a guessed
// shape. Texture layouts (NBTextureLayout) are answered only for memory the CPU alone reads, not for a heap or buffer the GPU
// would read the texture from.
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes
                                     length:(NSUInteger)length
                                 descriptor:(MTLTextureDescriptor *)d
                                deallocator:(void (^)(void *, NSUInteger))deallocator
{
    (void)bytes;
    (void)length;
    (void)d;
    (void)deallocator;
    NBREFUSE("texture.bytesnocopy", 0,
             @"newTextureWithBytesNoCopy:length:descriptor:deallocator: is not implemented (Apple's texture layout of "
             @"raw memory is not known)");
    return nil;
}

- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed
{
    NSString *why = nil;
    uint64_t block[4];
    if (backed)
        why = @"heap or buffer backed (the GPU would read it in Apple's layout, which is not known)";
    else if (d.sampleCount > 1 || d.textureType == MTLTextureType2DMultisample ||
             d.textureType == MTLTextureType2DMultisampleArray || d.textureType == MTLTextureTypeTextureBuffer)
        why = [NSString stringWithFormat:@"texture type %lu with %lu samples", (unsigned long)d.textureType,
                                         (unsigned long)d.sampleCount];
    else if (!d.mipmapLevelCount || d.mipmapLevelCount > kLayoutMaxLevels || !d.width || !d.height || !d.depth ||
             !d.arrayLength)
        why = [NSString stringWithFormat:@"%lux%lux%lu, %lu levels, %lu slices", (unsigned long)d.width,
                                         (unsigned long)d.height, (unsigned long)d.depth,
                                         (unsigned long)d.mipmapLevelCount, (unsigned long)d.arrayLength];
    else if (!nmb_device_pixel_format_block(gDevice, d.pixelFormat, block))
        why = lastError();
    if (why) {
        NBLOG(@"native-metal: newTextureLayoutWithDescriptor:isHeapOrBufferBacked: refused: %@", why);
        return nil;
    }
    return [[NBTextureLayout alloc] initWithDescriptor:d block:block];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)count
{
    (void)count;
    return [(id<MTLDevice>)self newCommandQueue];
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
    NmbBuffer *handle = nmb_buffer_new(gDevice, length, options, nullptr);
    if (!handle) {
        NBLOG(@"native-metal: buffer refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    return (id<MTLBuffer>)buffer;
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
{
    if (!bytes)
        return nil;
    NmbBuffer *handle = nmb_buffer_new(gDevice, length, options, bytes);
    if (!handle) {
        NBLOG(@"native-metal: buffer refused: %@", lastError());
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    return (id<MTLBuffer>)buffer;
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *, NSUInteger))deallocator
{
    NmbBuffer *handle = nmb_buffer_new_no_copy(gDevice, pointer, length, options);
    if (!handle) {
        // Each refusal sends the caller to a CPU copy (QuartzCore's copy_image_to_texture uploads the image instead), so the
        // line names the memory: where it sits in its page, and the VM region's protection, sharing and tag.
        static constexpr uint64_t kRefusal = switch_refusal_key("frontend", "buffer.nocopy");
        if (switch_refusal_first(kRefusal)) {
            vm_address_t region = (vm_address_t)pointer;
            vm_size_t regionSize = 0;
            vm_region_extended_info_data_t info = {};
            mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
            mach_port_t object = MACH_PORT_NULL;
            const kern_return_t kr = vm_region_64(mach_task_self(), &region, &regionSize, VM_REGION_EXTENDED_INFO,
                                                  (vm_region_info_t)&info, &count, &object);
            NSString *text = [NSString
                stringWithFormat:@"%lu bytes at page offset 0x%lx (region %d: prot %d share %d tag %u): %@",
                                 (unsigned long)length, (unsigned long)((uintptr_t)pointer & (vm_page_size - 1)), kr,
                                 info.protection, info.share_mode, info.user_tag, lastError()];
            switch_refusal_log("frontend", "buffer.nocopy", text.UTF8String);
        }
        return nil;
    }
    NBBuffer *buffer = [NBBuffer new];
    buffer.handle = handle;
    buffer.options = options;
    buffer.wrapped = pointer;
    buffer.deallocator = deallocator;
    return (id<MTLBuffer>)buffer;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    NmbTextureDescriptor descriptor;
    if (!textureDescriptor(d, &descriptor))
        return nil;
    const bool wide = heldAsRGBA8(d);
    if (wide)
        descriptor.format = MTLPixelFormatRGBA8Unorm_sRGB;
    NmbTexture *handle = nmb_texture_new(gDevice, &descriptor);
    SWITCH_METAL_DIAGNOSTIC(noteFormat((uint32_t)descriptor.format, handle != nullptr));
    if (!handle) {
        NBLOG(@"native-metal: texture refused: %@", lastError());
        return nil;
    }
    NBTexture *texture = [NBTexture new];
    texture.handle = handle;
    nmb_texture_describe(handle, &descriptor);
    if (wide) {
        descriptor.format = 31;  // RG8Unorm_sRGB, what the program asked for, and reads back
        texture.rgWide = true;
    }
    texture.descriptor = descriptor;
    return (id<MTLTexture>)texture;
}

// The surface's pages are the texels: its mapping in this process is imported without a copy (pinned by the kernel, mapped into the GPU's
// 4 KiB context by Turnip), so every process that imports the surface renders and samples the same memory.
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
                                 iosurface:(IOSurfaceRef)surface
                                     plane:(NSUInteger)plane
{
    if (surface && d && (plane || IOSurfaceGetPlaneCount(surface))) {
        // Every planar surface goes to the runtime with its facts (planarSurfaceTexture), which makes what it holds exactly and
        // refuses the rest (logged once per reason there); planar= counts the planar textures asked for.
        SWITCH_METAL_DIAGNOSTIC(gSurfaceMultiPlane++);
        return (id<MTLTexture>)planarSurfaceTexture(d, surface, plane);
    }
    if (!surface || !d)
        return nil;
    void *base = IOSurfaceGetBaseAddress(surface);
    const size_t page = vm_page_size, allocation = IOSurfaceGetAllocSize(surface);
    if (!page || (page & (page - 1)) || !base || ((uintptr_t)base & (page - 1)) || !allocation ||
        allocation > SIZE_MAX - (page - 1))
        return nil;
    const size_t bytes = (allocation + page - 1) & ~(page - 1), row = IOSurfaceGetBytesPerRow(surface);
    // A single 2D image of the surface asks the layout with its shape and use; another texture the surface's own, sampled
    surface_layout::Shape shape = surfaceShape(surface);
    const bool image =
        d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 && d.sampleCount == 1 && d.arrayLength == 1 &&
        surface_layout::shapeOf(gDevice, d.pixelFormat, d.width, d.height, row, usageWrites(d.usage), shape);
    NBBuffer *buffer = surfaceBuffer(surface, base, bytes, shape);
    if (!buffer)
        return nil;
    const uint32_t layout = surface_layout::layoutFor(surface_layout::nvk(nvkSymbol), buffer.layout, shape);
    NBTexture *texture = nil;
    if (layout == surface_layout::kPitch)
        texture = textureOnBuffer(buffer, d, 0, row, true);
    else if (layout == surface_layout::kTiled && image)
        texture = tiledTextureOnBuffer(buffer, d, 0);
    else
        refuseLayout(surface, "a texture", layout);
    if (!texture)
        return nil;
    texture.surface = (IOSurfaceRef)CFRetain(surface);
    if (usageWrites(d.usage))
        textureGpuWrites(texture);
    return (id<MTLTexture>)texture;
}

// A library of a metallib's AIR slice, the one this IPSW's Metal takes (MetallibSlice.h): its functions, and the bytes their
// bitcode and extern entries are read from. `storage` keeps the file's bytes.
static NBLibrary *metallibLibrary(id storage, const uint8_t *file, size_t length, NSError **error,
                                  bool compileOnly = false)
{
    size_t offset = 0, size = 0;
    if (!switch_metal::airSlice(file, length, offset, size)) {
        if (error)
            *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                         code:MTLLibraryErrorUnsupported
                                     userInfo:@{
                                         NSLocalizedDescriptionKey :
                                             @"This library does not have a compatible AIR slice for this device"
                                     }];
        return nil;
    }
    SWITCH_METAL_DIAGNOSTIC(const uint64_t libraryBegan = nowUs());
    NmbLibrary *handle = compileOnly ? nmb_compile_library_new(file + offset, size, false)
                                     : nmb_library_new(gDevice, file + offset, size, false);
    SWITCH_METAL_DIAGNOSTIC(gTimeLibrary.add(libraryBegan));
    if (!handle)
        return failWith<NBLibrary *>(error, @"library", nil);
    NBLibrary *library = [NBLibrary new];
    library.handle = handle;
    library.storage = storage;
    library.bytes = file + offset;
    library.length = size;
    if (error)
        *error = nil;
    return library;
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    const void *bytes = nullptr;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &size);
    NBLibrary *library = metallibLibrary(mapped, (const uint8_t *)bytes, size, error);
    SWITCH_METAL_DIAGNOSTIC(if (library) library.journalId = libraryJournalId(bytes, size));
    return (id<MTLLibrary>)library;
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    NSData *bytes = path ? [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error] : nil;
    if (!bytes)
        return nil;
    NBLibrary *library = metallibLibrary(bytes, (const uint8_t *)bytes.bytes, bytes.length, error);
    if (library && journalOn())
        library.journalId = journalToken(path.UTF8String);
    return (id<MTLLibrary>)library;
}

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    if (!url.isFileURL) {
        if (error)
            *error = nbError(@"a file URL is required");
        return nil;
    }
    return [(id<MTLDevice>)self newLibraryWithFile:url.path error:error];
}

- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    if (!url) {
        if (error)
            *error = nbError(@"the bundle has no default.metallib");
        return nil;
    }
    return [(id<MTLDevice>)self newLibraryWithURL:url error:error];
}

- (id<MTLLibrary>)newDefaultLibrary
{
    return [(id<MTLDevice>)self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:nil];
}

// The asynchronous forms make the state on a worker and call back there (asyncCall; Core Image's, Espresso's): the
// base class would queue a request to Apple's compiler service. The descriptor is copied at the call, as Metal does.
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                          completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler
{
    asyncCall(^{
        NSError *error = nil;
        id<MTLComputePipelineState> state = [(id<MTLDevice>)self newComputePipelineStateWithFunction:function
                                                                                               error:&error];
        handler(state, error);
    });
}

- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                      options:(MTLPipelineOption)options
                            completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
    MTLComputePipelineDescriptor *copy = [d copy];
    asyncCall(^{
        NSError *error = nil;
        id<MTLComputePipelineState> state = [(id<MTLDevice>)self newComputePipelineStateWithDescriptor:copy
                                                                                               options:options
                                                                                            reflection:nil
                                                                                                 error:&error];
        handler(state, nil, error);
    });
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                           completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    asyncCall(^{
        NSError *error = nil;
        id<MTLRenderPipelineState> state = [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:copy error:&error];
        handler(state, error);
    });
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                     options:(MTLPipelineOption)options
                           completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
    MTLRenderPipelineDescriptor *copy = [d copy];
    asyncCall(^{
        NSError *error = nil;
        id<MTLRenderPipelineState> state = [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:copy
                                                                                             options:options
                                                                                          reflection:nil
                                                                                               error:&error];
        handler(state, nil, error);
    });
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error
{
    return [(id<MTLDevice>)self newComputePipelineStateWithFunction:function
                                                            options:MTLPipelineOptionNone
                                                         reflection:nil
                                                              error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                                           options:(MTLPipelineOption)options
                                                        reflection:
                                                            (MTLAutoreleasedComputePipelineReflection *)reflection
                                                             error:(NSError **)error
{
    if (reflection)
        *reflection = nil;
    if (![(id)function isKindOfClass:[NBFunction class]]) {
        if (error)
            *error = nbError(@"foreign function");
        return nil;
    }
    const NmbPipelineOptions creation = {(uint64_t)options, nullptr, 0};
    SWITCH_METAL_DIAGNOSTIC(const uint64_t pipelineBegan = nowUs());
    NmbComputePipeline *handle =
        nmb_compute_pipeline_new_with_options(gDevice, [(NBFunction *)function handle], &creation);
    SWITCH_METAL_DIAGNOSTIC(gTimePipeline.add(pipelineBegan));
    SWITCH_METAL_DIAGNOSTIC(std::string journaled =
                                journalComputePipeline(nil, (NBFunction *)function, options, pipelineBegan,
                                                       handle ? nullptr : nmb_last_error()));
    if (!handle)
        return failWith<id<MTLComputePipelineState>>(error, @"compute pipeline", nil);
    NBComputePipeline *state = [NBComputePipeline new];
    state.handle = handle;
    SWITCH_METAL_DIAGNOSTIC(state.journalState = journaled);
    SWITCH_METAL_DIAGNOSTIC(gPipelines++);
    if (error)
        *error = nil;
    return (id<MTLComputePipelineState>)state;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
                                                             options:(MTLPipelineOption)options
                                                          reflection:
                                                              (MTLAutoreleasedComputePipelineReflection *)reflection
                                                               error:(NSError **)error
{
    if (reflection)
        *reflection = nil;
    NmbComputePipelineDescriptor p = {};
    std::vector<NmbFunction *> linked;
    if (!computeDescriptor(d, &p, linked, error))
        return nil;
    std::vector<NmbBinaryArchive *> archives;
    if (!pipelineArchives(d.binaryArchives, archives, error))
        return nil;
    const NmbPipelineOptions creation = {(uint64_t)options, archives.data(), archives.size()};
    SWITCH_METAL_DIAGNOSTIC(const uint64_t pipelineBegan = nowUs());
    NmbComputePipeline *handle = nmb_compute_pipeline_new_with_descriptor(gDevice, &p, &creation);
    SWITCH_METAL_DIAGNOSTIC(gTimePipeline.add(pipelineBegan));
    SWITCH_METAL_DIAGNOSTIC(std::string journaled = journalComputePipeline(d, nil, options, pipelineBegan,
                                                                           handle ? nullptr : nmb_last_error()));
    if (!handle)
        return failWith<id<MTLComputePipelineState>>(error, @"compute pipeline", nil);
    NBComputePipeline *state = [NBComputePipeline new];
    state.handle = handle;
    SWITCH_METAL_DIAGNOSTIC(state.journalState = journaled);
    state.label = d.label;
    SWITCH_METAL_DIAGNOSTIC(gPipelines++);
    if (error)
        *error = nil;
    return (id<MTLComputePipelineState>)state;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                             error:(NSError **)error
{
    return [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:d
                                                             options:MTLPipelineOptionNone
                                                          reflection:nil
                                                               error:error];
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
                                                           options:(MTLPipelineOption)options
                                                        reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
                                                             error:(NSError **)error
{
    if (reflection)
        *reflection = nil;
    // The render server's states are made on a worker (AsyncPipelines.h), the call returning at once.
    if (asyncRenderPipelineWanted(d, options)) {
        if (error)
            *error = nil;
        return asyncRenderPipeline(self, d, options);
    }
    auto *p = new NmbRenderPipelineDescriptor();
    std::vector<NmbFunction *> linked;
    if (!renderDescriptor(d, p, linked, error)) {
        delete p;
        return nil;
    }
    std::vector<NmbBinaryArchive *> archives;
    if (!pipelineArchives(d.binaryArchives, archives, error)) {
        delete p;
        return nil;
    }
    const NmbPipelineOptions creation = {(uint64_t)options, archives.data(), archives.size()};
    SWITCH_METAL_DIAGNOSTIC(const uint64_t pipelineBegan = nowUs());
    SWITCH_METAL_DIAGNOSTIC(const NBCost costBegan = !journalOn() ? NBCost() : NBCost::now());
    NmbRenderPipeline *handle = nmb_render_pipeline_new_with_options(gDevice, p, &creation);
    delete p;
    SWITCH_METAL_DIAGNOSTIC(gTimePipeline.add(pipelineBegan));
    SWITCH_METAL_DIAGNOSTIC(gHistPso.add(nowUs() - pipelineBegan));
    SWITCH_METAL_DIAGNOSTIC(journalRenderPipeline(d, options, pipelineBegan, handle ? nullptr : nmb_last_error(),
                                                  !journalOn() ? std::string() : NBCost::now().since(costBegan)));
    if (!handle)
        return failWith<id<MTLRenderPipelineState>>(error, @"render pipeline", nil);
    NBRenderPipeline *state = [NBRenderPipeline new];
    state.handle = handle;
    SWITCH_METAL_DIAGNOSTIC(gPipelines++);
    if (error)
        *error = nil;
    return (id<MTLRenderPipelineState>)state;
}

- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error
{
    if (d.url && !d.url.isFileURL) {
        if (error)
            *error = nbError(@"a file URL is required");
        return nil;
    }
    NmbBinaryArchive *handle = nmb_binary_archive_new(gDevice, d.url ? d.url.fileSystemRepresentation : nullptr);
    if (!handle)
        return failWith<id<MTLBinaryArchive>>(error, @"binary archive", nil);
    NBBinaryArchive *archive = [NBBinaryArchive new];
    archive.handle = handle;
    if (error)
        *error = nil;
    return (id<MTLBinaryArchive>)archive;
}

- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)d
{
    const uint64_t row = nmb_device_linear_bytes_per_row(gDevice, d.pixelFormat, d.width);
    if (!row)
        NBLOG(@"native-metal: requiredLinearTextureBytesPerRow: %@", lastError());
    return row;
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
    NmbSamplerDescriptor s = {(uint64_t)d.minFilter,
                              (uint64_t)d.magFilter,
                              (uint64_t)d.mipFilter,
                              (uint64_t)d.sAddressMode,
                              (uint64_t)d.tAddressMode,
                              (uint64_t)d.rAddressMode,
                              (uint64_t)d.borderColor,
                              (uint64_t)d.normalizedCoordinates,
                              d.maxAnisotropy,
                              (uint64_t)d.compareFunction,
                              (uint64_t)d.lodAverage,
                              d.lodMinClamp,
                              d.lodMaxClamp};
    NmbSampler *handle = nmb_sampler_new(gDevice, &s);
    if (!handle) {
        NBLOG(@"native-metal: sampler refused: %@", lastError());
        return nil;
    }
    NBSampler *sampler = [NBSampler new];
    sampler.handle = handle;
    sampler.descriptor = d;
    return (id<MTLSamplerState>)sampler;
}

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
    auto face = [](MTLStencilDescriptor *s) -> NmbStencil {
        return {(uint64_t)s.stencilCompareFunction,
                (uint64_t)s.stencilFailureOperation,
                (uint64_t)s.depthFailureOperation,
                (uint64_t)s.depthStencilPassOperation,
                s.readMask,
                s.writeMask};
    };
    NmbDepthStencilDescriptor s = {(uint64_t)d.depthCompareFunction, (uint64_t)d.depthWriteEnabled,
                                   face(d.frontFaceStencil), face(d.backFaceStencil)};
    NmbDepthStencil *handle = nmb_depth_stencil_new(gDevice, &s);
    if (!handle) {
        NBLOG(@"native-metal: depth/stencil state refused: %@", lastError());
        return nil;
    }
    NBDepthStencil *state = [NBDepthStencil new];
    state.handle = handle;
    return (id<MTLDepthStencilState>)state;
}

@end

// The device queries the runtime answers (nmb_device_answer_selector), as methods of the device class. One without an argument is read once
// and installed only where the device has an answer of its own; one with an argument asks the runtime once per argument and keeps the answer
// (each is a fixed property of the device, and QuartzCore asks thousands a second while the screen moves: about 140 us each on Adreno),
// a refusal answering 0/NO with the reason logged once. NMB_ANSWER_CACHE=0 asks the runtime every call again.
static std::unordered_map<SEL, uint64_t> gAnswers;

static NSUInteger countAnswer(id, SEL cmd)
{
    return (NSUInteger)gAnswers.at(cmd);
}

static BOOL flagAnswer(id, SEL cmd)
{
    return gAnswers.at(cmd) != 0;
}

static uint64_t askAnswer(SEL cmd, NSUInteger arg)
{
    static const bool keep = [] {
        const char *v = getenv("NMB_ANSWER_CACHE");
        return !v || strcmp(v, "0");
    }();
    static std::mutex lock;
    static std::unordered_map<SEL, std::unordered_map<NSUInteger, uint64_t>> kept;
    if (keep) {
        std::lock_guard<std::mutex> hold(lock);
        auto answers = kept.find(cmd);
        if (answers != kept.end()) {
            auto answer = answers->second.find(arg);
            if (answer != answers->second.end()) {
                SWITCH_METAL_DIAGNOSTIC(gAnswersKept++);
                return answer->second;
            }
        }
    }
    SWITCH_METAL_DIAGNOSTIC(gAnswersAsked++);
    uint64_t value = 0;
    if (!nmb_device_answer(gDevice, sel_getName(cmd), arg, &value)) {
        NBREFUSE("device.answer", uintptr_t(cmd) ^ uint64_t(arg) << 48, @"no device answer for %s(%lu): %@",
                 sel_getName(cmd), (unsigned long)arg, lastError());
        value = 0;
    }
    if (keep) {
        std::lock_guard<std::mutex> hold(lock);
        kept[cmd][arg] = value;
    }
    return value;
}

static NSUInteger countArgAnswer(id, SEL cmd, NSUInteger arg)
{
    return (NSUInteger)askAnswer(cmd, arg);
}

static BOOL flagArgAnswer(id, SEL cmd, NSUInteger arg)
{
    return askAnswer(cmd, arg) != 0;
}

// Each object class conforms to its Metal protocol, as the target's own drivers' classes do through their _MTL base classes:
// the OS checks conformance before trusting an object (MPSCore's -[MPSCommandBufferImageCache initWithCommandBuffer:] refuses a
// command buffer that does not conform to MTLCommandBuffer, and MPSAutoCache then reads its nil cache).
static void adoptMetalProtocols()
{
    const std::pair<Class, Protocol *> classes[] = {
        {[NBBuffer class], @protocol(MTLBuffer)},
        {[NBTexture class], @protocol(MTLTexture)},
        {[NBLibrary class], @protocol(MTLLibrary)},
        {[NBFunction class], @protocol(MTLFunction)},
        {[NBArgumentEncoder class], @protocol(MTLArgumentEncoder)},
        {[NBComputePipeline class], @protocol(MTLComputePipelineState)},
        {[NBRenderPipeline class], @protocol(MTLRenderPipelineState)},
        {[NBSampler class], @protocol(MTLSamplerState)},
        {[NBDepthStencil class], @protocol(MTLDepthStencilState)},
        {[NBEvent class], @protocol(MTLEvent)},
        {[NBSharedEvent class], @protocol(MTLSharedEvent)},
        {[NBFence class], @protocol(MTLFence)},
        {[NBHeap class], @protocol(MTLHeap)},
        {[NBQueue class], @protocol(MTLCommandQueue)},
        {[NBCommandBuffer class], @protocol(MTLCommandBuffer)},
        {[NBEncoder class], @protocol(MTLCommandEncoder)},
        {[NBComputeEncoder class], @protocol(MTLComputeCommandEncoder)},
        {[NBRenderEncoder class], @protocol(MTLRenderCommandEncoder)},
        {[NBBlitEncoder class], @protocol(MTLBlitCommandEncoder)},
        {[NBBinaryArchive class], @protocol(MTLBinaryArchive)},
    };
    for (const auto &[cls, protocol] : classes)
        class_addProtocol(cls, protocol);
}

static void installAnswers(Class cls)
{
    uint32_t kind = 0;
    for (size_t i = 0; const char *name = nmb_device_answer_selector(i, &kind); i++) {
        SEL sel = sel_registerName(name);
        uint64_t value = 0;
        switch (kind) {
        case NMB_ANSWER_COUNT:
        case NMB_ANSWER_FLAG:
            if (!nmb_device_answer(gDevice, name, 0, &value)) {
                NBREFUSE("device.answer", uintptr_t(sel), @"no device answer for %s: %@", name, lastError());
                continue;
            }
            // Extended-range 10-bit (XR10) framebuffers are withheld from the render server: this display chain presents 8-bit BGRX scanout
            // only, and backboardd picks its wide-gamut 64-bit (and Apple-compressed) framebuffer when a device offers them, which the display
            // driver cannot show (its swap's surface is '&w4a', 8 bytes per pixel, and every swap is refused). Everyone else gets
            // the runtime's answer: CAMetalLayer raises CAMetalLayerInvalid ("unsupported extended range format") at setPixelFormat: of an
            // XR10 format on a device without them, and ClockKitUI's quad views ask for one whatever the display (AegirPoster's colorSpace 2
            // is BGR10_XR_sRGB). Their 'w30r'/'w40a' drawables are textures and render targets here, and the render server samples them.
            if (!strcmp(name, "supportsExtendedXR10Formats") && !strcmp(getprogname(), "backboardd"))
                value = 0;
            // Command buffer jumps are withheld: on YES MPSGraph and MPSImageCanny send the AGX encoder's encodeStartWhile/If/Else/EndIf SPI,
            // which this frontend's encoders do not implement.
            if (!strcmp(name, "supportsCommandBufferJump"))
                value = 0;
            gAnswers[sel] = value;
            class_addMethod(cls, sel, kind == NMB_ANSWER_COUNT ? (IMP)countAnswer : (IMP)flagAnswer,
                            kind == NMB_ANSWER_COUNT ? "Q16@0:8" : "B16@0:8");
            break;
        case NMB_ANSWER_COUNT_ARG:
            class_addMethod(cls, sel, (IMP)countArgAnswer, "Q24@0:8Q16");
            break;
        case NMB_ANSWER_FLAG_ARG:
            class_addMethod(cls, sel, (IMP)flagArgAnswer, "B24@0:8Q16");
            break;
        }
    }
}

extern "C" id native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID,
                                       void (*missing)(const char *, const char *))
{
    if (gDeviceObject)
        return gDeviceObject;
    gMissing = missing;
    gInFlight = [NSMutableSet set];
    gNvkPath = loader ? loader : "";
    gDevice = nmb_device_open(loader, cache);
    if (!gDevice) {
        NBLOG(@"native-metal: device refused: %s", nmb_last_error());
        return nil;
    }
    gMemoryRelease.start(gDevice);
    if (cache) {
        gCompileCache = std::string(cache) + "/msl";
        gNativeArtifactStorage.cache = std::string(cache) + "/native-analysis";
    }
    if (const char *seed = getenv("METAL2VULKAN_SHADER_CACHE_SEED"); seed && *seed) {
        gCompileSeed = std::string(seed) + "/msl";
        gNativeArtifactStorage.seed = std::string(seed) + "/native-analysis";
    }
    if (cache && kJournal) {
        // The journal lives in a cache that survives reboots: past 2 MiB it starts again, the previous one kept as pipelines.log.old.
        gJournalPath = std::string(cache) + "/pipelines.log";
        gJournalBegan = nowUs();
        struct stat journalStat;
        if (stat(gJournalPath.c_str(), &journalStat) == 0 && journalStat.st_size > (2 << 20))
            rename(gJournalPath.c_str(), (gJournalPath + ".old").c_str());
    }
    char name[128] = {};
    nmb_device_name(gDevice, name, sizeof name);
    gName = [NSString stringWithUTF8String:name];
    gRegistryID = registryID;
    // The target families supportsFamily: answers true, with the rows of Apple's tables the device does not meet in them (nmb_device_family_waivers).
    {
        char waived[4096] = {};
        nmb_device_family_waivers(gDevice, waived, sizeof waived);
        // No filter of sampled RG8Unorm_sRGB (no R8G8_SRGB sampled image): such textures are held as RGBA8 (heldAsRGBA8).
        gHoldRG8SrgbAsRGBA8 = strstr(waived, "RG8Unorm_sRGB/Filter") != nullptr;
        for (char *line = waived, *next; *line; line = next) {
            next = strchr(line, '\n');
            if (next)
                *next++ = 0;
            else
                next = line + strlen(line);
            if (journalOn())
                journal(std::string("F ") + line);
        }
    }
    Class base = objc_getClass("_MTLDevice");
#if !TARGET_OS_IPHONE
    // Host instrument: on NSObject every selector a client sends that this file lacks reaches the missing hook, instead of the host's
    // own _MTLDevice implementation.
    if (getenv("NMB_DEVICE_BASE_NSOBJECT"))
        base = [NSObject class];
#endif
#if TARGET_OS_IPHONE
    // The dynamic subclass adds no instance variables to the selected IPSW kind's base class (MetalABI.h).
    if (!base || class_getInstanceSize(base) != kMTLDeviceInstanceSize) {
        NBLOG(@"native-metal: unexpected _MTLDevice ABI");
        return nil;
    }
#endif
    if (!base)
        base = [NSObject class];
    Class cls = objc_allocateClassPair(base, "NativeBridgeMetalDevice", 0);
    if (!cls)
        return nil;
    unsigned count = 0;
    Method *methods = class_copyMethodList([NBDeviceMethods class], &count);
    for (unsigned i = 0; i < count; i++)
        class_addMethod(cls, method_getName(methods[i]), method_getImplementation(methods[i]),
                        method_getTypeEncoding(methods[i]));
    free(methods);
    installAnswers(cls);
    adoptMetalProtocols();
    class_addProtocol(cls, @protocol(MTLDevice));
    // The original registration (_MTLAddDevice) asserts that a plugin's device conforms to Metal's private device protocol as well; the target's own
    // drivers get it from their IOGPU base class. Conformance only answers conformsToProtocol:, the methods are still looked up one by one.
    if (Protocol *spi = objc_getProtocol("MTLDeviceSPI"))
        class_addProtocol(cls, spi);
    objc_registerClassPair(cls);
    gDeviceObject = [cls new];
    fillLimits(gDeviceObject);
    return gDeviceObject;
}

// Bare AIR bitcode (no metallib wrapper), which Metal itself refuses: for the bridge's own fixtures only.
extern "C" id native_bridge_metal_library_with_air(const void *bytes, size_t length)
{
    NmbLibrary *handle = gDevice ? nmb_library_new(gDevice, bytes, length, true) : nullptr;
    if (!handle)
        return nil;
    NBLibrary *library = [NBLibrary new];
    library.handle = handle;
    return library;
}

extern "C" bool native_bridge_metal_compile_open(const char *loader)
{
    if (!gCompile) {
        gNvkPath = loader ? loader : "";
        gCompile = nmb_compile_context_open(loader);
    }
    return gCompile != nullptr;
}

extern "C" id native_bridge_metal_compile_library(const char *path, dispatch_data_t data, NSError **error)
{
    if (!gCompile) {
        if (error)
            *error = nbError(@"no compile context");
        return nil;
    }
    if (path) {
        NSData *bytes = [NSData dataWithContentsOfFile:@(path) options:NSDataReadingMappedIfSafe error:error];
        return bytes ? metallibLibrary(bytes, (const uint8_t *)bytes.bytes, bytes.length, error, true) : nil;
    }
    const void *bytes = nullptr;
    size_t size = 0;
    dispatch_data_t mapped = dispatch_data_create_map(data, &bytes, &size);
    return metallibLibrary(mapped, (const uint8_t *)bytes, size, error, true);
}

extern "C" bool native_bridge_metal_shader_cache_stats(NmbShaderCacheStats *out)
{
    return gDevice && nmb_device_shader_cache_stats(gDevice, out);
}

extern "C" bool native_bridge_metal_work_stats(uint64_t out[10])
{
    return gDevice && nmb_device_work_stats(gDevice, out);
}

extern "C" void native_bridge_metal_release_idle_memory(void)
{
    gMemoryRelease.release();
}

extern "C" bool native_bridge_metal_memory(uint64_t out[3])
{
    return gDevice && nmb_device_memory(gDevice, out);
}

extern "C" size_t native_bridge_metal_status(char *out, size_t size)
{
    std::lock_guard<std::mutex> lock(gProblemLock);
    return (size_t)snprintf(out, size, "c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%llu C%llu B%llu %s | %s",
                            (unsigned long long)gCommits.load(), (unsigned long long)gCommitFailures.load(),
                            (unsigned long long)gCompleted.load(), (unsigned long long)gPipelines.load(),
                            (unsigned long long)gProblems.load(), (unsigned long long)gPasses.load(),
                            (unsigned long long)gDraws.load(), (unsigned long long)gFullScreenPasses.load(),
                            (unsigned long long)(gBiggestPass.load() >> 10), (unsigned long long)gDispatches.load(),
                            (unsigned long long)gBlits.load(), gLastProblems[0], gLastProblems[1]);
}

static size_t native_bridge_metal_formats_locked(char *out, size_t size)
{
    int n = snprintf(out, size, "tex x%llu f", (unsigned long long)gTextureRefusals.load());
    for (unsigned i = 0; i < gFormatCount && n > 0 && (size_t)n < size; i++)
        n += snprintf(out + n, size - (size_t)n, "%u ", gFormats[i]);
    return n > 0 ? (size_t)n : 0;
}

// The benchmark's rows (native_bridge_metal_benchmark), kept for the telemetry table.
static std::mutex gBenchLock;
static char gBench[4][96];
static std::atomic<unsigned> gBenchRuns;

extern "C" size_t native_bridge_metal_benchmark(id device, const char *metallib, unsigned pairs, char *out, size_t size)
{
    auto now = [] {
        return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    };
    // the first run fills rows 0-1 (Y, Y1), a later one rows 2-3 (Z, Z1), so a busy and an idle run are both kept
    const unsigned base = gBenchRuns++ ? 2u : 0u;
    auto put = [&](unsigned row, const char *format, auto... args) {
        std::lock_guard<std::mutex> lock(gBenchLock);
        snprintf(gBench[base + row], sizeof gBench[base + row], format, args...);
        if (base)
            gBench[base + row][0] = 'Z';
    };
    // compute: a dependent integer chain (no memory), 100M steps
    uint64_t t = now(), x = 0x9e3779b97f4a7c15ull;
    for (uint32_t i = 0; i < 100000000u; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    const uint64_t computeUs = (now() - t) / 1000;
    // first touch: 32 MiB of fresh anonymous memory (the tablet's free memory is small), one store per 4 KiB page (the kernel's fault and zero-fill), then four memsets of it
    const size_t bytes = 32u << 20;
    uint64_t faultUs = 0, fillUs = 0, shaUs = 0;
    void *m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m != MAP_FAILED) {
        volatile uint8_t *b = (volatile uint8_t *)m;
        t = now();
        for (size_t at = 0; at < bytes; at += 4096)
            b[at] = 1;
        faultUs = (now() - t) / 1000;
        t = now();
        for (int k = 0; k < 4; k++)
            memset(m, k, bytes);
        fillUs = (now() - t) / 1000;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        t = now();
        CC_SHA256(m, (CC_LONG)(16u << 20), digest);
        shaUs = (now() - t) / 1000;
        x ^= digest[0];
        munmap(m, bytes);
    }
    put(0, "Y cpu=%llums(100M) fault=%lluus/32MiB=%lluns/pg fill=%lluMB/s sha=%lluMB/s %llx",
        (unsigned long long)(computeUs / 1000), (unsigned long long)faultUs,
        (unsigned long long)(faultUs * 1000 / 8192), (unsigned long long)(fillUs ? 4ull * 32 * 1000000 / fillUs : 0),
        (unsigned long long)(shaUs ? 16ull * 1000000 / shaUs : 0), (unsigned long long)(x & 0xf));
    // pipelines: the host tool's pairs, in its order
    uint64_t made = 0, refused = 0, createUs = 0, createMax = 0, drawUs = 0, drawMax = 0, libraryUs = 0;
    @autoreleasepool {
        id<MTLDevice> dev = (id<MTLDevice>)device;
        NSError *error = nil;
        t = now();
        id<MTLLibrary> qc = metallib ? [dev newLibraryWithURL:[NSURL fileURLWithPath:@(metallib)] error:&error] : nil;
        libraryUs = (now() - t) / 1000;
        if (qc) {
            NSArray<NSString *> *names = qc.functionNames;
            id<MTLCommandQueue> queue = [dev newCommandQueue];
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:64
                                                                                         height:64
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> target = [dev newTextureWithDescriptor:td];
            id<MTLBuffer> zeros = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
            unsigned n = 0;
            for (NSString *name in names) {
                if (n >= pairs)
                    break;
                id<MTLFunction> f = [qc newFunctionWithName:name];
                if (f.functionType != MTLFunctionTypeVertex)
                    continue;
                NSString *fragmentName = [name stringByReplacingOccurrencesOfString:@"_vert_" withString:@"_frag_"];
                if ([fragmentName isEqualToString:name] || ![names containsObject:fragmentName])
                    continue;
                ++n;
                MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = f;
                d.fragmentFunction = [qc newFunctionWithName:fragmentName];
                d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                t = now();
                id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:d error:&error];
                const uint64_t took = (now() - t) / 1000;
                if (!state) {
                    ++refused;
                    continue;
                }
                ++made;
                createUs += took;
                createMax = std::max(createMax, took);
                t = now();
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
                [encoder setRenderPipelineState:state];
                for (NSUInteger i = 0; i < 8; ++i) {
                    [encoder setVertexBuffer:zeros offset:0 atIndex:i];
                    [encoder setFragmentBuffer:zeros offset:0 atIndex:i];
                }
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [encoder endEncoding];
                [commands commit];
                [commands waitUntilCompleted];
                const uint64_t drew = (now() - t) / 1000;
                drawUs += drew;
                drawMax = std::max(drawMax, drew);
            }
        }
    }
    put(1, "Y1 qc lib=%llums made=%llu refused=%llu create=%llums max=%llums draw=%llums max=%llums",
        (unsigned long long)(libraryUs / 1000), (unsigned long long)made, (unsigned long long)refused,
        (unsigned long long)(createUs / 1000), (unsigned long long)(createMax / 1000),
        (unsigned long long)(drawUs / 1000), (unsigned long long)(drawMax / 1000));
    size_t at = 0;
    if (out && size) {
        std::lock_guard<std::mutex> lock(gBenchLock);
        out[0] = 0;
        for (unsigned r = base; r < base + 2; r++)
            at += (size_t)snprintf(out + at, size > at ? size - at : 0, "%s\n", gBench[r]);
    }
    return at;
}

// One line of rates since the previous call (the plugin logs it every 5 s; 0 and nothing written when no command buffer was
// committed and no pipeline made since): commits per second; process CPU per commit and mean encoding wall time; the runtime's
// commit call (submission, with any CPU wait in it); time blocked in waitUntilCompleted/Scheduled and resource waits; GPU latency
// (commit to completion); per commit the draws, the render passes and the time inside this frontend's encoding calls (api: the
// rest of enc is the caller's own work between them), and the passes that could have joined the one before (join); the runtime's
// host calls that waited for a submission of their own (staging queue, internal) and its host texel copies (gw); pipeline states
// made (count, ms) and the driver's VkPipeline makings (count, ms); the shader cache's hits, misses and writes, and the driver
// records' hits and misses; the host imports the GPU wrote that this process read back, invalidated lazily (rb: count/MB/ms); last,
// surface imports made (count/ms) and answered by a live one (hits), and textures made over them (stx: count/ms).
extern "C" size_t native_bridge_metal_rates(char *out, size_t size)
{
    // sys= is the kernel's part of cpu= (both per commit, all of this process's threads), flt= the process's page faults per commit.
    struct Snapshot {
        uint64_t at, cpu, sys, faults, commits, encode, submitN, submitUs, waitN, waitUs, gpuN, gpuUs, draws, passes,
            apiUs, joinable, psoN, psoUs, texN, texUs, asked, kept, planar, rb, rbBytes, rbUs, impN, impUs, impHits,
            stxN, stxUs, fwdN, fwdUs;
        uint64_t work[10], counts[10], waits[4], routes[7], memory[3];
        NmbShaderCacheStats cache;
    };

    static std::mutex lock;
    static Snapshot last{};
    Snapshot now{};
    now.at = nowUs();
    struct rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    now.cpu = (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 + usage.ru_utime.tv_usec +
              usage.ru_stime.tv_usec;
    now.sys = (uint64_t)usage.ru_stime.tv_sec * 1000000 + usage.ru_stime.tv_usec;
    now.faults = (uint64_t)(usage.ru_minflt + usage.ru_majflt);
    now.commits = gCommits.load();
    now.encode = gEncodeUs.load();
    now.submitN = gTimeSubmit.count.load();
    now.submitUs = gTimeSubmit.total.load();
    now.waitN = gTimeWaitCompleted.count.load() + gTimeWaitScheduled.count.load() + gTimeResourceWait.count.load();
    now.waitUs = gTimeWaitCompleted.total.load() + gTimeWaitScheduled.total.load() + gTimeResourceWait.total.load();
    now.gpuN = gTimeEncodeToCommit.count.load();
    now.gpuUs = gTimeEncodeToCommit.total.load();
    now.draws = gDraws.load();
    now.passes = gPasses.load();
    now.apiUs = gApiUs.load();
    now.joinable = gJoinablePasses.load();
    now.psoN = gTimePipeline.count.load();
    now.psoUs = gTimePipeline.total.load();
    now.texN = gTimeTexelCopy.count.load();
    now.texUs = gTimeTexelCopy.total.load();
    now.asked = gAnswersAsked.load();
    now.kept = gAnswersKept.load();
    now.planar = gSurfaceMultiPlane.load();
    now.rb = gReadbackInvalidates.load();
    now.rbBytes = gReadbackBytes.load();
    now.rbUs = gReadbackUs.load();
    now.impN = gTimeSurfaceImport.count.load();
    now.impUs = gTimeSurfaceImport.total.load();
    now.impHits = gSurfaceImportHits.load();
    now.stxN = gTimeSurfaceTexture.count.load();
    now.stxUs = gTimeSurfaceTexture.total.load();
    now.fwdN = gForwardCalls.load();
    now.fwdUs = gForwardUs.load();
    if (gDevice) {
        nmb_device_work_stats(gDevice, now.work);
        nmb_device_shader_cache_stats(gDevice, &now.cache);
        nmb_device_counts(gDevice, now.counts);
        nmb_device_memory(gDevice, now.memory);
        nmb_device_texel_copy_routes(gDevice, now.routes);
        nmb_device_wait_counts(gDevice, now.waits);
    }
    std::lock_guard<std::mutex> guard(lock);
    const Snapshot was = last;
    last = now;
    const uint64_t commits = now.commits - was.commits, pso = now.psoN - was.psoN, vk = now.work[0] - was.work[0];
    // This window's command-buffer distributions, taken (and cleared) whether or not the line is written.
    const NBHist::Taken enc = gHistEncode.take(17000, 34000), gap = gHistGap.take(25000, 34000),
                        sched = gHistSched.take(17000, 34000);
    const NBHist::Taken gpu = gHistGpu.take(17000, 34000), made = gHistPso.take(17000, 34000);
    if (!was.at || (!commits && !pso && !vk)) {
        if (out && size)
            out[0] = 0;
        return 0;
    }
    auto per = [](uint64_t total, uint64_t n) {
        return n ? (double)total / n / 1000.0 : 0.0;
    };  // ms per item
    const double seconds = (double)(now.at - was.at) / 1e6;
    const double perCommit = commits ? 1.0 / (double)commits : 0.0;
    const int n = snprintf(
        out, size,
        "%.0fs c=%llu(%.1f/s) cpu=%.1fms/c sys=%.1fms flt=%.0f enc=%.1fms sub=%.2fms wait=%llums/%llu tex=%llu/%llums "
        "gpu=%.1fms "
        "d=%.0f p=%.1f api=%.1fms join=%llu gw=s%llu/i%llu/h%llu pso=%llu/%llums vk=%llu/%llums "
        "cache=h%llu/m%llu/w%llu drv=l%llu/h%llu/m%llu ans=%llu/%llu planar=%llu mem=%lluMB/%llu+%llu+%llu tlw=%llu "
        "var=%llu lnk=%llu fsl=%llu shadow=%llu rb=%llu/%lluMB/%.1fms imp=%llu/%.1fms/h%llu stx=%llu/%.1fms",
        seconds, (unsigned long long)commits, commits / seconds, per(now.cpu - was.cpu, commits),
        per(now.sys - was.sys, commits), (double)(now.faults - was.faults) * perCommit,
        per(now.encode - was.encode, commits), per(now.submitUs - was.submitUs, now.submitN - was.submitN),
        (unsigned long long)((now.waitUs - was.waitUs) / 1000), (unsigned long long)(now.waitN - was.waitN),
        (unsigned long long)(now.texN - was.texN), (unsigned long long)((now.texUs - was.texUs) / 1000),
        per(now.gpuUs - was.gpuUs, now.gpuN - was.gpuN), (double)(now.draws - was.draws) * perCommit,
        (double)(now.passes - was.passes) * perCommit, per(now.apiUs - was.apiUs, commits),
        (unsigned long long)(now.joinable - was.joinable), (unsigned long long)(now.waits[0] - was.waits[0]),
        (unsigned long long)(now.waits[1] - was.waits[1]), (unsigned long long)(now.waits[2] - was.waits[2]),
        (unsigned long long)pso, (unsigned long long)((now.psoUs - was.psoUs) / 1000), (unsigned long long)vk,
        (unsigned long long)((now.work[1] - was.work[1]) / 1000), (unsigned long long)(now.work[7] - was.work[7]),
        (unsigned long long)(now.work[8] - was.work[8]), (unsigned long long)(now.work[9] - was.work[9]),
        (unsigned long long)(now.cache.driver_loads - was.cache.driver_loads),
        (unsigned long long)(now.cache.driver_hits - was.cache.driver_hits),
        (unsigned long long)(now.cache.driver_misses - was.cache.driver_misses),
        (unsigned long long)(now.asked - was.asked), (unsigned long long)(now.kept - was.kept),
        (unsigned long long)(now.planar - was.planar), (unsigned long long)(now.memory[0] >> 20),
        (unsigned long long)now.counts[1], (unsigned long long)now.counts[2], (unsigned long long)now.counts[3],
        (unsigned long long)(now.counts[4] - was.counts[4]), (unsigned long long)(now.counts[5] - was.counts[5]),
        (unsigned long long)(now.counts[6] - was.counts[6]), (unsigned long long)(now.counts[7] - was.counts[7]),
        (unsigned long long)(now.counts[8] - was.counts[8]), (unsigned long long)(now.rb - was.rb),
        (unsigned long long)((now.rbBytes - was.rbBytes) >> 20), (double)(now.rbUs - was.rbUs) / 1000.0,
        (unsigned long long)(now.impN - was.impN), (double)(now.impUs - was.impUs) / 1000.0,
        (unsigned long long)(now.impHits - was.impHits), (unsigned long long)(now.stxN - was.stxN),
        (double)(now.stxUs - was.stxUs) / 1000.0);
    size_t at = n > 0 ? std::min<size_t>((size_t)n, size ? size - 1 : 0) : 0;
    // In ms: encp and cgap (one thread's commit to its next) p50/p99/max with the counts at 17 ms and over (past a 60 Hz
    // frame) and, for cgap, at 25 and 34 ms and over (1.5x and over 2x the frame); swait (waitUntilScheduled) and gpuq (commit to
    // completion) p50/p99/max; the slowest render pipeline state made. Bins are whole ms.
    auto ms = [](uint64_t us) {
        return (unsigned long long)(us / 1000);
    };
    if (at + 1 < size) {
        const int more =
            snprintf(out + at, size - at,
                     " encp=%llu/%llu/%llums>16:%llu cgap=%llu/%llu/%llums>25:%llu>33:%llu swait=%llu/%llu/%llums "
                     "gpuq=%llu/%llu/%llums psomax=%llums",
                     ms(enc.p50), ms(enc.p99), ms(enc.max), (unsigned long long)enc.over, ms(gap.p50), ms(gap.p99),
                     ms(gap.max), (unsigned long long)gap.over, (unsigned long long)gap.over2, ms(sched.p50),
                     ms(sched.p99), ms(sched.max), ms(gpu.p50), ms(gpu.p99), ms(gpu.max), ms(made.max));
        if (more > 0)
            at = std::min<size_t>(at + (size_t)more, size - 1);
    }
    if (at + 1 < size)
        at += nbasync::appendRates(out + at, size - at);  // apso= ... (AsyncPipelines.h)
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // seed=: look-ups the shader cache seed missed since the last line, and a seed hit's p50/p99 look-up so far (ns, the
    // power of two above it; counted in debug builds only).
    if (at + 1 < size) {
        const int more =
            snprintf(out + at, size - at, " seed=m%llu/%llu/%lluns",
                     (unsigned long long)(now.cache.seed_misses - was.cache.seed_misses),
                     (unsigned long long)now.cache.seed_hit_p50_ns, (unsigned long long)now.cache.seed_hit_p99_ns);
        if (more > 0)
            at = std::min<size_t>(at + (size_t)more, size - 1);
    }
#endif
    // spare=: dedicated memory requests a kept block served instead of a new BoAlloc (m2v 0113, memory.rs).
    if (at + 1 < size) {
        // held=: waitUntilScheduled calls that returned behind a shared event not reached (heldBehindEvent). rr= and gb=:
        // replaceRegion by route (into a buffer's bytes / on the host / staged for the next submission / waited for) and
        // getBytes (from a buffer's bytes / on the host / waited for), m2v 0116: the waited ones are a GPU wait each.
        static uint64_t heldWas;
        const uint64_t held = gHeldReturns.load();
        uint64_t d[7];
        for (unsigned i = 0; i < 7; i++)
            d[i] = now.routes[i] - was.routes[i];
        const int more = snprintf(out + at, size - at, " spare=%llu held=%llu rr=%llu/%llu/%llu/%llu gb=%llu/%llu/%llu",
                                  (unsigned long long)(now.counts[9] - was.counts[9]),
                                  (unsigned long long)(held - heldWas), (unsigned long long)d[0],
                                  (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3],
                                  (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)d[6]);
        heldWas = held;
        if (more > 0)
            at = std::min<size_t>(at + (size_t)more, size - 1);
    }
    if (at + 1 < size) {
        // fwd=: calls of Metal selectors this file lacks, answered zero through message forwarding
        // (native_bridge_metal_answer_unimplemented), and their time from the signature asked to the zero returned.
        const int more = snprintf(out + at, size - at, " fwd=%llu/%.1fms", (unsigned long long)(now.fwdN - was.fwdN),
                                  (double)(now.fwdUs - was.fwdUs) / 1000.0);
        if (more > 0)
            at = std::min<size_t>(at + (size_t)more, size - 1);
    }
    return at;
}

// The telemetry table the plugin streams to the kernel's log: newline-separated rows of at most 90 characters.
extern "C" size_t native_bridge_metal_telemetry(char *out, size_t size)
{
    size_t at = 0;
    out[0] = 0;
    auto row = [&](const char *format, auto... args) {
        char text[200];
        int n = snprintf(text, sizeof text, format, args...);
        if (n <= 0)
            return;
        if (n > 90)
            n = 90;
        if (at + (size_t)n + 2 >= size)
            return;
        memcpy(out + at, text, (size_t)n);
        at += (size_t)n;
        out[at++] = '\n';
        out[at] = 0;
    };
    auto ms = [](uint64_t us) {
        return (unsigned long long)(us / 1000);
    };
    {
        std::lock_guard<std::mutex> lock(gBenchLock);
        for (unsigned r = 0; r < 4; r++)
            if (gBench[r][0])
                row("%.90s", gBench[r]);
    }
    row("S c%llu/%llu d%llu p%llu x%llu R%llu D%llu F%llu M%lluk C%llu B%llu", (unsigned long long)gCommits.load(),
        (unsigned long long)gCommitFailures.load(), (unsigned long long)gCompleted.load(),
        (unsigned long long)gPipelines.load(), (unsigned long long)gProblems.load(), (unsigned long long)gPasses.load(),
        (unsigned long long)gDraws.load(), (unsigned long long)gFullScreenPasses.load(),
        (unsigned long long)(gBiggestPass.load() >> 10), (unsigned long long)gDispatches.load(),
        (unsigned long long)gBlits.load());
    row("T pipeline n=%llu total=%llums max=%llums", (unsigned long long)gTimePipeline.count.load(),
        ms(gTimePipeline.total.load()), ms(gTimePipeline.slowest.load()));
    row("T library n=%llu total=%llums max=%llums", (unsigned long long)gTimeLibrary.count.load(),
        ms(gTimeLibrary.total.load()), ms(gTimeLibrary.slowest.load()));
    row("T function n=%llu total=%llums max=%llums", (unsigned long long)gTimeFunction.count.load(),
        ms(gTimeFunction.total.load()), ms(gTimeFunction.slowest.load()));
    row("G commit-to-complete n=%llu total=%llums max=%llums mean=%lluus",
        (unsigned long long)gTimeEncodeToCommit.count.load(), ms(gTimeEncodeToCommit.total.load()),
        ms(gTimeEncodeToCommit.slowest.load()),
        (unsigned long long)(gTimeEncodeToCommit.count.load()
                                 ? gTimeEncodeToCommit.total.load() / gTimeEncodeToCommit.count.load()
                                 : 0));
    row("J scheduled wait n=%llu total=%llums max=%llums", (unsigned long long)gTimeWaitScheduled.count.load(),
        ms(gTimeWaitScheduled.total.load()), ms(gTimeWaitScheduled.slowest.load()));
    row("J completed wait n=%llu total=%llums max=%llums", (unsigned long long)gTimeWaitCompleted.count.load(),
        ms(gTimeWaitCompleted.total.load()), ms(gTimeWaitCompleted.slowest.load()));
    row("J resource wait n=%llu total=%llums max=%llums", (unsigned long long)gTimeResourceWait.count.load(),
        ms(gTimeResourceWait.total.load()), ms(gTimeResourceWait.slowest.load()));
    row("J resource checks=%llu skipped=%llu", (unsigned long long)gResourceChecks.load(),
        (unsigned long long)gResourceSkipped.load());
    // The runtime's VkPipeline makings (the creation log, METAL2VULKAN_LOG_PIPELINES; they happen at draws, not at the state's creation) and
    // its shader cache: where the driver's compile time goes.
    uint64_t work[10] = {};
    if (gDevice && nmb_device_work_stats(gDevice, work)) {
        row("V vkpipe n=%llu total=%llums max=%llums waited=%llums failed=%llu", (unsigned long long)work[0],
            ms(work[1]), ms(work[2]), ms(work[3]), (unsigned long long)work[4]);
        row("V cache hit%llu miss%llu wr%llu | msg i%llu w%llu", (unsigned long long)work[7],
            (unsigned long long)work[8], (unsigned long long)work[9], (unsigned long long)work[5],
            (unsigned long long)work[6]);
        char warning[200];
        size_t n = nmb_last_warning(warning, sizeof warning);
        if (n) {
            row("W %.86s", warning);
            if (n > 86)
                row("Wb %.86s", warning + 86);
        }
    }
    {
        std::lock_guard<std::mutex> lock(gProblemLock);
        char formats[160];
        native_bridge_metal_formats_locked(formats, sizeof formats);
        row("F %.86s", formats);
        row("P0 %.86s", gLastProblems[0]);
        row("P0b %.86s", strlen(gLastProblems[0]) > 86 ? gLastProblems[0] + 86 : "");
        row("P1 %.86s", gLastProblems[1]);
        row("P1b %.86s", strlen(gLastProblems[1]) > 86 ? gLastProblems[1] + 86 : "");
    }
    {
        std::lock_guard<std::mutex> lock(gCommitLock);
        const uint32_t newest = (uint32_t)gCommits.load();
        row("%s", "Q timing v2: per-buffer API counts; ctc=commit-to-callback; age includes caller idle");
        for (uint32_t k = 0; k < 20 && k < newest; k++) {
            const CommitRecord &r = gCommitRing[(newest - k) % 32];
            if (r.index != newest - k)
                continue;
            row("K%u t=%llu.%03llus rp%u dr%u kpx%llu cs%u bl%u ctc=%lluus", r.index,
                (unsigned long long)(r.committedUs / 1000000), (unsigned long long)((r.committedUs / 1000) % 1000),
                r.work.passes, r.work.draws, (unsigned long long)(r.work.pixels >> 10), r.work.dispatches, r.work.blits,
                (unsigned long long)r.callbackUs);
            if (k < 4)
                row("E%u age=%lluus scheduled=%lldus", r.index, (unsigned long long)r.ageUs, (long long)r.scheduledUs);
        }
    }
    return at;
}

extern "C" size_t native_bridge_metal_formats(char *out, size_t size)
{
    std::lock_guard<std::mutex> lock(gProblemLock);
    return native_bridge_metal_formats_locked(out, size);
}
