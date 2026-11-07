// Host stand-ins for the kernel interfaces drivers/SwitchBattery/SwitchBattery.cpp and drivers/SwitchUSB/SwitchPD.cpp
// use (IOKit, IOPMPowerSource, OSDictionary and friends, IOLock, os_log, mach time), for switch_charging_test.cpp.
// BUILD.bazel points <IOKit/...>, <libkern/...> and <os/log.h> here (host_shims) and compiles each driver as its own
// translation unit. A work loop runs actions at once on the caller's thread; timers keep a deadline on a fake clock
// that the test advances; interrupt sources are fired by the test. A stand-in used wrongly (a lock released that is
// not held, a detach from another provider, a timer fired that is not armed) panics: the scenario fails.
#ifndef SWITCH_TEST_SWITCH_CHARGING_STUBS_H
#define SWITCH_TEST_SWITCH_CHARGING_STUBS_H
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNoResources = 0x2be;
constexpr IOReturn kIOReturnBadArgument = 0x2c2;
constexpr IOReturn kIOReturnUnsupported = 0x2c7;

// Fake time in nanoseconds; the absolute-time conversions are the identity.
inline uint64_t gNow = 0;
inline uint64_t gDelayedUS = 0;  // IODelay total (busy waits)

inline uint64_t mach_absolute_time()
{
    return gNow;
}

inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs;
}

inline void IODelay(unsigned us)
{
    gNow += us * 1000ull;
    gDelayedUS += us;
}

inline void IOSleep(unsigned ms)
{
    gNow += ms * 1000000ull;
}

// IOLog and os_log lines, in order, each prefixed with its channel.
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
    char line[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    throw Panic(line);
}

// IOLock: a plain flag that catches recursion.
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
    if (lock->held)
        throw Panic("IOLock taken twice");
    lock->held = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    if (!lock->held)
        throw Panic("IOLock released while not held");
    lock->held = false;
}

#define OSDeclareDefaultStructors(cls) \
public:                                \
    cls() = default;
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

class OSNumber : public OSObject {
public:
    unsigned long long value = 0;

    static OSNumber *withNumber(unsigned long long v, unsigned)
    {
        auto *n = new OSNumber;
        n->value = v;
        return n;
    }

    uint32_t unsigned32BitValue() const
    {
        return (uint32_t)value;
    }
};

class OSString : public OSObject {
public:
    std::string text;

    static OSString *withCString(const char *s)
    {
        auto *o = new OSString;
        o->text = s;
        return o;
    }
};

class OSSymbol : public OSString {
public:
    static const OSSymbol *withCString(const char *s)
    {
        auto *o = new OSSymbol;
        o->text = s;
        return o;
    }
};

class OSBoolean : public OSObject {
public:
    bool value = false;

    bool getValue() const
    {
        return value;
    }
};

inline OSBoolean *const kOSBooleanTrue = [] {
    auto *b = new OSBoolean;
    b->value = true;
    return b;
}();
inline OSBoolean *const kOSBooleanFalse = new OSBoolean;

class OSDictionary : public OSObject {
public:
    std::map<std::string, long long> numbers;
    std::map<std::string, std::string> strings;
    std::map<std::string, OSObject *> objects;  // as given (not retained: the tests keep them alive)

    static OSDictionary *withCapacity(unsigned)
    {
        return new OSDictionary;
    }

    bool setObject(const char *key, const OSObject *value)
    {
        if (auto *n = dynamic_cast<const OSNumber *>(value))
            numbers[key] = (long long)(int32_t)n->value;
        if (auto *s = dynamic_cast<const OSString *>(value))
            strings[key] = s->text;
        if (auto *b = dynamic_cast<const OSBoolean *>(value))
            numbers[key] = b->value;
        objects[key] = const_cast<OSObject *>(value);
        return true;
    }

    OSObject *getObject(const char *key) const
    {
        auto it = objects.find(key);
        return it == objects.end() ? nullptr : it->second;
    }
};

class IOWorkLoop;

class IOService : public OSObject {
public:
    virtual IOWorkLoop *getWorkLoop() const
    {
        return nullptr;
    }

    virtual IOReturn setProperties(OSObject *)
    {
        return kIOReturnUnsupported;
    }

    IOService *provider = nullptr;
    bool registered = false;
    std::string name;

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

    void setName(const char *n)
    {
        name = n;
    }

    static IOService *getResourceService()
    {
        static IOService *resources = new IOService;
        return resources;
    }
};

// IOPMPowerSource: the properties land in `ps` (numbers) and `psDicts` (the adapter details).
#define kIOPMFullyChargedKey                "FullyCharged"
#define kIOPMPSBatteryTemperatureKey        "Temperature"
#define kIOPMPSDesignCapacityKey            "DesignCapacity"
#define kIOPMPSSerialKey                    "Serial"
#define kIOPMPSAdapterDetailsKey            "AdapterDetails"
#define kIOPMPSAdapterDetailsFamilyKey      "FamilyCode"
#define kIOPMPSAdapterDetailsWattsKey       "Watts"
#define kIOPMPSAdapterDetailsVoltage        "AdapterVoltage"
#define kIOPMPSAdapterDetailsAmperageKey    "Current"
#define kIOPMPSAdapterDetailsDescriptionKey "Description"
constexpr int kIOPSFamilyCodeDisconnected = 0;
constexpr int kIOPSFamilyCodeUSBHost = (int)0xe0004000;
constexpr int kIOPSFamilyCodeUSBChargingPort = (int)0xe0004006;
constexpr int kIOPSFamilyCodeUSBCPD = (int)0xe000400a;

class IOPMPowerSource : public IOService {
public:
    std::map<std::string, long long> ps;
    std::map<std::string, std::string> strings;
    OSDictionary adapter;
    std::map<std::string, OSDictionary> dicts;  // every dictionary property by key (AdapterDetails, ChargerData)
    bool charging = false, external = false;
    unsigned capacity = 0, updates = 0;

    bool start(IOService *) override
    {
        return true;
    }

    void setBatteryInstalled(bool)
    {}

    void setExternalConnected(bool v)
    {
        external = v;
    }

    void setExternalChargeCapable(bool)
    {}

    void setIsCharging(bool v)
    {
        charging = v;
    }

    void setAtCriticalLevel(bool)
    {}

    void setCurrentCapacity(unsigned v)
    {
        capacity = v;
    }

    void setMaxCapacity(unsigned)
    {}

    void setVoltage(unsigned v)
    {
        ps["Voltage"] = v;
    }

    // The published booleans by their IOPMPowerSource keys, as powerd reads them.
    long long isChargingKey() const
    {
        return charging;
    }

    void setAmperage(int v)
    {
        ps["Amperage"] = v;
    }

    void setCycleCount(unsigned v)
    {
        ps["CycleCount"] = v;
    }

    void setPSProperty(const OSSymbol *key, OSObject *value)
    {
        if (auto *n = dynamic_cast<OSNumber *>(value))
            ps[key->text] = (long long)(int32_t)n->value;
        if (auto *b = dynamic_cast<OSBoolean *>(value))
            ps[key->text] = b->value;
        if (auto *t = dynamic_cast<OSString *>(value))
            strings[key->text] = t->text;
        if (auto *d = dynamic_cast<OSDictionary *>(value)) {
            dicts[key->text] = *d;
            if (key->text == kIOPMPSAdapterDetailsKey)
                adapter = *d;
        }
    }

    void updateStatus()
    {
        ++updates;
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

    static IOWorkLoop *workLoop()
    {
        return new IOWorkLoop;
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

    void interruptOccurred()
    {
        if (enabled)
            action(owner, this, 1);
    }
};

class IOTimerEventSource : public IOEventSource {
public:
    typedef void (*Action)(OSObject *owner, IOTimerEventSource *sender);
    Action action = nullptr;
    bool armed = false;
    uint64_t deadline = 0;
    unsigned index = 0;  // creation order
    static inline std::vector<IOTimerEventSource *> all;

    static IOTimerEventSource *timerEventSource(OSObject *owner, Action action)
    {
        auto *source = new IOTimerEventSource;
        source->owner = owner;
        source->action = action;
        source->index = (unsigned)all.size();
        all.push_back(source);
        return source;
    }

    IOReturn setTimeoutMS(uint32_t timeout)
    {
        armed = true;
        deadline = gNow + timeout * 1000000ull;
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
