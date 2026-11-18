// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class OSObject {
public:
    virtual ~OSObject() = default;

    virtual void release() const
    {
        ++releases;
    }

    mutable unsigned releases = 0;

    virtual const OSObject *metaCast(const char *) const
    {
        return nullptr;
    }
};

class OSData : public OSObject {
public:
    explicit OSData(std::vector<unsigned char> value) : bytes(std::move(value))
    {}

    unsigned getLength() const
    {
        return static_cast<unsigned>(bytes.size());
    }

    const void *getBytesNoCopy() const
    {
        return bytes.data();
    }

private:
    std::vector<unsigned char> bytes;
};

class OSNumber : public OSObject {
public:
    explicit OSNumber(uint32_t value) : number(value)
    {}

    uint32_t unsigned32BitValue() const
    {
        return number;
    }

private:
    uint32_t number;
};

class OSString : public OSObject {};

#define OSDynamicCast(type, object) dynamic_cast<type *>(const_cast<OSObject *>(static_cast<const OSObject *>(object)))

inline constexpr int gIODTPlane = 1;
inline constexpr int gIOServicePlane = 2;

class OSIterator : public OSObject {
public:
    explicit OSIterator(std::vector<std::shared_ptr<OSObject>> objects) : values(std::move(objects))
    {}

    void release() const override
    {
        delete this;
    }

    OSObject *getNextObject()
    {
        return index < values.size() ? values[index++].get() : nullptr;
    }

private:
    std::vector<std::shared_ptr<OSObject>> values;
    size_t index = 0;
};

class IORegistryEntry : public OSObject {
public:
    OSObject *getProperty(const char *key) const
    {
        auto found = properties.find(key);
        return found == properties.end() ? nullptr : found->second.get();
    }

    OSObject *copyProperty(const char *key) const
    {
        auto found = properties.find(key);
        return found == properties.end() ? nullptr : found->second.get();
    }

    IORegistryEntry *childFromPath(const char *path, int plane)
    {
        auto found = nodes.find({plane, path});
        return found == nodes.end() ? nullptr : found->second.get();
    }

    OSIterator *getChildIterator(int plane) const
    {
        return plane == gIOServicePlane && !fail_iterator ? new OSIterator(children) : nullptr;
    }

    std::map<std::string, std::shared_ptr<OSObject>> properties;
    std::map<std::pair<int, std::string>, std::shared_ptr<IORegistryEntry>> nodes;
    std::vector<std::shared_ptr<OSObject>> children;
    bool fail_iterator = false;
};

class IOService : public IORegistryEntry {
public:
    bool isInactive() const
    {
        return inactive;
    }

    IOService *getProvider() const
    {
        return provider;
    }

    bool inactive = false;
    IOService *provider = nullptr;
};

class IODTNVRAM : public IOService {};

class SwitchTestNVRAMDiags : public IOService {
public:
    const OSObject *metaCast(const char *name) const override
    {
        return !strcmp(name, "IODTNVRAMDiags") ? this : nullptr;
    }
};
