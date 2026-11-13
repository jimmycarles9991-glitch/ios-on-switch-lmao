// SPDX-License-Identifier: MIT
// The process's m2v device, opened at its first need (RawM2V): a process that draws only states the pack holds never opens
// one, and keeps none of its memory (its NVK device, its runtime, its pools). Every way into it is rmM2V, which names the
// need; the device selectors only m2v's device class has a method for come here through the device class's forwarders.
#import "RawMetalInternal.h"

namespace {

// Held only across the opener, which calls nothing of the facade: a leaf under any lock its callers hold
raw::Mutex gOpenLock;
bool gOpenTried;
thread_local bool tOpening;

// The device class and its selectors that open m2v (installM2VForwarders): written before the class is registered, read only
// after
Class gDeviceClass;
std::unordered_map<SEL, std::string> *gForwarded;

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
struct Needs {
    raw::Mutex lock;
    std::map<std::string, uint64_t> counts;
    std::string opener;  // the need that opened it
    uint64_t opens = 0, openUs = 0;
};

Needs &needs()
{
    static Needs *n = new Needs;
    return *n;
}
#endif

// Inside gOpenLock, before any other thread can see `m2v`: the frontend's surface imports become the raw path's, and each
// forwarder runs m2v's own method on the raw device from now on, as the subclass of m2v's class did
void adopt(RawState &r, id<MTLDevice> m2v)
{
    rawAliasSurfaces(r.raw);
    Class from = object_getClass(m2v);
    for (const auto &[sel, need] : *gForwarded) {
        (void)need;
        const IMP imp = class_getMethodImplementation(from, sel);
        Method forwarder = class_getInstanceMethod(gDeviceClass, sel);
        if (imp && imp != _objc_msgForward && forwarder)
            method_setImplementation(forwarder, imp);
    }
    if (!r.gpuOrder)
        ::once(@"CPU-ordered: the kernel has no Order (TegraGM20BInfoOrder): raw and m2v command buffers wait for each "
               @"other's work to complete");
}

}  // namespace

id<MTLDevice> rmM2VIfOpen()
{
    return gRM && gRM->m2vOpened.load(std::memory_order_acquire) ? gRM->fallback : nil;
}

id<MTLDevice> rmM2V(const char *need)
{
    if (!gRM)
        return nil;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    {
        Needs &n = needs();
        std::lock_guard<raw::Mutex> l(n.lock);
        n.counts[need]++;
    }
#endif
    if (gRM->m2vOpened.load(std::memory_order_acquire))
        return gRM->fallback;
    if (!gRM->m2v.open || tOpening)
        return nil;
    std::lock_guard<raw::Mutex> l(gOpenLock);
    if (gRM->m2vOpened.load(std::memory_order_relaxed))
        return gRM->fallback;
    if (gOpenTried)
        return nil;
    gOpenTried = true;
    const uint64_t began = nowUs();
    tOpening = true;
    id<MTLDevice> m2v = gRM->m2v.open();
    tOpening = false;
    if (!m2v)
        return nil;
    gRM->fallback = m2v;
    adopt(*gRM, m2v);
    gRM->m2vOpened.store(true, std::memory_order_release);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    Needs &n = needs();
    std::lock_guard<raw::Mutex> m(n.lock);
    n.opens++;
    n.openUs = nowUs() - began;
    n.opener = need;
    os_log(rawLog(), "raw: %{public}s opened its m2v device for %{public}s in %llu ms", getprogname(), need,
           (unsigned long long)(n.openUs / 1000));
#else
    (void)began;
#endif
    return m2v;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
std::string rmM2VState()
{
    Needs &n = needs();
    std::lock_guard<raw::Mutex> l(n.lock);
    if (!n.opens)
        return "m2v device never opened";
    return "m2v device opened for " + n.opener + " in " + std::to_string(n.openUs / 1000) + " ms";
}

id<MTLDevice> rmM2VFor(const char *need, std::atomic<bool> &said)
{
    if (!said.exchange(true, std::memory_order_relaxed))
        os_log(rawLog(), "raw: %{public}s needs its m2v device for %{public}s", getprogname(), need);
    return rmM2V(need);
}
#endif

void installM2VForwarders(Class cls, const RawM2V &m2v)
{
    gDeviceClass = cls;
    gForwarded = new std::unordered_map<SEL, std::string>;
    std::set<SEL> own;
    unsigned count = 0;
    Method *methods = class_copyMethodList(cls, &count);
    for (unsigned i = 0; i < count; i++)
        own.insert(method_getName(methods[i]));
    free(methods);
    auto forward = [&](SEL sel, const char *types) {
        if (own.count(sel) || !types)
            return;
        own.insert(sel);
        class_addMethod(cls, sel, _objc_msgForward, types);
        gForwarded->emplace(sel, std::string("m2v's -[MTLDevice ") + sel_getName(sel) + "]");
    };
    if (m2v.methods) {
        methods = class_copyMethodList(m2v.methods, &count);
        for (unsigned i = 0; i < count; i++)
            forward(method_getName(methods[i]), method_getTypeEncoding(methods[i]));
        free(methods);
    }
    // the bridge's answer kinds (NativeMetalObjects.h NmbAnswerKind): count, flag, and each with an argument
    static const char *const kAnswerTypes[] = {"Q16@0:8", "B16@0:8", "Q24@0:8Q16", "B24@0:8Q16"};
    uint32_t kind = 0;
    for (size_t i = 0; m2v.answerSelector; i++) {
        const char *name = m2v.answerSelector(i, &kind);
        if (!name)
            break;
        forward(sel_registerName(name), kind < std::size(kAnswerTypes) ? kAnswerTypes[kind] : nullptr);
    }
}

@interface RMDevice (M2V)
@end
@implementation RMDevice (M2V)

// A forwarder's selector opens the m2v device and runs m2v's method (adopt swapped it in); any other selector a Metal protocol
// declares is answered zero, as m2v's device answered what it lacked; anything else is not recognized
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    if (Method m = class_getInstanceMethod(object_getClass(self), selector))
        return [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(m)];
    if (const char *types = metalTypes(selector))
        return [NSMethodSignature signatureWithObjCTypes:types];
    return nil;
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
    const SEL selector = invocation.selector;
    if (gForwarded) {
        const auto forwarded = gForwarded->find(selector);
        if (forwarded != gForwarded->end() && rmM2V(forwarded->second.c_str()) &&
            class_getMethodImplementation(object_getClass(self), selector) != _objc_msgForward) {
            [invocation invoke];
            return;
        }
    }
    answerZero(self, invocation);
}

- (void)doesNotRecognizeSelector:(SEL)selector
{
    SWITCH_REFUSE_DETAIL("raw", "device.selector", uintptr_t(selector), "-[MTLDevice %s]: no method",
                         sel_getName(selector));
    const IMP base = class_getMethodImplementation(class_getSuperclass(object_getClass(self)), _cmd);
    reinterpret_cast<void (*)(id, SEL, SEL)>(base)(self, _cmd, selector);
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's and the host harness's: whether this process opened its m2v device, for which need, in how long, and every
// need asked of it since the device opened (each a count), whether or not it was opened for it
- (NSDictionary *)switchRawM2V
{
    Needs &n = needs();
    std::lock_guard<raw::Mutex> l(n.lock);
    NSMutableDictionary *asked = [NSMutableDictionary dictionary];
    for (const auto &[need, times] : n.counts)
        asked[[NSString stringWithUTF8String:need.c_str()]] = @(times);
    return @{
        @"opened" : @(rmM2VIfOpen() != nil),
        @"opens" : @(n.opens),
        @"open_us" : @(n.openUs),
        @"opened_for" : [NSString stringWithUTF8String:n.opener.c_str()],
        @"needs" : asked,
        @"forwarders" : @(gForwarded ? gForwarded->size() : 0)
    };
}
#endif

@end
