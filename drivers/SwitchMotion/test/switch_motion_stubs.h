// Host stand-ins for the kernel interfaces drivers/SwitchMotion/SwitchMotion.cpp uses (IOKit, os_log, mach time), for
// switch_motion_test.cpp. BUILD.bazel points <IOKit/...> and <os/log.h> here (host_shims). The work loop runs
// everything at once on the caller's thread; time is a fake clock that IOSleep and IODelay advance. A stand-in used
// wrongly (a detach from another provider, a timer fired that is not armed) panics: the scenario fails.
#ifndef SWITCH_TEST_SWITCH_MOTION_STUBS_H
#define SWITCH_TEST_SWITCH_MOTION_STUBS_H
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;

// Fake time in nanoseconds; nanoseconds_to_absolutetime is the identity.
inline uint64_t gNow = 0;

inline uint64_t mach_absolute_time()
{
    return gNow;
}

inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs)
{
    *abs = ns;
}

inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs;
}

inline void IODelay(unsigned us)
{
    gNow += us * 1000ull;
}

inline void IOSleep(unsigned ms)
{
    gNow += ms * 1000000ull;
}

// IOLog and os_log lines, in order, each prefixed with its channel.
inline std::vector<std::string> gLog;

inline void stubLog(const char *channel, const char *format, va_list ap)
{
    char line[1024];
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
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw Panic(line);
}

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
    mutable int refs = 1;

public:
    virtual ~OSObject() = default;

    virtual void free()
    {}

    // const, as libkern's: a const pointer releases too
    void retain() const
    {
        ++refs;
    }

    void release() const
    {
        if (--refs == 0) {
            const_cast<OSObject *>(this)->free();
            delete this;
        }
    }
};

// SwitchMotion's debug-kernel registry counter (serializeProperties): just enough to compile and call.
class OSSerialize : public OSObject {};

class OSNumber : public OSObject {
public:
    uint64_t value = 0;

    static OSNumber *withNumber(uint64_t number, unsigned)
    {
        auto *n = new OSNumber;
        n->value = number;
        return n;
    }
};

class OSDictionary : public OSObject {
public:
    std::vector<std::pair<std::string, uint64_t>> numbers;

    bool setObject(const char *key, const OSNumber *number)
    {
        numbers.emplace_back(key, number->value);
        return true;
    }

    bool serialize(OSSerialize *) const
    {
        return true;
    }
};

class IOService : public OSObject {
public:
    IOService *provider = nullptr;
    bool registered = false;
    std::string imu;

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

    void detach(IOService *from)
    {
        if (provider != from)
            throw Panic("detach from a provider not attached to");
        provider = nullptr;
        release();
    }

    void registerService()
    {
        registered = true;
    }

    virtual bool serializeProperties(OSSerialize *) const
    {
        return true;
    }

    OSDictionary *dictionaryWithProperties() const
    {
        return new OSDictionary;
    }

    bool setProperty(const char *key, const char *value)
    {
        if (!strcmp(key, "IMU"))
            imu = value;
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
    bool enabled = true;

    void enable()
    {
        enabled = true;
    }

    void disable()
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

class IOInterruptEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *owner, IOInterruptEventSource *sender, int count);
    Action action = nullptr;
    IOService *provider = nullptr;
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

    // The work loop runs the action at once (a disabled source's interrupt waits; none of these tests needs that).
    virtual void interruptOccurred(void *, IOService *, int)
    {
        if (enabled && action)
            action(owner, this, 1);
    }
};

class IOFilterInterruptEventSource : public IOInterruptEventSource {
public:
    typedef bool (*Filter)(OSObject *owner, IOFilterInterruptEventSource *sender);
    Filter filter = nullptr;
    static inline uint64_t filterDelayNS = 0;

    static IOFilterInterruptEventSource *filterInterruptEventSource(OSObject *owner, Action action, Filter filter,
                                                                    IOService *provider = nullptr, int = 0)
    {
        auto *source = new IOFilterInterruptEventSource;
        source->owner = owner;
        source->action = action;
        source->filter = filter;
        source->provider = provider;
        all.push_back(source);
        return source;
    }

    void interruptOccurred(void *, IOService *, int) override
    {
        if (!enabled)
            return;
        if (filter && !filter(owner, this))
            return;
        gNow += filterDelayNS;
        if (action)
            action(owner, this, 1);
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

    void fire()
    {
        if (!armed)
            throw Panic("timer fired while not armed");
        armed = false;
        action(owner, this);
    }
};
#endif
