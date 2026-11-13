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
#include "raw_lock.h"
#import "RawMetalIndirect.h"
#include "raw_device_answers.h"
#include "raw_limits_fill.h"

#include <dlfcn.h>
#include <malloc/malloc.h>

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
    static raw::Mutex lock;
    static std::set<std::string> seen;
    {
        std::lock_guard<raw::Mutex> l(lock);
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

// raw_metal_prepare's thread and what it made (taken once, by raw_metal_device_open)
static struct {
    raw::Mutex lock;
    std::thread thread;
    std::string path, err;
    uint64_t stack = 0;
    raw::Device *dev = nullptr;
    bool started = false;
} gPrepared;

static raw::Device *openDevice(const char *packPath, uint64_t stack, raw::Device::Role role, std::string &err)
{
    raw::Device *dev = new raw::Device;
    dev->indexZeroPastLimit = raw::answers::chipFeature(SWITCH_GM20B_FEATURE_3D_B11);
    dev->heapHolds(rmHeapHolds);
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
    std::lock_guard<raw::Mutex> l(gPrepared.lock);
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
        std::lock_guard<raw::Mutex> m(gPrepared.lock);
        gPrepared.dev = dev;
        gPrepared.err = err;
    });
}

void raw_metal_complete_states(const char *cacheDir, const char *nativeSeed)
{
    if (gRM && gRM->m2v.open && !gRM->completion)
        gRM->completion =
            std::make_unique<raw::Completion>(*gRM->raw, cacheDir, gRM->raw->pack().header()->stack, nativeSeed);
}

// libdispatch SPI (dispatch/private/queue_private.h): a root queue over threads of its own, `configure` run on each as it
// starts.
extern "C" dispatch_queue_global_t dispatch_pthread_root_queue_create(const char *label, unsigned long flags,
                                                                      const pthread_attr_t *attr,
                                                                      dispatch_block_t configure);

// Scheduled handlers hold up the frame as the submit and completion threads do (Core Animation's render thread waits for
// them in waitUntilScheduled), so they run on threads that follow the committers (raw_follow.h), not on a QoS queue's
// workers, which XNU clamps to 37 in a daemon: below the fixed-priority render thread waiting on them.
dispatch_queue_t followingRootQueue()
{
    static const dispatch_queue_global_t root =
        dispatch_pthread_root_queue_create("com.brunostjohn.switch.raw.following", 0, nullptr, ^{
            pthread_setname_np("com.brunostjohn.switch.raw.following");
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            raw::followers().join();
        });
    return root;
}

dispatch_queue_t followingSerialQueue(const char *label)
{
    const dispatch_queue_t queue =
        dispatch_queue_create(label, dispatch_queue_attr_make_initially_inactive(DISPATCH_QUEUE_SERIAL));
    dispatch_set_target_queue(queue, followingRootQueue());
    dispatch_activate(queue);
    return queue;
}

// The device class: a subclass of _MTLDevice, as the frontend's m2v device class is (conforming to MTLDevice and MTLDeviceSPI,
// with no instance variables of its own: the base class's ABI is the IPSW kind's), with the facade's methods, the kind's
// capability answers and a forwarder for each method only m2v's device class has
static Class deviceClass(const RawM2V &m2v, NSString **error)
{
    Class base = NSClassFromString(@"_MTLDevice") ?: [NSObject class];
#if TARGET_OS_IPHONE
    if (class_getInstanceSize(base) != kMTLDeviceInstanceSize) {
        if (error)
            *error = @"unexpected _MTLDevice ABI";
        return nil;
    }
#endif
    Class cls = objc_allocateClassPair(base, "RawGM20BMetalDevice", 0);
    if (!cls) {
        if (error)
            *error = @"the device class could not be made";
        return nil;
    }
    unsigned count = 0;
    Method *methods = class_copyMethodList([RMDevice class], &count);
    for (unsigned i = 0; i < count; i++)
        class_addMethod(cls, method_getName(methods[i]), method_getImplementation(methods[i]),
                        method_getTypeEncoding(methods[i]));
    free(methods);
    installDeviceAnswers(cls);
    installSharedTextureMethods(cls);
    installM2VForwarders(cls, m2v);
    class_addProtocol(cls, @protocol(MTLDevice));
    // _MTLAddDevice asserts the private device protocol too, which the target's own drivers get from their IOGPU base class
    if (Protocol *spi = objc_getProtocol("MTLDeviceSPI"))
        class_addProtocol(cls, spi);
    objc_registerClassPair(cls);
    return cls;
}

id<MTLDevice> raw_metal_device_open(const char *packPath, uint64_t stack, id<MTLDevice> fallback, NSString **error)
{
    static const auto answerSelector =
        reinterpret_cast<const char *(*)(size_t, uint32_t *)>(dlsym(RTLD_DEFAULT, "nmb_device_answer_selector"));
    RawM2V m2v;
    if (fallback)
        m2v.open = ^{
            return fallback;
        };
    m2v.methods = NSClassFromString(@"NBDeviceMethods");
    m2v.answerSelector = answerSelector;
    static const SEL registry = NSSelectorFromString(@"registryID");
    const uint64_t registryID = [fallback respondsToSelector:registry] ? [fallback registryID] : 0;
    return raw_metal_device_open_lazy(packPath, stack, registryID, m2v, error);
}

id<MTLDevice> raw_metal_device_open_lazy(const char *packPath, uint64_t stack, uint64_t registryID, RawM2V m2v,
                                         NSString **error)
{
    static raw::Mutex once;
    std::lock_guard<raw::Mutex> l(once);
    if (gRM)
        return gRM->device;  // one per process
    rmPoolEncoders();
    rmPoolCommandBuffers();
    raw::Device *dev = nullptr;
    std::string err;
    bool prepared = false;
    {
        std::unique_lock<raw::Mutex> m(gPrepared.lock);
        if (gPrepared.started && gPrepared.thread.joinable()) {
            m.unlock();
            gPrepared.thread.join();  // what is left of it: it began with the plugin
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
    // m2v's views of the raw path's memory are made in the GPU VM the two drivers share: NVK's connection, whichever opens it
    // first (raw_kernel.h)
    if (dev && m2v.open && !dev->shared()) {
        err = "the kernel connection is not NVK's, so m2v cannot alias the raw path's memory";
        delete dev;
        dev = nullptr;
    }
    if (!dev) {
        if (error)
            *error = [NSString stringWithUTF8String:err.c_str()];
        return nil;
    }
    Class cls = deviceClass(m2v, error);
    if (!cls) {
        delete dev;
        return nil;
    }
    // Each facade class conforms to its Metal protocol, as the m2v objects it stands in for do (the frontend's adoptMetalProtocols).
    const std::pair<Class, Protocol *> classes[] = {
        {[RMBuffer class], @protocol(MTLBuffer)},
        {[RMTexture class], @protocol(MTLTexture)},
        {[RMLibrary class], @protocol(MTLLibrary)},
        {[RMFunction class], @protocol(MTLFunction)},
        {[RMHeap class], @protocol(MTLHeap)},
        {[RMSampler class], @protocol(MTLSamplerState)},
        {[RMDepthStencil class], @protocol(MTLDepthStencilState)},
        {[RMPipeline class], @protocol(MTLRenderPipelineState)},
        {[RMQueue class], @protocol(MTLCommandQueue)},
        {[RMCommandBuffer class], @protocol(MTLCommandBuffer)},
        {[RMEncoder class], @protocol(MTLCommandEncoder)},
        {[RMRenderEncoder class], @protocol(MTLRenderCommandEncoder)},
        {[RMParallelRenderEncoder class], @protocol(MTLParallelRenderCommandEncoder)},
        {[RMIndirectCommandBuffer class], @protocol(MTLIndirectCommandBuffer)},
        {[RMIndirectRenderCommand class], @protocol(MTLIndirectRenderCommand)},
        {[RMBlitEncoder class], @protocol(MTLBlitCommandEncoder)},
        {[RMComputeEncoder class], @protocol(MTLComputeCommandEncoder)},
    };
    for (const auto &[c, protocol] : classes)
        class_addProtocol(c, protocol);
    RawState *r = new RawState;
    r->raw = dev;
    r->sharedEvents = dev->sharedEvents();
    r->m2v = m2v;
    r->registryID = registryID;
    // Before the device object: RMDevice's answers (name, maxBufferLength) read the kernel's info through gRM, and its init
    // may ask them
    gRM = r;
    r->device = [cls new];
    fillDeviceLimits(r->device);
    r->gpuOrder = dev->canOrder();
    r->handlers = followingSerialQueue("com.brunostjohn.switch.raw.handlers");
    r->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:r->handlers];
    gRawRelease.start(
        [](void *) {
            gRM->raw->trim();
            gRM->raw->purgeArenas(gRM->committed);
            gRM->raw->trimArenas(gRM->committed);
            releaseRecordings();
            // A daemon over its inactive jetsam limit is killed when it goes inactive, without a pressure notification that
            // would have made malloc give back the dirty pages its zones keep free (a compile's working memory)
            const size_t relieved = malloc_zone_pressure_relief(nullptr, 0);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
            logGivenBack(relieved);
#else
            (void)relieved;
#endif
        },
        nullptr, &r->rangesInFlight);
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
    watchKeylineProbe();
    watchGlassProbe(dev);
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
