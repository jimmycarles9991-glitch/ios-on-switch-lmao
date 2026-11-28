// Host stand-ins for the kernel interfaces drivers/SwitchTouch/SwitchTouch.cpp and drivers/TegraI2C/TegraI2C.cpp use
// (IOKit event sources and work loops, IOLock sleep/wakeup, os_log, mach time), for tests/switch_touch_harness.cpp.
// test_switch_touch.py points <IOKit/...>, <kern/clock.h>, <os/log.h>, <pexpert/pexpert.h> and <libkern/OSAtomic.h>
// here. Each driver is its own translation unit (both have an anonymous-namespace kBus), so everything here is inline.
//
// One thread plays every kernel thread: the harness's event queue (gSchedule) runs model events, interrupt actions and
// wake-ups in time order. Time is a fake clock in nanoseconds (mach ticks are nanoseconds here). A thread that sleeps
// (IOLockSleepDeadline, IOSleep, IODelay) runs the queue until it is woken or its time is up, so the I2C controller's
// interrupt actions run nested inside the touch work loop's wait, as they would on the I2C work loop's own thread.
#ifndef SWITCH_TEST_SWITCH_TOUCH_STUBS_H
#define SWITCH_TEST_SWITCH_TOUCH_STUBS_H
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

typedef int IOReturn;
typedef uint32_t IOOptionBits;
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int boolean_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNoMemory = (IOReturn)0xe00002bd;
constexpr IOReturn kIOReturnBadArgument = (IOReturn)0xe00002c2;
constexpr IOReturn kIOReturnUnsupported = (IOReturn)0xe00002c7;
constexpr IOReturn kIOReturnNotReady = (IOReturn)0xe00002d8;

// --- Time and the event queue ---------------------------------------------------------------------------------------
inline uint64_t gNow = 0;

inline uint64_t mach_absolute_time()
{
    return gNow;
}

inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs;
}

inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs)
{
    *abs = ns;
}

enum {
    kNanosecondScale = 1,
    kMicrosecondScale = 1000,
    kMillisecondScale = 1000000,
    kSecondScale = 1000000000
};

inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = gNow + (uint64_t)interval * scale;
}

// The harness's scheduler: runs `what` at `when` (never before gNow). gRunUntil runs every event due by `until` in time
// order and leaves gNow there; it returns early (true) once `stop` says so.
inline std::multimap<uint64_t, std::function<void()>> gQueue;

inline void gSchedule(uint64_t when, std::function<void()> what)
{
    gQueue.emplace(when < gNow ? gNow : when, std::move(what));
}

inline std::function<void()> gAfterEvent;  // the harness's line checks after each event (interrupt delivery)

inline bool gRunUntil(uint64_t until, const std::function<bool()> &stop = nullptr)
{
    for (;;) {
        if (gAfterEvent)
            gAfterEvent();  // a register write may have raised a line since the last event
        if (stop && stop())
            return true;
        auto first = gQueue.begin();
        if (first == gQueue.end() || first->first > until)
            break;
        gNow = first->first;
        auto what = std::move(first->second);
        gQueue.erase(first);
        what();
        if (gAfterEvent)
            gAfterEvent();
    }
    if (until > gNow)
        gNow = until;
    return stop && stop();
}

inline void IODelay(unsigned us)
{
    gRunUntil(gNow + us * 1000ull);
}

inline void IOSleep(unsigned ms)
{
    gRunUntil(gNow + ms * 1000000ull);
}

// --- Logs and panics ------------------------------------------------------------------------------------------------
inline std::vector<std::string> gLog;

inline void stubLog(const char *channel, const char *format, va_list ap)
{
    char line[2048];
    vsnprintf(line, sizeof(line), format, ap);
    std::string text = std::string(channel) + line;
    if (!text.empty() && text.back() == '\n')
        text.pop_back();
    gLog.push_back(text);
}

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    stubLog("IOLog: ", format, ap);
    va_end(ap);
}

__attribute__((format(printf, 1, 2))) inline void stubOsLog(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    stubLog("os_log: ", format, ap);
    va_end(ap);
}

#define OS_LOG_DEFAULT   nullptr
#define os_log(log, ...) stubOsLog(__VA_ARGS__)

struct Panic : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] __attribute__((format(printf, 1, 2))) inline void panic(const char *format, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw Panic(line);
}

inline bool PE_parse_boot_argn(const char *, void *, int)
{
    return false;
}

inline bool PE_parse_boot_arg_str(const char *, char *, int)
{
    return false;
}

inline bool OSCompareAndSwapPtr(void *oldValue, void *newValue, void *volatile *address)
{
    if (*address != oldValue)
        return false;
    *address = newValue;
    return true;
}

template <typename T> inline bool OSCompareAndSwapPtr(void *oldValue, T *newValue, T *volatile *address)
{
    if (*address != oldValue)
        return false;
    *address = newValue;
    return true;
}

// --- Objects ----------------------------------------------------------------------------------------------------
#define OSDeclareDefaultStructors(cls)
#define OSDefineMetaClassAndStructors(cls, super)
#define OSTypeAlloc(cls)        (new cls)
#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
#define OSSafeReleaseNULL(obj) \
    do {                       \
        if (obj) {             \
            (obj)->release();  \
            (obj) = nullptr;   \
        }                      \
    } while (0)

class OSObject {
    int refs = 1;

public:
    virtual ~OSObject() = default;

    virtual void free()
    {}

    void retain()
    {
        ++refs;
    }

    void release()
    {
        if (--refs == 0) {
            free();
            delete this;
        }
    }
};

class IOService : public OSObject {
public:
    IOService *provider = nullptr;

    virtual bool init()
    {
        return true;
    }

    virtual bool start(IOService *)
    {
        return true;
    }

    void free() override
    {}

    bool attach(IOService *to)
    {
        provider = to;
        retain();
        return true;
    }

    static IOService *getResourceService()
    {
        static IOService *resources = new IOService;
        return resources;
    }
};

class IOEventSource : public OSObject {
public:
    OSObject *owner = nullptr;
    bool enabled = false;

    virtual void enable()
    {
        enabled = true;
    }

    virtual void disable()
    {
        enabled = false;
    }
};

class IOWorkLoop : public OSObject {
public:
    typedef IOReturn (*Action)(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    static inline IOWorkLoop *last = nullptr;
    static inline unsigned created = 0;

    static IOWorkLoop *workLoop()
    {
        auto *wl = new IOWorkLoop;
        last = wl;
        ++created;
        return wl;
    }

    IOReturn addEventSource(IOEventSource *)
    {
        return kIOReturnSuccess;
    }

    IOReturn removeEventSource(IOEventSource *)
    {
        return kIOReturnSuccess;
    }

    IOReturn runAction(Action action, OSObject *target, void *a0 = nullptr, void *a1 = nullptr, void *a2 = nullptr,
                       void *a3 = nullptr)
    {
        return action(target, a0, a1, a2, a3);
    }
};

// Interrupt event sources. A source with a provider is a level line the harness delivers (its own line checks, after
// the work loop's latency); a provider-less one is software-triggered: interruptOccurred() runs its action on the work
// loop gWorkLoopUs later, as IOInterruptEventSource::normalInterruptOccurred's signal would.
inline uint64_t gWorkLoopNs = 40000;  // primary interrupt to work-loop action (r7z display stats: fe-late 35-47 us)

class IOInterruptEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *owner, IOInterruptEventSource *sender, int count);
    Action action = nullptr;
    IOService *provider = nullptr;
    unsigned actions = 0;
    static inline std::vector<IOInterruptEventSource *> all;

    static IOInterruptEventSource *interruptEventSource(OSObject *owner, Action action, IOService *provider = nullptr,
                                                        int = 0)
    {
        auto *source = new IOInterruptEventSource;
        source->owner = owner;
        source->action = action;
        source->provider = provider;
        all.push_back(source);
        return source;
    }

    void run()
    {
        ++actions;
        action(owner, this, 1);
    }

    void interruptOccurred(void *, IOService *, int)
    {
        gSchedule(gNow + gWorkLoopNs, [this] {
            if (enabled)
                run();
        });
    }
};

class IOFilterInterruptEventSource : public IOInterruptEventSource {
public:
    typedef bool (*Filter)(OSObject *owner, IOFilterInterruptEventSource *sender);
    Filter filter = nullptr;
    static inline std::vector<IOFilterInterruptEventSource *> filters;

    static IOFilterInterruptEventSource *filterInterruptEventSource(OSObject *owner, Action action, Filter filter,
                                                                    IOService *provider = nullptr, int = 0)
    {
        auto *source = new IOFilterInterruptEventSource;
        source->owner = owner;
        source->action = action;
        source->filter = filter;
        source->provider = provider;
        filters.push_back(source);
        return source;
    }
};

class IOTimerEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *owner, IOTimerEventSource *sender);
    Action action = nullptr;
    bool armed = false;
    uint32_t ms = 0;
    static inline IOTimerEventSource *last = nullptr;

    static IOTimerEventSource *timerEventSource(OSObject *owner, Action action)
    {
        auto *source = new IOTimerEventSource;
        source->owner = owner;
        source->action = action;
        last = source;
        return source;
    }

    IOReturn setTimeoutMS(uint32_t timeout)
    {
        armed = true;
        ms = timeout;
        return kIOReturnSuccess;
    }

    void cancelTimeout()
    {
        armed = false;
    }
};

// --- Locks ------------------------------------------------------------------------------------------------------
// A non-recursive lock (a second holder fails the test). Sleep releases it while the queue runs; a wakeup reaches the
// sleeper gWakeNs later (the scheduler's thread wake).
typedef void *event_t;
typedef void *thread_t;
typedef int wait_result_t;
typedef uint32_t wait_interrupt_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define THREAD_CONTINUE_NULL static_cast<thread_continue_t>(nullptr)

enum {
    THREAD_UNINT = 0,
    THREAD_WAITING = -1,
    THREAD_AWAKENED = 0,
    THREAD_TIMED_OUT = 1
};

inline uint64_t gWakeNs = 30000;

struct IOLock {
    bool held = false;
};

inline std::map<void *, uint64_t> gWakeups;  // event -> when its sleeper runs again
inline event_t gWaitEvent = nullptr;
inline uint64_t gWaitDeadline = 0;

inline thread_t current_thread()
{
    return nullptr;
}

inline wait_result_t assert_wait_deadline(event_t event, wait_interrupt_t, uint64_t deadline)
{
    gWaitEvent = event;
    gWaitDeadline = deadline;
    gWakeups.erase(event);
    return THREAD_WAITING;
}

inline void clear_wait(thread_t, wait_result_t)
{
    gWaitEvent = nullptr;
}

inline void thread_wakeup(event_t event)
{
    gWakeups[event] = gNow + gWakeNs;
    gSchedule(gNow + gWakeNs, [] {});
}

inline wait_result_t thread_block(thread_continue_t)
{
    event_t event = gWaitEvent;
    const uint64_t deadline = gWaitDeadline;
    gWaitEvent = nullptr;
    const bool woken = gRunUntil(deadline, [event] {
        const auto it = gWakeups.find(event);
        return it != gWakeups.end() && it->second <= gNow;
    });
    if (woken)
        gWakeups.erase(event);
    return woken ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    if (lock->held)
        throw std::logic_error("IOLockLock: already held (deadlock)");
    lock->held = true;
}

inline bool IOLockTryLock(IOLock *lock)
{
    if (lock->held)
        return false;
    lock->held = true;
    return true;
}

inline void IOLockUnlock(IOLock *lock)
{
    if (!lock->held)
        throw std::logic_error("IOLockUnlock: not held");
    lock->held = false;
}

inline void IOLockWakeup(IOLock *, void *event, bool)
{
    gWakeups[event] = gNow + gWakeNs;
    gSchedule(gNow + gWakeNs, [] {});  // the sleeper's turn is an event of its own
}

inline int IOLockSleepDeadline(IOLock *lock, void *event, uint64_t deadline, uint32_t)
{
    IOLockUnlock(lock);
    gWakeups.erase(event);
    const bool woken = gRunUntil(deadline, [event] {
        const auto it = gWakeups.find(event);
        return it != gWakeups.end() && it->second <= gNow;
    });
    if (woken)
        gWakeups.erase(event);
    IOLockLock(lock);
    return woken ? THREAD_AWAKENED : THREAD_TIMED_OUT;
}
#endif
