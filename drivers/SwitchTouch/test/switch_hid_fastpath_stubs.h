// SPDX-License-Identifier: BSD-2-Clause-Patent
// Minimal host stubs for testing drivers/SwitchTouch/SwitchHIDFastPath.cpp directly.
#ifndef SWITCH_TEST_SWITCH_HID_FASTPATH_STUBS_H
#define SWITCH_TEST_SWITCH_HID_FASTPATH_STUBS_H
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using IOReturn = int;
using IOOptionBits = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0;

struct IOLock {
    int locked = 0;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *l)
{
    delete l;
}

inline void IOLockLock(IOLock *l)
{
    ++l->locked;
}

inline void IOLockUnlock(IOLock *l)
{
    --l->locked;
}

inline void *IOMalloc(size_t size)
{
    return std::malloc(size);
}

inline void IOFree(void *ptr, size_t)
{
    std::free(ptr);
}

inline void IOLog(const char *, ...)
{}

class OSObject {
    mutable int refs = 1;

public:
    virtual ~OSObject() = default;

    void retain() const
    {
        ++refs;
    }

    void release() const
    {
        if (--refs == 0)
            delete this;
    }

    int refCount() const
    {
        return refs;
    }
};

#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
#define OSSafeReleaseNULL(obj) \
    do {                       \
        if (obj) {             \
            (obj)->release();  \
            (obj) = nullptr;   \
        }                      \
    } while (0)

class OSBoolean : public OSObject {
    bool v;

public:
    explicit OSBoolean(bool val) : v(val)
    {}

    bool isTrue() const
    {
        return v;
    }
};

class OSNumber : public OSObject {
    uint64_t v;

public:
    explicit OSNumber(uint64_t val) : v(val)
    {}

    static OSNumber *withNumber(uint64_t val, unsigned)
    {
        return new OSNumber(val);
    }

    uint32_t unsigned32BitValue() const
    {
        return static_cast<uint32_t>(v);
    }
};

class OSString : public OSObject {
    std::string s;

public:
    explicit OSString(const char *str) : s(str ? str : "")
    {}

    static OSString *withCString(const char *str)
    {
        return new OSString(str);
    }

    const char *getCStringNoCopy() const
    {
        return s.c_str();
    }
};

class OSDictionary : public OSObject {
    std::map<std::string, const OSObject *> entries;

public:
    ~OSDictionary() override
    {
        for (auto &[k, v] : entries)
            v->release();
    }

    static OSDictionary *withCapacity(unsigned)
    {
        return new OSDictionary;
    }

    static OSDictionary *withDictionary(const OSDictionary *other, unsigned)
    {
        auto *d = new OSDictionary;
        if (other) {
            for (const auto &[k, v] : other->entries)
                d->setObject(k.c_str(), v);
        }
        return d;
    }

    unsigned getCount() const
    {
        return static_cast<unsigned>(entries.size());
    }

    bool setObject(const char *key, const OSObject *obj)
    {
        if (!key || !obj)
            return false;
        obj->retain();
        auto it = entries.find(key);
        if (it != entries.end()) {
            it->second->release();
            it->second = obj;
        } else {
            entries.emplace(key, obj);
        }
        return true;
    }

    OSObject *getObject(const char *key) const
    {
        auto it = entries.find(key);
        return it == entries.end() ? nullptr : const_cast<OSObject *>(it->second);
    }
};

class IONotifier : public OSObject {};

class IOService;
using IOServiceMatchingNotificationHandler = bool (*)(void *target, void *refCon, IOService *newService,
                                                      IONotifier *notifier);

inline const char *gIOFirstPublishNotification = "IOFirstPublish";

class IOService : public OSObject {
    IOService *provider = nullptr;
    std::map<std::string, const OSObject *> props;

public:
    static inline IOServiceMatchingNotificationHandler gHandler = nullptr;
    static inline void *gTarget = nullptr;
    static inline void *gRefCon = nullptr;

    ~IOService() override
    {
        for (auto &[k, v] : props)
            v->release();
    }

    void setProvider(IOService *p)
    {
        provider = p;
    }

    IOService *getProvider() const
    {
        return provider;
    }

    bool setProperty(const char *key, const OSObject *val)
    {
        if (!key || !val)
            return false;
        val->retain();
        auto it = props.find(key);
        if (it != props.end()) {
            it->second->release();
            it->second = val;
        } else {
            props.emplace(key, val);
        }
        return true;
    }

    bool setProperty(const char *key, const char *val)
    {
        const OSString *s = OSString::withCString(val);
        const bool ok = setProperty(key, s);
        OSSafeReleaseNULL(s);
        return ok;
    }

    OSObject *getProperty(const char *key) const
    {
        auto it = props.find(key);
        return it == props.end() ? nullptr : const_cast<OSObject *>(it->second);
    }

    static OSDictionary *serviceMatching(const char *)
    {
        return OSDictionary::withCapacity(1);
    }

    static IONotifier *addMatchingNotification(const char *, OSDictionary *, IOServiceMatchingNotificationHandler fn,
                                               void *target, void *refCon = nullptr)
    {
        gHandler = fn;
        gTarget = target;
        gRefCon = refCon;
        return new IONotifier;
    }
};

#endif
