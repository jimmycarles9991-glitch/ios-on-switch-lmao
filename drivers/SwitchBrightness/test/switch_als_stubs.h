// SPDX-License-Identifier: BSD-2-Clause-Patent
// Minimal host stubs for testing drivers/SwitchBrightness/SwitchALS.cpp directly.
#ifndef SWITCH_TEST_SWITCH_ALS_STUBS_H
#define SWITCH_TEST_SWITCH_ALS_STUBS_H
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using IOReturn = int;
using IOOptionBits = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr uint32_t kMicrosecondScale = 1000;
constexpr uint32_t kIOTimerEventSourceOptionsPriorityKernel = 0;
constexpr uint32_t kIOTimeOptionsWithLeeway = 1;

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

[[noreturn]] inline void panic(const char *msg, ...)
{
    std::fprintf(stderr, "PANIC: %s\n", msg);
    std::abort();
}

#define OSDeclareDefaultStructors(cls)
#define OSDefineMetaClassAndStructors(cls, super)
#define OSTypeAlloc(cls) (new cls)
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

struct FakeHIDEvent {
    uintptr_t *vtable;
    void *pad;
    void *data;
    uint32_t type = 0;
    uint64_t timestamp = 0;
    int32_t level = -1;
    int32_t ch0 = -1;
    int32_t ch1 = -1;
    int refs = 1;
};

static_assert(offsetof(FakeHIDEvent, data) == 0x10, "SwitchALS checks _data at +0x10");

struct OSMetaClass {
    static inline unsigned allocs = 0;
    static inline std::vector<FakeHIDEvent *> live;
    static inline uintptr_t eventVtable[32] = {};

    static OSObject *allocClassWithName(const char *name);
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

    void registerService()
    {}

    static IOService *getResourceService()
    {
        static IOService *res = new IOService;
        return res;
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
    using Action = IOReturn (*)(OSObject *target, void *a0, void *a1, void *a2, void *a3);
    static inline IOWorkLoop *last = nullptr;
    static inline unsigned created = 0;
    static inline unsigned runActionCalls = 0;

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
        ++runActionCalls;
        return action(target, a0, a1, a2, a3);
    }
};

class IOInterruptEventSource : public IOEventSource {
public:
    using Action = void (*)(OSObject *owner, IOInterruptEventSource *sender, int count);
    Action action = nullptr;

    static IOInterruptEventSource *interruptEventSource(OSObject *owner, Action action, IOService * = nullptr, int = 0)
    {
        auto *src = new IOInterruptEventSource;
        src->owner = owner;
        src->action = action;
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
    uint64_t lastIntervalUS = 0;
    uint64_t lastLeewayUS = 0;
    static inline std::vector<IOTimerEventSource *> all;

    static IOTimerEventSource *timerEventSource(OSObject *owner, Action action)
    {
        auto *src = new IOTimerEventSource;
        src->owner = owner;
        src->action = action;
        all.push_back(src);
        return src;
    }

    static IOTimerEventSource *timerEventSource(uint32_t, OSObject *owner, Action action)
    {
        return timerEventSource(owner, action);
    }

    IOReturn setTimeoutUS(uint32_t us)
    {
        armed = true;
        lastIntervalUS = us;
        lastLeewayUS = 0;
        return kIOReturnSuccess;
    }

    IOReturn setTimeoutMS(uint32_t ms)
    {
        armed = true;
        lastIntervalUS = static_cast<uint64_t>(ms) * 1000ULL;
        lastLeewayUS = 0;
        return kIOReturnSuccess;
    }

    IOReturn setTimeout(uint32_t, uint64_t intervalAbs, uint64_t leewayAbs)
    {
        armed = true;
        lastIntervalUS = intervalAbs / 1000ULL;
        lastLeewayUS = leewayAbs / 1000ULL;
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
