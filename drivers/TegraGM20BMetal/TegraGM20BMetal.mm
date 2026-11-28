// SPDX-License-Identifier: MIT
// See TegraGM20BMetal.h.
//
// A command buffer here is a tape: each encoder call appends a record (the bytes of setBytes copied in, every object retained by
// the command buffer until it completes). A call Metal's validation stops fails the command buffer as it is recorded. At commit,
// in commit order, the tape is encoded into the raw rings (gpu.cpp) and submitted; a refusal at encode fails the command
// buffer with its error. The completion thread sleeps in the kernel on each fence (the interrupt), then runs the handlers. A
// command buffer that waits on shared events goes to the submit thread with every one committed after it while it waits, which
// submits them in order once they may run; the render thread never blocks for it. waitUntilScheduled waits for the
// submission, so the display's copy (which waits on the GPU for every channel that imports the surface) never runs before the
// frame's work is submitted.
#include "lock.h"
#import "TegraGM20BMetalIndirect.h"
#import "TegraGM20BMetalPassCache.h"
#include "heap_fit.h"
#include "limits_fill.h"

#include <malloc/malloc.h>

#if SWITCH_FRAME_TIMELINE
std::atomic<uint64_t> gSurfaceTextures{0};  // the frame timeline's churn count (SwitchFrameTimeline.h)
#endif

os_log_t gm20bLog()
{
    return gm20b::log();
}

[[noreturn]] void waitTooLong(const char *what, const void *object)
{
    os_log_fault(gm20bLog(), "SwitchMetal: %{public}s %p not done after %lld s: abort() for the stack", what, object,
                 (long long)kWaitLimit.count());
#if !TARGET_OS_IPHONE
    fprintf(stderr, "SwitchMetal: %s %p not done after %lld s\n", what, object, (long long)kWaitLimit.count());
#endif
    abort();
}

// One line per distinct text, then silence (shipped paths: no diagnostics but one-shot lines).
void once(NSString *text)
{
    static gm20b::UnfairMutex lock;
    static std::set<std::string> seen;
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock);
        if (seen.size() > 512 || !seen.insert(text.UTF8String).second)
            return;
    }
    os_log_error(gm20bLog(), "SwitchMetal: %{public}@", text);
#if !TARGET_OS_IPHONE
    fprintf(stderr, "SwitchMetal: %s\n", text.UTF8String);  // host tests read their stderr
#endif
}

double hostSeconds()
{
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return double(mach_absolute_time()) * tb.numer / tb.denom / 1e9;
}

uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

GM20BState *gRM;
Class gSwitchMetalBufferClass;
Class gSwitchMetalTextureClass;
Class gSwitchMetalNoCopyTextureClass;
Class gSwitchMetalSamplerClass;
Class gSwitchMetalPipelineClass;
Class gSwitchMetalDepthStencilClass;
Class gSwitchMetalHeapClass;
Class gSwitchMetalComputeStateClass;

@implementation SwitchMetalObject

+ (void)load
{
    gSwitchMetalBufferClass = [SwitchMetalBuffer class];
    gSwitchMetalTextureClass = [SwitchMetalTexture class];
    gSwitchMetalNoCopyTextureClass = objc_getClass("SwitchMetalNoCopyTexture");
    gSwitchMetalSamplerClass = [SwitchMetalSampler class];
    gSwitchMetalPipelineClass = [SwitchMetalPipeline class];
    gSwitchMetalDepthStencilClass = [SwitchMetalDepthStencil class];
    gSwitchMetalHeapClass = [SwitchMetalHeap class];
    gSwitchMetalComputeStateClass = objc_getClass("SwitchMetalComputeState");
}

- (id<MTLDevice>)device
{
    return (id<MTLDevice>)_device;
}

// A selector a Metal protocol declares that this class has no method for is answered zero (forwardInvocation:)
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    const char *types = metalTypes(selector);
    return types ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
    answerZero(self, invocation);
}

@end

// Resource calls Core Animation makes on every texture and buffer. Ownership is accounting only: no content is protected on
// this device; waitUntilComplete waits for the raw work submitted so far (the kernel's fence interrupt).
@interface SwitchMetalObject (Resource)
@end
@implementation SwitchMetalObject (Resource)

- (kern_return_t)setOwnerWithIdentity:(mach_port_t)identity
{
    (void)identity;
    return KERN_SUCCESS;
}

- (uint64_t)protectionOptions
{
    return 0;
}

- (void)waitUntilComplete
{
    if (gRM && gRM->raw)
        gRM->raw->wait(gRM->raw->submitted());
}

@end

// metal_prepare's thread and what it made (taken once, by metal_device_open; one that did not open is prepared
// again by the plugin's next open)
static struct {
    gm20b::Mutex lock;
    std::thread thread;
    std::string path, err;
    uint64_t stack = 0;
    gm20b::Device *dev = nullptr;
    gm20b::OpenFailure failure = gm20b::OpenFailure::Structural;
    bool started = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint64_t openNs = 0, warmNs = 0, waitedNs = 0;
#endif
} gPrepared;

static gm20b::OpenFailure gOpenFailure = gm20b::OpenFailure::Structural;  // metal_device_open's last nil

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
static gm20b::Device *openDevice(const char *packPath, uint64_t stack, gm20b::Device::Role role, std::string &err,
                                 gm20b::OpenFailure &failure, uint64_t *openNs = nullptr, uint64_t *warmNs = nullptr)
#else
static gm20b::Device *openDevice(const char *packPath, uint64_t stack, gm20b::Device::Role role, std::string &err,
                                 gm20b::OpenFailure &failure)
#endif
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    gm20b::Device *dev = new gm20b::Device;
    dev->heapHolds(gm20b::heapHolds);
    if (!dev->open(packPath, stack, err, role, true)) {
        failure = dev->openFailure();
        delete dev;  // its connection closes: the kernel frees what it held
        return nullptr;
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    if (openNs && warmNs) {
        *openNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began;
        *warmNs = 0;
    }
#endif
    return dev;
}

void metal_prepare(const char *packPath, uint64_t stack)
{
    std::lock_guard<gm20b::Mutex> l(gPrepared.lock);
    if (gPrepared.started)
        return;
    gPrepared.started = true;
    gPrepared.path = packPath;
    gPrepared.stack = stack;
    gPrepared.thread = std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.gm20b.open");
        std::string err;
        gm20b::OpenFailure failure = gm20b::OpenFailure::Structural;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        uint64_t openNs = 0, warmNs = 0;
        gm20b::Device *dev = openDevice(gPrepared.path.c_str(), gPrepared.stack, gm20b::Device::Role::RenderServer, err,
                                        failure, &openNs, &warmNs);
#else
        gm20b::Device *dev =
            openDevice(gPrepared.path.c_str(), gPrepared.stack, gm20b::Device::Role::RenderServer, err, failure);
#endif
        std::lock_guard<gm20b::Mutex> m(gPrepared.lock);
        gPrepared.dev = dev;
        gPrepared.err = err;
        gPrepared.failure = failure;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        gPrepared.openNs = openNs;
        gPrepared.warmNs = warmNs;
#endif
    });
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
void metal_prepare_split(uint64_t *open_ns, uint64_t *warm_ns, uint64_t *waited_ns)
{
    std::lock_guard<gm20b::Mutex> l(gPrepared.lock);
    *open_ns = gPrepared.openNs;
    *warm_ns = gPrepared.warmNs;
    *waited_ns = gPrepared.waitedNs;
}
#endif

void metal_complete_states(const char *cacheDir, const char *nativeSeed)
{
    if (gRM && !gRM->completion)
        gRM->completion =
            std::make_unique<gm20b::Completion>(*gRM->raw, cacheDir, gRM->raw->pack().header()->stack, nativeSeed);
}

void metal_complete_states_lazy(switch_cache_dir_fn resolveCacheDir, const char *nativeSeed)
{
    if (gRM && !gRM->completion)
        gRM->completion = std::make_unique<gm20b::Completion>(
            *gRM->raw, nullptr, gRM->raw->pack().header()->stack, nativeSeed,
            resolveCacheDir ? std::function<std::string()>([resolveCacheDir]() -> std::string {
                char buf[512];
                return resolveCacheDir(buf, sizeof buf) ? std::string(buf) : std::string();
            })
                            : nullptr);
}

dispatch_queue_t followingRootQueue()
{
    return gm20b::followingRoot();
}

dispatch_queue_t followingSerialQueue(const char *label)
{
    return gm20b::followingSerial(label);
}

// The device class: a subclass of _MTLDevice (conforming to MTLDevice and MTLDeviceSPI, with no instance variables of its own:
// the base class's ABI is the IPSW kind's), with the facade's methods and the kind's capability answers
static Class deviceClass(NSString **error)
{
    Class base = NSClassFromString(@"_MTLDevice") ?: [NSObject class];
#if TARGET_OS_IPHONE
    if (class_getInstanceSize(base) != kMTLDeviceInstanceSize) {
        if (error)
            *error = @"unexpected _MTLDevice ABI";
        return nil;
    }
#endif
    Class cls = objc_allocateClassPair(base, "GM20BGM20BMetalDevice", 0);
    if (!cls) {
        if (error)
            *error = @"the device class could not be made";
        return nil;
    }
    unsigned count = 0;
    Method *methods = class_copyMethodList([SwitchMetalDevice class], &count);
    for (unsigned i = 0; i < count; i++)
        class_addMethod(cls, method_getName(methods[i]), method_getImplementation(methods[i]),
                        method_getTypeEncoding(methods[i]));
    free(methods);
    installDeviceAnswers(cls);
    installSharedTextureMethods(cls);
    class_addProtocol(cls, @protocol(MTLDevice));
    // _MTLAddDevice asserts the private device protocol too, which the target's own drivers get from their IOGPU base class
    if (Protocol *spi = objc_getProtocol("MTLDeviceSPI"))
        class_addProtocol(cls, spi);
    objc_registerClassPair(cls);
    return cls;
}

gm20b::OpenFailure metal_open_failure(void)
{
    return gOpenFailure;
}

// A client's memory goes back when its app goes to the background, as well as on memory pressure. By name: UIKit's and
// Foundation's extension notifications post only in the processes that link them, and this driver links neither.
static void observeBackground()
{
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    for (NSString *name in
         @[ @"UIApplicationDidEnterBackgroundNotification", @"NSExtensionHostDidEnterBackgroundNotification" ])
        [center addObserverForName:name
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *) {
                            gRawRelease.backgrounded();
                        }];
    for (NSString *name in
         @[ @"UIApplicationWillEnterForegroundNotification", @"NSExtensionHostWillEnterForegroundNotification" ])
        [center addObserverForName:name
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification *) {
                            gRawRelease.foregrounded();
                        }];
}

id<MTLDevice> metal_device_open(const char *packPath, uint64_t stack, uint64_t registryID, NSString **error)
{
    static gm20b::Mutex once;
    std::lock_guard<gm20b::Mutex> l(once);
    if (gRM)
        return gRM->device;  // one per process
    gm20bPoolEncoders();
    gm20bPoolCommandBuffers();
    gm20b::Device *dev = nullptr;
    std::string err;
    gm20b::OpenFailure failure = gm20b::OpenFailure::Structural;
    bool prepared = false;
    {
        std::unique_lock<gm20b::Mutex> m(gPrepared.lock);
        if (gPrepared.started && gPrepared.thread.joinable()) {
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            const uint64_t joining = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
            m.unlock();
            gPrepared.thread.join();  // what is left of it: it began with the plugin
            m.lock();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            gPrepared.waitedNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - joining;
#endif
        }
        if (gPrepared.started && gPrepared.path == packPath && gPrepared.stack == stack) {
            prepared = true;
            dev = gPrepared.dev;
            err = gPrepared.err;
            failure = gPrepared.failure;
            gPrepared.dev = nullptr;
            gPrepared.started = dev != nullptr;
        }
    }
    if (!prepared)
        dev = openDevice(packPath, stack, gm20b::Device::Role::Client, err, failure);
    if (!dev) {
        gOpenFailure = failure;
        if (error)
            *error = [NSString stringWithUTF8String:err.c_str()];
        return nil;
    }
    Class cls = deviceClass(error);
    if (!cls) {
        gOpenFailure = gm20b::OpenFailure::Structural;
        delete dev;
        return nil;
    }
    // Each facade class conforms to its Metal protocol
    const std::pair<Class, Protocol *> classes[] = {
        {[SwitchMetalBuffer class], @protocol(MTLBuffer)},
        {[SwitchMetalTexture class], @protocol(MTLTexture)},
        {[SwitchMetalLibrary class], @protocol(MTLLibrary)},
        {[SwitchMetalFunction class], @protocol(MTLFunction)},
        {[SwitchMetalHeap class], @protocol(MTLHeap)},
        {[SwitchMetalSampler class], @protocol(MTLSamplerState)},
        {[SwitchMetalDepthStencil class], @protocol(MTLDepthStencilState)},
        {[SwitchMetalPipeline class], @protocol(MTLRenderPipelineState)},
        {[SwitchMetalQueue class], @protocol(MTLCommandQueue)},
        {[SwitchMetalCommandBuffer class], @protocol(MTLCommandBuffer)},
        {[SwitchMetalEncoder class], @protocol(MTLCommandEncoder)},
        {[SwitchMetalRenderEncoder class], @protocol(MTLRenderCommandEncoder)},
        {[SwitchMetalParallelRenderEncoder class], @protocol(MTLParallelRenderCommandEncoder)},
        {[SwitchMetalIndirectCommandBuffer class], @protocol(MTLIndirectCommandBuffer)},
        {[SwitchMetalIndirectRenderCommand class], @protocol(MTLIndirectRenderCommand)},
        {[SwitchMetalBlitEncoder class], @protocol(MTLBlitCommandEncoder)},
        {[SwitchMetalComputeEncoder class], @protocol(MTLComputeCommandEncoder)},
    };
    for (const auto &[c, protocol] : classes)
        class_addProtocol(c, protocol);
    GM20BState *r = new GM20BState;
    r->raw = dev;
    r->sharedEvents = dev->sharedEvents();
    r->registryID = registryID;
    // Before the device object: SwitchMetalDevice's answers (name, maxBufferLength) read the kernel's info through gRM, and its init
    // may ask them
    gRM = r;
    r->device = [cls new];
    fillDeviceLimits(r->device);
    r->handlers = followingSerialQueue("com.brunostjohn.switch.gm20b.handlers");
    r->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:r->handlers];
    gRawRelease.start(
        [](void *) {
            gm20bReleaseAhead(gRM->raw->hasBoundVolatileArena());
            const SwitchMetalPassTrim passes = gm20bPassTrim();
            // freed Shared spans zeroed and back in their chunks first, so an idle chunk they were in can park: the
            // submit worker submits their job, and this thread waits for it, not the worker
            if (gRM->raw->scrubDue()) {
                uint32_t seqno = 0;
                uint64_t generation = 0;
                if (gm20bExcludingSubmissions([&] {
                        return gRM->raw->scrubFreed(seqno, generation);
                    }))
                    gRM->raw->scrubWait(seqno, generation);
            }
            const gm20b::Device::Trimmed kept = gRM->raw->trim();
            gRM->raw->kernelSparesGiveBack();
#if SWITCH_METAL_STATS
            gm20b::Device::PurgeStats purge;
            gRM->raw->purgeArenas(gRM->committed, &purge);
#else
            gRM->raw->purgeArenas(gRM->committed);
#endif
            gRM->raw->trimArenas(gRM->committed);
            releaseRecordings();
            gm20bTrimCompiledLibraries();
            if (gRM->completion)
                gRM->completion->trim();
            // A daemon over its inactive jetsam limit is killed when it goes inactive, without a pressure notification that
            // would have made malloc give back the dirty pages its zones keep free (a compile's working memory)
            const size_t relieved = malloc_zone_pressure_relief(nullptr, 0);
#if SWITCH_METAL_STATS
            logGivenBack(relieved, passes, kept, purge);
#else
            (void)relieved;
            (void)passes;
            (void)kept;
#endif
        },
        nullptr);
    const bool renderServer = dev->role() == gm20b::Device::Role::RenderServer;
    if (!renderServer)
        observeBackground();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    {  // the stall line's timer (gm20bStallTick): one a process, on a serial queue of its own, which alone runs the tick
        // and so owns its state. Nothing the render server's other threads hold delays it; another process's rides the
        // wakeups it takes anyway. On a global queue a source's ticks are kept apart only inside libdispatch, where
        // ThreadSanitizer cannot see it.
        static dispatch_source_t stall;
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            dispatch_queue_t ticks = dispatch_queue_create(
                "com.brunostjohn.switch.gm20b.stall",
                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
            stall = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, ticks);
            dispatch_source_set_timer(stall, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC,
                                      renderServer ? NSEC_PER_SEC / 4 : 5 * NSEC_PER_SEC);
            dispatch_source_set_event_handler(stall, ^{
                gm20bStallTick();
            });
            dispatch_resume(stall);
        });
    }
    watchFetchBarrierWay(dev);
    watchDebugSwitches(dev);
    watchFrameWeights(dev);
#endif
    // what both threads hold up is the frame: they follow its committing threads (follow.h)
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.gm20b.submit");
        gm20b::followers().join();
        submitLoop();
    }).detach();
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.gm20b.completion");
        gm20b::followers().join();
        completionLoop();
    }).detach();
    return (id<MTLDevice>)r->device;
}
