// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_TEST_SWITCH_BUTTONS_STUBS_H
#define SWITCH_TEST_SWITCH_BUTTONS_STUBS_H
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using IOReturn = int;
using IOOptionBits = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr uint32_t kMillisecondScale = 1000000;

inline uint64_t gNowNS = 1000000000ULL;

inline uint64_t mach_absolute_time()
{
    return gNowNS;
}

inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs;
}

inline void clock_interval_to_absolutetime_interval(uint32_t interval, uint32_t scale, uint64_t *abs)
{
    *abs = static_cast<uint64_t>(interval) * scale;
}

inline void IOLog(const char *, ...)
{}

#define OS_LOG_DEFAULT nullptr
#define os_log(...)    ((void)0)

[[noreturn]] inline void panic(const char *msg, ...)
{
    std::fprintf(stderr, "PANIC: %s\n", msg);
    std::abort();
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

class IOService : public OSObject {
public:
    virtual bool init()
    {
        return true;
    }

    virtual bool start(IOService *)
    {
        return true;
    }

    bool attach(IOService *)
    {
        retain();
        return true;
    }

    void detach(IOService *)
    {
        release();
    }

    static IOService *getResourceService()
    {
        static IOService *res = new IOService;
        return res;
    }
};

class IOEventSource : public OSObject {
public:
    OSObject *owner = nullptr;
    bool enabled = false;

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
    using Action = IOReturn (*)(OSObject *target, void *a0, void *a1, void *a2, void *a3);
    static inline IOWorkLoop *last = nullptr;
    static inline unsigned created = 0;
    static inline unsigned live = 0;

    IOWorkLoop()
    {
        ++live;
    }

    ~IOWorkLoop() override
    {
        --live;
    }

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
    using Action = void (*)(OSObject *owner, IOInterruptEventSource *sender, int count);
    Action action = nullptr;
    IOService *provider = nullptr;
    static inline std::vector<IOInterruptEventSource *> all;

    static IOInterruptEventSource *interruptEventSource(OSObject *owner, Action action, IOService *provider = nullptr,
                                                        int = 0)
    {
        auto *src = new IOInterruptEventSource;
        src->owner = owner;
        src->action = action;
        src->provider = provider;
        all.push_back(src);
        return src;
    }

    void interruptOccurred(void *, IOService *, int)
    {
        if (enabled && action)
            action(owner, this, 1);
    }
};

class IOTimerEventSource : public IOEventSource {
public:
    using Action = void (*)(OSObject *owner, IOTimerEventSource *sender);
    Action action = nullptr;
    bool armed = false;
    uint64_t deadline = 0;
    static inline std::vector<IOTimerEventSource *> all;

    static IOTimerEventSource *timerEventSource(OSObject *owner, Action action)
    {
        auto *src = new IOTimerEventSource;
        src->owner = owner;
        src->action = action;
        all.push_back(src);
        return src;
    }

    IOReturn wakeAtTime(uint64_t abs)
    {
        armed = true;
        deadline = abs;
        return kIOReturnSuccess;
    }

    void cancelTimeout()
    {
        armed = false;
    }

    void fire()
    {
        if (!armed)
            return;
        armed = false;
        action(owner, this);
    }
};

#endif
