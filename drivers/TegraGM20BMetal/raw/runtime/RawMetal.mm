// SPDX-License-Identifier: MIT
// See RawMetal.h. r7-raw wrote the first facade (render encoder, pipeline key); r8-rawca made it the render server's device: the
// m2v twins, the recorded command buffer and its replay on m2v, the order between the two drivers, the copies, the events,
// the lifetimes.
//
// A command buffer here is a tape: each encoder call appends a record (the bytes of setBytes copied in, every object retained by
// the command buffer until it completes). A call the raw path cannot draw exactly as m2v marks the command buffer for m2v, with a
// reason. At commit, under the device's submit lock and in commit order:
//  - raw: the tape is encoded into the raw rings (raw_gpu.cpp) and submitted, one Exec. The encoder can still refuse a draw (a
//    uniform buffer at an unaligned offset, a texture without a header); the raw work is then abandoned (the rings rewound)
//    and the command buffer goes to m2v. The completion thread sleeps in the kernel on each fence (the interrupt), then
//    runs the handlers.
//  - m2v: the tape is replayed into a command buffer of the m2v device with every object's twin, CPU lines of the buffers it
//    reads cleaned, and committed once the raw work committed before it has completed (raw's fence, a kernel wait on the
//    interrupt). Its completion handler runs ours.
//  - a raw command buffer waits for the m2v ones committed before it, and for the shared events it waits on: such a command
//    buffer, and every one committed after it while it waits, goes to the submit thread, which submits them in order once
//    they may run; the render thread never blocks for it. waitUntilScheduled waits for the submission, so the display's
//    copy (which waits on the GPU for every channel that imports the surface) never runs before the frame's work is submitted.
#import "RawMetalInternal.h"

#include <mach/mach.h>

#if SWITCH_FRAME_TIMELINE
std::atomic<uint64_t> gSurfaceTextures{0}, gTwinsMade{0};  // the frame timeline's churn counts (SwitchFrameTimeline.h)
#endif

os_log_t rawLog()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "raw");
    return l;
}

[[noreturn]] void waitTooLong(const char *what, const void *object)
{
    os_log_fault(rawLog(), "raw: %{public}s %p not done after %lld s: abort() for the stack", what, object,
                 (long long)kWaitLimit.count());
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: %s %p not done after %lld s\n", what, object, (long long)kWaitLimit.count());
#endif
    abort();
}

// One line per distinct text, then silence (shipped paths: no diagnostics but one-shot lines).
void once(NSString *text)
{
    static std::mutex lock;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> l(lock);
        if (seen.size() > 512 || !seen.insert(text.UTF8String).second)
            return;
    }
    os_log_error(rawLog(), "raw: %{public}@", text);
#if !TARGET_OS_IPHONE
    fprintf(stderr, "raw: %s\n", text.UTF8String);  // host tests read their stderr
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

RawState *gRM;

@implementation RMObject

- (id)rmTwin
{
    return _twin;
}

- (id<MTLDevice>)device
{
    return (id<MTLDevice>)_device;
}

// The m2v class this object stands in for: its twin's, or the one it would make
- (Class)rmTwinClass
{
    id twin = _twin;
    return twin ? object_getClass(twin) : [[self class] twinClass];
}

// A selector this class does not answer is never forwarded to an m2v twin: it is answered zero here (forwardInvocation:). When
// m2v's class does answer it, raw is missing a method (raw/test/selcover finds none any kind sends): answerMissing says so. An
// RMTwinObject is the exception: it stands for the m2v object it carries.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (NSMethodSignature *s = [super methodSignatureForSelector:selector])
        return s;
    if (const char *types = metalTypes(selector))
        return [NSMethodSignature signatureWithObjCTypes:types];
    return [[self rmTwinClass] instanceMethodSignatureForSelector:selector];
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
    if (twinAnswers([self rmTwinClass], invocation.selector))
        answerMissing(self, invocation);
    else
        answerZero(self, invocation);
}

+ (Class)twinClass
{
    return nil;
}

@end

// Resource calls Core Animation makes on every texture and buffer that need no m2v twin (forwarding them would make one, an m2v
// import of memory the raw path draws from). Ownership is accounting only (the twin's, when it has one); no content is protected
// on this device; waitUntilComplete waits for the raw work submitted so far (the kernel's fence interrupt) and the twin's.
@interface RMObject (Resource)
@end
@implementation RMObject (Resource)

- (kern_return_t)setOwnerWithIdentity:(mach_port_t)identity
{
    id twin = _twin;
    if (twin && [twin respondsToSelector:_cmd])
        return ((kern_return_t (*)(id, SEL, mach_port_t))objc_msgSend)(twin, _cmd, identity);
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
    id twin = _twin;
    if (twin && [twin respondsToSelector:_cmd])
        ((void (*)(id, SEL))objc_msgSend)(twin, _cmd);
}

@end

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// What the process keeps once the GPU's idle memory went back: jetsam reads phys_footprint against the daemon's limits
// (JetsamProperties: iconservicesagent's ActiveSoft 200 MB, InactiveHard 15 MB); the rest of it is mostly IOKit's mappings
extern "C" bool native_bridge_metal_memory(uint64_t out[3]) __attribute__((weak_import));

static void logGivenBack()
{
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) != KERN_SUCCESS)
        return;
    raw::Device &d = *gRM->raw;
    uint64_t m2v[3] = {};
    if (native_bridge_metal_memory)
        native_bridge_metal_memory(m2v);
    const uint64_t footprint = vm.phys_footprint >> 20, internal = vm.internal >> 20, compressed = vm.compressed >> 20;
    os_log(rawLog(),
           "raw: idle memory given back: pool %llu MB in %llu chunks (heaps %llu MB), write-combined %llu MB, shader "
           "heap %llu KB, m2v %llu MB; footprint %llu MB (internal %llu, compressed %llu, other %llu)",
           (unsigned long long)(d.stats.poolBytes.load() >> 20), (unsigned long long)d.stats.poolChunks.load(),
           (unsigned long long)(d.arenaBytes() >> 20), (unsigned long long)(d.wcChunkBytesHeld() >> 20),
           (unsigned long long)(d.heapBytes() >> 10), (unsigned long long)(m2v[0] >> 20), (unsigned long long)footprint,
           (unsigned long long)internal, (unsigned long long)compressed,
           (unsigned long long)(footprint > internal + compressed ? footprint - internal - compressed : 0));
}
#endif

// raw_metal_prepare's thread and what it made (taken once, by raw_metal_device_open)
static struct {
    std::mutex lock;
    std::thread thread;
    std::string path, err;
    uint64_t stack = 0;
    raw::Device *dev = nullptr;
    bool started = false;
} gPrepared;

static raw::Device *openDevice(const char *packPath, uint64_t stack, raw::Device::Role role, std::string &err)
{
    raw::Device *dev = new raw::Device;
    if (!dev->open(packPath, stack, err, role)) {
        delete dev;  // its connection closes: the kernel frees what it held
        return nullptr;
    }
    if (role == raw::Device::Role::Client)
        return dev;
    dev->warm();
    return dev;
}

void raw_metal_prepare(const char *packPath, uint64_t stack)
{
    std::lock_guard<std::mutex> l(gPrepared.lock);
    if (gPrepared.started)
        return;
    gPrepared.started = true;
    gPrepared.path = packPath;
    gPrepared.stack = stack;
    gPrepared.thread = std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.raw.open");
        std::string err;
        raw::Device *dev = openDevice(gPrepared.path.c_str(), gPrepared.stack, raw::Device::Role::RenderServer, err);
        std::lock_guard<std::mutex> m(gPrepared.lock);
        gPrepared.dev = dev;
        gPrepared.err = err;
    });
}

void raw_metal_complete_states(const char *cacheDir)
{
    if (gRM && gRM->fallback && !gRM->completion)
        gRM->completion = std::make_unique<raw::Completion>(*gRM->raw, cacheDir, gRM->raw->pack().header()->stack);
}

// libdispatch SPI (dispatch/private/queue_private.h): a root queue over threads of its own, `configure` run on each as it
// starts.
extern "C" dispatch_queue_global_t dispatch_pthread_root_queue_create(const char *label, unsigned long flags,
                                                                      const pthread_attr_t *attr,
                                                                      dispatch_block_t configure);

// Scheduled handlers hold up the frame as the submit and completion threads do (Core Animation's render thread waits for
// them in waitUntilScheduled), so they run on threads that follow the committers (raw_follow.h), not on a QoS queue's
// workers, which XNU clamps to 37 in a daemon: below the fixed-priority render thread waiting on them.
static dispatch_queue_t followingSerialQueue(const char *label)
{
    static const dispatch_queue_global_t root =
        dispatch_pthread_root_queue_create("com.brunostjohn.switch.raw.following", 0, nullptr, ^{
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            raw::followers().join();
        });
    const dispatch_queue_t queue =
        dispatch_queue_create(label, dispatch_queue_attr_make_initially_inactive(DISPATCH_QUEUE_SERIAL));
    dispatch_set_target_queue(queue, root);
    dispatch_activate(queue);
    return queue;
}

id<MTLDevice> raw_metal_device_open(const char *packPath, uint64_t stack, id<MTLDevice> fallback, NSString **error)
{
    static std::mutex once;
    std::lock_guard<std::mutex> l(once);
    if (gRM)
        return gRM->device;  // one per process
    raw::Device *dev = nullptr;
    std::string err;
    bool prepared = false;
    {
        std::unique_lock<std::mutex> m(gPrepared.lock);
        if (gPrepared.started && gPrepared.thread.joinable()) {
            m.unlock();
            gPrepared.thread.join();  // what is left of it: it began with the plugin, before the m2v device's open
            m.lock();
        }
        if (gPrepared.started && gPrepared.path == packPath && gPrepared.stack == stack) {
            prepared = true;
            dev = gPrepared.dev;
            err = gPrepared.err;
            gPrepared.dev = nullptr;
        }
    }
    if (!prepared)
        dev = openDevice(packPath, stack, raw::Device::Role::Client, err);
    if (dev && fallback && !dev->shared()) {
        err = "the kernel connection is not NVK's, so m2v cannot alias the raw path's memory";
        delete dev;
        dev = nullptr;
    }
    if (!dev) {
        if (error)
            *error = [NSString stringWithUTF8String:err.c_str()];
        return nil;
    }
    // The device class: a subclass of the m2v device's own (an _MTLDevice conforming to MTLDevice and MTLDeviceSPI on the iPad),
    // with no instance variables of its own, and the facade's methods over the inherited ones
    Class base = fallback ? object_getClass(fallback) : [NSObject class];
    Class cls = objc_allocateClassPair(base, "RawGM20BMetalDevice", 0);
    if (!cls) {
        if (error)
            *error = @"the device class could not be made";
        delete dev;
        return nil;
    }
    for (Class from : {[RMDevice class]}) {
        unsigned count = 0;
        Method *methods = class_copyMethodList(from, &count);
        for (unsigned i = 0; i < count; i++)
            class_addMethod(cls, method_getName(methods[i]), method_getImplementation(methods[i]),
                            method_getTypeEncoding(methods[i]));
        free(methods);
    }
    installDeviceAnswers(cls);
    objc_registerClassPair(cls);
    // Each facade class conforms to its Metal protocol, as the m2v objects it stands in for do (the frontend's adoptMetalProtocols).
    const std::pair<Class, Protocol *> classes[] = {
        {[RMBuffer class], @protocol(MTLBuffer)},
        {[RMTexture class], @protocol(MTLTexture)},
        {[RMLibrary class], @protocol(MTLLibrary)},
        {[RMFunction class], @protocol(MTLFunction)},
        {[RMHeap class], @protocol(MTLHeap)},
        {[RMSampler class], @protocol(MTLSamplerState)},
        {[RMPipeline class], @protocol(MTLRenderPipelineState)},
        {[RMQueue class], @protocol(MTLCommandQueue)},
        {[RMCommandBuffer class], @protocol(MTLCommandBuffer)},
        {[RMEncoder class], @protocol(MTLCommandEncoder)},
        {[RMRenderEncoder class], @protocol(MTLRenderCommandEncoder)},
        {[RMBlitEncoder class], @protocol(MTLBlitCommandEncoder)},
        {[RMComputeEncoder class], @protocol(MTLComputeCommandEncoder)},
    };
    for (const auto &[c, protocol] : classes)
        class_addProtocol(c, protocol);
    RawState *r = new RawState;
    r->raw = dev;
    r->fallback = fallback;
    r->device = [cls new];
    // Before the limits fill: RMDevice's answers (name, maxBufferLength) read the kernel's info through gRM
    gRM = r;
    // The base class's limits table (_MTLDevice's own accessors read it; the frontend's fillLimits filled the m2v device's from its
    // answers): the same for this device, then this device's own answers (RMDevice's) where the base class reads them there.
    static const SEL limits = NSSelectorFromString(@"limits");
    if (fallback && [fallback respondsToSelector:limits] && [r->device respondsToSelector:limits]) {
        const void *from = ((const void *(*)(id, SEL))objc_msgSend)(fallback, limits);
        void *to = ((void *(*)(id, SEL))objc_msgSend)(r->device, limits);
        if (from && to && from != to) {
            memcpy(to, from, kMTLDeviceLimitsBytes);
            fillLimitsFromMethods(r->device, static_cast<uint8_t *>(to), [](const LimitField &field, LimitFill how) {
                if (how != LimitFill::Filled)
                    SWITCH_REFUSE_DETAIL("raw", "limits.layout", field.offset,
                                         "limits: %s not written at 0x%x (the base class does not read it there)",
                                         field.selector, field.offset);
            });
        }
    }
    r->handlers = followingSerialQueue("com.brunostjohn.switch.raw.handlers");
    r->rawSignals = [NSMapTable weakToStrongObjectsMapTable];
    r->m2vSignals = [NSMapTable weakToStrongObjectsMapTable];
    r->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:r->handlers];
    r->gpuOrder = dev->canOrder();
    if (!r->gpuOrder && fallback)
        ::once(@"CPU-ordered: the kernel has no Order (TegraGM20BInfoOrder): raw and m2v command buffers wait for each "
               @"other's "
               @"work to complete");
    if (fallback)
        rawAliasSurfaces(dev);
    gRawRelease.start(
        [](void *) {
            gRM->raw->trim();
            gRM->raw->purgeArenas(gRM->committed);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            logGivenBack();
#endif
        },
        nullptr);
    const bool renderServer = dev->role() == raw::Device::Role::RenderServer;
    if (renderServer)
        os_log(rawLog(), "raw: shader heap %llu KB, each program placed at its state's first use",
               (unsigned long long)(dev->heapBytes() >> 10));
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    {  // the stall line's timer (rawStallTick): its own, so nothing the render server's other threads hold delays it; another
        // process's rides the wakeups it takes anyway
        static dispatch_source_t stall;
        stall =
            dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(stall, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC,
                                  renderServer ? NSEC_PER_SEC / 4 : 5 * NSEC_PER_SEC);
        dispatch_source_set_event_handler(stall, ^{
            rawStallTick();
        });
        dispatch_resume(stall);
    }
    watchFetchBarrierWay(dev);
#endif
    // what both threads hold up is the frame: they follow its committing threads (raw_follow.h)
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.raw.submit");
        raw::followers().join();
        submitLoop();
    }).detach();
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("com.brunostjohn.switch.raw.completion");
        raw::followers().join();
        completionLoop();
    }).detach();
    return (id<MTLDevice>)r->device;
}
