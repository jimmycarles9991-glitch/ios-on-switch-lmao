// Minimal host stand-ins for compiling SwitchPD.cpp, SwitchPDRequest.cpp and SwitchPDSettings.cpp directly in
// //drivers/SwitchUSB/test without pulling in SwitchBattery's full harness.
#ifndef SWITCH_TEST_SWITCH_PD_NONBLOCKING_STUBS_H
#define SWITCH_TEST_SWITCH_PD_NONBLOCKING_STUBS_H
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using IOReturn = int;
using UInt32 = uint32_t;
using vm_size_t = size_t;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNoResources = 0x2be;
constexpr UInt32 kIOMessageSystemWillPowerOff = 0xe0000250;
constexpr UInt32 kIOMessageSystemWillRestart = 0xe0000310;

inline uint64_t gPdNow = 0;
inline uint64_t gPdSleptMs = 0;
inline uint64_t gPdDelayedUs = 0;
inline std::vector<std::string> gPdLog;

inline uint64_t mach_absolute_time()
{
    return gPdNow;
}

inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs;
}

inline void IODelay(unsigned us)
{
    gPdNow += static_cast<uint64_t>(us) * 1000ULL;
    gPdDelayedUs += us;
}

inline void IOSleep(unsigned ms)
{
    gPdNow += static_cast<uint64_t>(ms) * 1000000ULL;
    gPdSleptMs += ms;
}

inline void pdStubLog(const char *format, va_list ap)
{
    char line[1024];
    vsnprintf(line, sizeof(line), format, ap);
    std::string text(line);
    if (!text.empty() && text.back() == '\n')
        text.pop_back();
    gPdLog.push_back(std::move(text));
}

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, format);
    pdStubLog(format, ap);
    va_end(ap);
}

#define OS_LOG_DEFAULT   nullptr
#define os_log(log, ...) IOLog(__VA_ARGS__)

struct PdStubPanic : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] __attribute__((format(printf, 1, 2))) inline void panic(const char *format, ...)
{
    char line[512];
    va_list ap;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw PdStubPanic(line);
}

struct IOLock {
    bool held = false;
};

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
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    lock->held = false;
}

#define OSDeclareDefaultStructors(cls) \
public:                                \
    cls() = default;
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
    int refs = 1;

public:
    virtual ~OSObject() = default;

    virtual void free()
    {}

    void retain() const
    {
        ++const_cast<OSObject *>(this)->refs;
    }

    void release() const
    {
        auto *self = const_cast<OSObject *>(this);
        if (--self->refs == 0) {
            self->free();
            delete self;
        }
    }
};

class IOWorkLoop;
class IOService;

class IONotifier : public OSObject {};

using IOServiceInterestHandler = IOReturn (*)(void *target, void *refCon, UInt32 messageType, IOService *provider,
                                              void *messageArgument, vm_size_t argSize);

inline std::vector<std::pair<IOServiceInterestHandler, void *>> gPdPowerInterests;

inline IONotifier *registerPrioritySleepWakeInterest(IOServiceInterestHandler handler, void *target, void * = nullptr)
{
    static IONotifier notifier;
    gPdPowerInterests.emplace_back(handler, target);
    return &notifier;
}

class IOService : public OSObject {
public:
    IOService *getPMRootDomain()
    {
        return this;
    }

    virtual IOWorkLoop *getWorkLoop() const
    {
        return nullptr;
    }

    virtual bool init()
    {
        return true;
    }

    bool attach(IOService *)
    {
        retain();
        return true;
    }

    void registerService()
    {}

    void setName(const char *)
    {}

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
    using Action = IOReturn (*)(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);

    static IOWorkLoop *workLoop()
    {
        return new IOWorkLoop;
    }

    IOReturn addEventSource(IOEventSource *)
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
    static inline std::vector<IOInterruptEventSource *> all;

    static IOInterruptEventSource *interruptEventSource(OSObject *owner, Action action, IOService * = nullptr, int = 0)
    {
        auto *source = new IOInterruptEventSource;
        source->owner = owner;
        source->action = action;
        all.push_back(source);
        return source;
    }

    void fire()
    {
        if (enabled)
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
        auto *source = new IOTimerEventSource;
        source->owner = owner;
        source->action = action;
        all.push_back(source);
        return source;
    }

    IOReturn setTimeoutMS(uint32_t timeout)
    {
        armed = true;
        deadline = gPdNow + static_cast<uint64_t>(timeout) * 1000000ULL;
        return kIOReturnSuccess;
    }

    void cancelTimeout()
    {
        armed = false;
    }

    void fire()
    {
        if (!armed)
            throw PdStubPanic("timer fired while not armed");
        armed = false;
        action(owner, this);
    }
};

#endif
